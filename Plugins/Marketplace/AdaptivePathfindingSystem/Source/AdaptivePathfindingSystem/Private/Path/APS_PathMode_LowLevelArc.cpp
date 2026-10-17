/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Path/APS_PathMode_LowLevelArc.h"

#include "Core/APS_EngineCompatibility.h"

#include "Path/APS_PathMode_LowLevelBase.h"
#include "Navigation/APS_NavigationData.h"
#include "Navigation/APS_MoveToInterface.h"
#include "Debug/APS_DebugColorUtils.h"
#include "Debug/APS_DebugOverlaySubsystem.h"
#include "Debug/APS_NavLogs.h"
#include "Visualization/APS_Visualizer.h"

#include "NavMesh/RecastNavMesh.h"
#include "NavMesh/RecastHelpers.h"
#include "NavMesh/RecastQueryFilter.h"
#include "NavMesh/NavMeshPath.h"

#include "Detour/DetourNavMesh.h"
#include "Detour/DetourNavMeshQuery.h"
#include "Detour/APS_DetourNode.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"

#include "HAL/IConsoleManager.h"
#include "Runtime/Launch/Resources/Version.h"

#if WITH_RECAST

using FAPSNavMeshQuery = FAPSPathModeLowLevelBase::FAPSNavMeshQuery;

namespace
{
	constexpr float GOffsetScales[] = { 1.0f, 0.875f, 0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f };
	constexpr float GManeuverZoneMinWeightStrength = 0.1f;
	// Absolute fallback arc widths (UU) tried when relative-scale candidates all fail on long paths.
	// Only candidates smaller than BaseAmp * GOffsetScales[last] are used to avoid duplicate tests.
	constexpr float GArcAbsFallbackAmps[] = { 600.0f, 450.0f, 300.0f, 200.0f, 120.0f, 80.0f };

	// Activation gates for ManeuverZone side-route injection (large LaneOffset cases).
	// All three criteria must be met for a zone to trigger a side-route injection attempt.
	constexpr float GManeuverSideRouteMinLaneOffset     = 180.0f; // abs(LaneOffset) floor.
	constexpr float GManeuverSideRouteMinRequestedWidth = 130.0f; // Zone.RequestedWidth floor.
	constexpr float GManeuverSideRouteMinStrength       = 0.32f;  // Zone.Strength floor.
	constexpr int32 GManeuverSideRouteMaxInjectedPts    = 24;     // Max points per injected route segment.
	constexpr int32 GManeuverSideRouteBasePathCap       = 96;     // Max total base-path points after injection.
	// Maximum allowed local-path length as a multiple of the straight zone span, plus a flat budget.
	// Prevents accepting side routes that go the long way around (e.g. off-map shortcuts).
	constexpr float GManeuverSideRouteMaxLengthRatio    = 5.0f;   // max = DirectLen * ratio + offset
	constexpr float GManeuverSideRouteMaxLengthOffset   = 150.0f; // additive flat budget (UE units).
	constexpr int32 GManeuverSideRouteMaxFailedDebugOverlays = 32;
	constexpr int32 GManeuverSideRouteMaxCandidateDebugOverlays = 48;
	constexpr int32 GManeuverSideRouteMaxSideTries = 2;
	constexpr float GDirectCornerStrengthMin = 0.10f;
	constexpr float GDirectCornerBlockedAnchorDistanceScale = 0.90f;
	constexpr float GDirectCornerBlockedAnchorDistanceMin = 120.0f;
	constexpr float GDirectCornerBlockedAnchorDistanceMax = 520.0f;
	constexpr float GSyntheticFallbackStartRatio = 0.35f;
	constexpr float GSyntheticFallbackPeakRatio = 0.50f;
	constexpr float GSyntheticFallbackEndRatio = 0.65f;
	constexpr float GMinPathPointDistance2D = 1.0f;
	// Area-cost comparison tolerance shared by side-route injection and WarpPathToArc.
	constexpr float GNavAreaCostTolerance               = 0.001f;

	enum class EAPSManeuverZoneType : uint8
	{
		XYTurn,
		Slope,
		Mixed
	};

	struct FAPSManeuverZone
	{
		float StartDistance = 0.0f;
		float PeakDistance = 0.0f;
		float EndDistance = 0.0f;
		float Strength = 0.0f;
		float RequestedWidth = 0.0f;
		FVector SideDir = FVector::ZeroVector;
		EAPSManeuverZoneType Type = EAPSManeuverZoneType::XYTurn;
		bool bFromDirectCorner = false;
		bool bFromSyntheticFallback = false;
	};

	struct FAPSSideRouteDebugInfo
	{
		FVector ZoneStartLoc = FVector::ZeroVector;
		FVector ZonePeakLoc = FVector::ZeroVector;
		FVector ZoneEndLoc = FVector::ZeroVector;
		FVector SideWaypoint = FVector::ZeroVector;
		TArray<FVector> InjectedPoints;
		float CandidateWidth = 0.0f;
		EAPSManeuverZoneType ZoneType = EAPSManeuverZoneType::XYTurn;
		bool bInjected = false;
		bool bCandidateOnly = false;
		bool bTriedOppositeSide = false;
		bool bFromDirectCorner = false;
		bool bFromSyntheticFallback = false;
		FString FailureReason;
	};

	struct FAPSSideRouteStats
	{
		int32 CandidateCount = 0;
		int32 AttemptedCount = 0;
		int32 InjectedCount = 0;
		int32 FailedCount = 0;
	};

	static float OverlayPointSizeFromWorldRadius(const float Radius)
	{
		return FMath::Clamp(Radius * 0.25f, 4.0f, 14.0f);
	}

	static bool DrawSideRouteDebugOverlay(
		UWorld* World,
		const TArray<FAPSSideRouteDebugInfo>& DebugInfos,
		const FAPSDebugPathColorProfile& ColorProfile,
		float LifeTime,
		float AnchorRadius,
		float SideWaypointRadius,
		float InjectedRadius,
		float FailedRadius,
		float CandidateRadius,
		float AnchorLineThickness,
		float InjectedLineThickness,
		float FailedLineThickness,
		float CandidateLineThickness)
	{
		if (!World)
		{
			return false;
		}

		UAPS_DebugOverlaySubsystem* OverlaySubsystem = World->GetSubsystem<UAPS_DebugOverlaySubsystem>();
		if (!OverlaySubsystem)
		{
			return false;
		}

		OverlaySubsystem->EnsureWidget();
		if (!OverlaySubsystem->HasWidget())
		{
			return false;
		}

		const FLinearColor AnchorColor = ColorProfile.InjectedAnchor;
		const FLinearColor PeakColor = ColorProfile.InjectedPeak;
		const FLinearColor SideWaypointColor = ColorProfile.SideWaypoint;
		const FLinearColor InjectedColor = ColorProfile.InjectedRoute;
		const FLinearColor AnchorLinkColor = ColorProfile.AnchorLink;
		const FLinearColor FailedWaypointColor = ColorProfile.FailedWaypoint;
		const FLinearColor FailedAnchorColor = ColorProfile.FailedAnchor;
		const FLinearColor FailedLinkColor = ColorProfile.FailedLine;
		const FLinearColor CandidateAnchorColor = ColorProfile.CandidateAnchor;
		const FLinearColor CandidatePeakColor = ColorProfile.CandidatePeak;
		const FLinearColor CandidateLineColor = ColorProfile.CandidateLine;

		for (const FAPSSideRouteDebugInfo& Info : DebugInfos)
		{
			if (Info.bCandidateOnly)
			{
				OverlaySubsystem->AddPoint(Info.ZoneStartLoc, CandidateAnchorColor, OverlayPointSizeFromWorldRadius(CandidateRadius), LifeTime);
				OverlaySubsystem->AddPoint(Info.ZonePeakLoc, CandidatePeakColor, OverlayPointSizeFromWorldRadius(CandidateRadius * 1.1f), LifeTime);
				OverlaySubsystem->AddPoint(Info.ZoneEndLoc, CandidateAnchorColor, OverlayPointSizeFromWorldRadius(CandidateRadius), LifeTime);
				OverlaySubsystem->AddLine(Info.ZoneStartLoc, Info.ZonePeakLoc, CandidateLineColor, CandidateLineThickness, LifeTime);
				OverlaySubsystem->AddLine(Info.ZonePeakLoc, Info.ZoneEndLoc, CandidateLineColor, CandidateLineThickness, LifeTime);
			}
			else if (Info.bInjected)
			{
				OverlaySubsystem->AddPoint(Info.ZoneStartLoc, AnchorColor, OverlayPointSizeFromWorldRadius(AnchorRadius), LifeTime);
				OverlaySubsystem->AddPoint(Info.ZonePeakLoc, PeakColor, OverlayPointSizeFromWorldRadius(AnchorRadius), LifeTime);
				OverlaySubsystem->AddPoint(Info.ZoneEndLoc, AnchorColor, OverlayPointSizeFromWorldRadius(AnchorRadius), LifeTime);
				OverlaySubsystem->AddPoint(Info.SideWaypoint, SideWaypointColor, OverlayPointSizeFromWorldRadius(SideWaypointRadius), LifeTime);
				OverlaySubsystem->AddLine(Info.ZonePeakLoc, Info.SideWaypoint, AnchorLinkColor, AnchorLineThickness, LifeTime);
				OverlaySubsystem->AddPolyline(Info.InjectedPoints, InjectedColor, InjectedLineThickness, OverlayPointSizeFromWorldRadius(InjectedRadius), LifeTime);
			}
			else
			{
				OverlaySubsystem->AddPoint(Info.ZoneStartLoc, FailedAnchorColor, OverlayPointSizeFromWorldRadius(AnchorRadius * 0.8f), LifeTime);
				OverlaySubsystem->AddPoint(Info.ZonePeakLoc, FailedAnchorColor, OverlayPointSizeFromWorldRadius(AnchorRadius * 0.8f), LifeTime);
				OverlaySubsystem->AddPoint(Info.ZoneEndLoc, FailedAnchorColor, OverlayPointSizeFromWorldRadius(AnchorRadius * 0.8f), LifeTime);
				OverlaySubsystem->AddPoint(Info.SideWaypoint, FailedWaypointColor, OverlayPointSizeFromWorldRadius(FailedRadius), LifeTime);
				OverlaySubsystem->AddLine(Info.ZonePeakLoc, Info.SideWaypoint, FailedLinkColor, FailedLineThickness, LifeTime);
			}
		}

		return true;
	}

	static void DrawSideRouteDebugFallback(
		UWorld* World,
		const TArray<FAPSSideRouteDebugInfo>& DebugInfos,
		const FAPSDebugPathColorProfile& ColorProfile,
		float LifeTime,
		float AnchorRadius,
		float SideWaypointRadius,
		float InjectedRadius,
		float FailedRadius,
		float CandidateRadius,
		float AnchorLineThickness,
		float InjectedLineThickness,
		float FailedLineThickness,
		float CandidateLineThickness,
		uint8 DepthPriority)
	{
		if (!World)
		{
			return;
		}

		const FColor AnchorColor = ColorProfile.InjectedAnchor.ToFColor(true);
		const FColor PeakColor = ColorProfile.InjectedPeak.ToFColor(true);
		const FColor SideWaypointColor = ColorProfile.SideWaypoint.ToFColor(true);
		const FColor InjectedColor = ColorProfile.InjectedRoute.ToFColor(true);
		const FColor AnchorLinkColor = ColorProfile.AnchorLink.ToFColor(true);
		const FColor FailedWaypointColor = ColorProfile.FailedWaypoint.ToFColor(true);
		const FColor FailedAnchorColor = ColorProfile.FailedAnchor.ToFColor(true);
		const FColor FailedLinkColor = ColorProfile.FailedLine.ToFColor(true);
		const FColor CandidateAnchorColor = ColorProfile.CandidateAnchor.ToFColor(true);
		const FColor CandidatePeakColor = ColorProfile.CandidatePeak.ToFColor(true);
		const FColor CandidateLineColor = ColorProfile.CandidateLine.ToFColor(true);

		for (const FAPSSideRouteDebugInfo& Info : DebugInfos)
		{
			if (Info.bCandidateOnly)
			{
				DrawDebugPoint(World, Info.ZoneStartLoc, CandidateRadius, CandidateAnchorColor, false, LifeTime, DepthPriority);
				DrawDebugPoint(World, Info.ZonePeakLoc, CandidateRadius * 1.1f, CandidatePeakColor, false, LifeTime, DepthPriority);
				DrawDebugPoint(World, Info.ZoneEndLoc, CandidateRadius, CandidateAnchorColor, false, LifeTime, DepthPriority);
				DrawDebugLine(World, Info.ZoneStartLoc, Info.ZonePeakLoc, CandidateLineColor, false, LifeTime, DepthPriority, CandidateLineThickness);
				DrawDebugLine(World, Info.ZonePeakLoc, Info.ZoneEndLoc, CandidateLineColor, false, LifeTime, DepthPriority, CandidateLineThickness);
			}
			else if (Info.bInjected)
			{
				DrawDebugPoint(World, Info.ZoneStartLoc, AnchorRadius, AnchorColor, false, LifeTime, DepthPriority);
				DrawDebugPoint(World, Info.ZonePeakLoc, AnchorRadius, PeakColor, false, LifeTime, DepthPriority);
				DrawDebugPoint(World, Info.ZoneEndLoc, AnchorRadius, AnchorColor, false, LifeTime, DepthPriority);
				DrawDebugPoint(World, Info.SideWaypoint, SideWaypointRadius, SideWaypointColor, false, LifeTime, DepthPriority);
				DrawDebugLine(World, Info.ZonePeakLoc, Info.SideWaypoint, AnchorLinkColor, false, LifeTime, DepthPriority, AnchorLineThickness);

				for (int32 PointIdx = 0; PointIdx < Info.InjectedPoints.Num(); ++PointIdx)
				{
					DrawDebugPoint(World, Info.InjectedPoints[PointIdx], InjectedRadius, InjectedColor, false, LifeTime, DepthPriority);
					if (PointIdx > 0)
					{
						DrawDebugLine(
							World,
							Info.InjectedPoints[PointIdx - 1],
							Info.InjectedPoints[PointIdx],
							InjectedColor,
							false,
							LifeTime,
							DepthPriority,
							InjectedLineThickness);
					}
				}
			}
			else
			{
				DrawDebugPoint(World, Info.ZoneStartLoc, AnchorRadius * 0.8f, FailedAnchorColor, false, LifeTime, DepthPriority);
				DrawDebugPoint(World, Info.ZonePeakLoc, AnchorRadius * 0.8f, FailedAnchorColor, false, LifeTime, DepthPriority);
				DrawDebugPoint(World, Info.ZoneEndLoc, AnchorRadius * 0.8f, FailedAnchorColor, false, LifeTime, DepthPriority);
				DrawDebugPoint(World, Info.SideWaypoint, FailedRadius, FailedWaypointColor, false, LifeTime, DepthPriority);
				DrawDebugLine(World, Info.ZonePeakLoc, Info.SideWaypoint, FailedLinkColor, false, LifeTime, DepthPriority, FailedLineThickness);
			}
		}
	}

	/** 
	 * Draws the final nav path using UAPS_Visualizer.
	 * This is called only in debug builds / when you want path visualization.
	 */
	static void DebugDrawPathIfNeeded(
	const AAPS_NavigationData*   NavData,
	const FPathFindingQuery&     Query,
	const TArray<FNavPathPoint>& PathPoints)
	{
		// 1) Nothing to draw if there are no points at all.
		if (PathPoints.Num() == 0)
		{
			return;
		}

		// 2) Read the current mode from our registered CVar.
		//    0 = off, 1 = simple lines, 2 = dynamic (lines + points)
		const int32 PathMode = CVarAPSPathDebugMode.GetValueOnAnyThread();
		if (PathMode <= 0)
		{
			// Off by default – nothing will be drawn until the user
			// explicitly enables it via CVar.
			return;
		}

		// 3) Collect world-space locations from nav path points.
		TArray<FVector> Locations;
		Locations.Reserve(PathPoints.Num());
		for (const FNavPathPoint& P : PathPoints)
		{
			Locations.Add(P.Location);
		}

		// 4) Pick a reasonable world context – query owner if possible,
		//    otherwise fall back to NavData.
		const UObject* WorldContextObject = nullptr;

		if (const UObject* OwnerObj = Query.Owner.Get())
		{
			WorldContextObject = OwnerObj;
		}
		else
		{
			WorldContextObject = NavData;
		}

		if (!WorldContextObject)
		{
			return;
		}

		// 5) Choose visualization style based on PathMode.
		float SegmentThickness = 4.0f;
		float SphereRadius     = 0.0f;
		float LifeTime         = 5.0f;

		switch (PathMode)
		{
		case 1: // Simple
			SegmentThickness = 4.0f;
			SphereRadius     = 0.0f;   // lines only
			break;

		case 2: // Dynamic
			SegmentThickness = 3.0f;
			SphereRadius     = 30.0f;  // lines + points
			break;

		default:
			// Unknown mode – treat as off to be safe.
			return;
		}

		// 6) Finally, draw the path using our visualizer object.
		if (UAPS_Visualizer* Visualizer = NewObject<UAPS_Visualizer>(GetTransientPackage()))
		{
			Visualizer->VisualizePath(
				WorldContextObject,
				Locations,
				SegmentThickness,
				SphereRadius,
				LifeTime);
		}
	}

	static void DebugDrawSideRouteInjectionsIfNeeded(
		const AAPS_NavigationData*                NavData,
		const FPathFindingQuery&                  Query,
		const TArray<FNavPathPoint>&              PathPoints,
		const TArray<FAPSSideRouteDebugInfo>&     DebugInfos)
	{
		if (DebugInfos.Num() == 0)
		{
			return;
		}

		if (CVarAPSNavDebug.GetValueOnAnyThread() == 0 ||
			CVarAPSPathDebugMode.GetValueOnAnyThread() < 2)
		{
			return;
		}

		const UObject* WorldContextObject = nullptr;
		if (const UObject* OwnerObj = Query.Owner.Get())
		{
			WorldContextObject = OwnerObj;
		}
		else
		{
			WorldContextObject = NavData;
		}

		if (!WorldContextObject || !GEngine)
		{
			return;
		}

		UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
		if (!World)
		{
			return;
		}

		TArray<FVector> PathLocations;
		PathLocations.Reserve(PathPoints.Num());
		for (const FNavPathPoint& PathPoint : PathPoints)
		{
			PathLocations.Add(PathPoint.Location);
		}

		const FAPSDebugPathColorProfile ColorProfile = MakeAPSDebugPathColorProfile(WorldContextObject, PathLocations);

		AsyncTask(ENamedThreads::GameThread, [World, LocalInfos = TArray<FAPSSideRouteDebugInfo>(DebugInfos), ColorProfile]()
		{
			const float LifeTime = 5.0f;
			const float AnchorRadius = 20.0f;
			const float SideWaypointRadius = 35.0f;
			const float InjectedRadius = 30.0f;
			const float FailedRadius = 24.0f;
			const float CandidateRadius = 14.0f;
			const float AnchorLineThickness = 1.75f;
			const float InjectedLineThickness = 4.0f;
			const float FailedLineThickness = 1.5f;
			const float CandidateLineThickness = 1.0f;
			const uint8 DepthPriority = 0;

			if (DrawSideRouteDebugOverlay(
				World,
				LocalInfos,
				ColorProfile,
				LifeTime,
				AnchorRadius,
				SideWaypointRadius,
				InjectedRadius,
				FailedRadius,
				CandidateRadius,
				AnchorLineThickness,
				InjectedLineThickness,
				FailedLineThickness,
				CandidateLineThickness))
			{
				return;
			}

			DrawSideRouteDebugFallback(
				World,
				LocalInfos,
				ColorProfile,
				LifeTime,
				AnchorRadius,
				SideWaypointRadius,
				InjectedRadius,
				FailedRadius,
				CandidateRadius,
				AnchorLineThickness,
				InjectedLineThickness,
				FailedLineThickness,
				CandidateLineThickness,
				DepthPriority);
		});
	}


	// --------------------------------------------------------------------
	// Small tuning helpers (CVars etc.)
	// --------------------------------------------------------------------

	static float GetMinArcDistance()
	{
		static const auto* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("aps.Arc.MinDistance"));
		return CVar ? CVar->GetFloat() : 300.0f;
	}

	static int32 EstimateNodesForDistance(const float Dist2D)
	{
		if (Dist2D < 1000.0f)  return 256;
		if (Dist2D < 3000.0f)  return 512;
		if (Dist2D < 8000.0f)  return 1024;
		return 2048;
	}

	// --------------------------------------------------------------------
	// Owner / context helpers
	// --------------------------------------------------------------------

	/** Try to get a sensible actor from the path query owner. */
	static AActor* GetQueryActor(const FPathFindingQuery& Query)
	{
		const UObject* OwnerObj = Query.Owner.Get();
		if (!OwnerObj)
		{
			return nullptr;
		}

		if (const APawn* AsPawn = Cast<APawn>(OwnerObj))
		{
			return const_cast<APawn*>(AsPawn);
		}

		if (const AController* AsController = Cast<AController>(OwnerObj))
		{
			if (APawn* Pawn = AsController->GetPawn())
			{
				return Pawn;
			}

			return const_cast<AController*>(AsController);
		}

		if (const AActor* AsActor = Cast<AActor>(OwnerObj))
		{
			return const_cast<AActor*>(AsActor);
		}

		return nullptr;
	}

	inline void UnrealExtentToDetour(const FVector& UEExtent, float* OutExtent)
	{
		OutExtent[0] = UEExtent.X;
		OutExtent[1] = UEExtent.Z;
		OutExtent[2] = UEExtent.Y;
	}

	// --------------------------------------------------------------------
	// Thin wrappers over Detour
	// --------------------------------------------------------------------

	static bool FindNearestPoly(
		dtNavMeshQuery*           Query,
		const dtQueryFilter*      Filter,
		const float               Extent[3],
		const FVector&            WorldLocation,
		dtPolyRef&                OutRef,
		float                     OutPos[3])
	{
		if (!Query || !Filter)
		{
			return false;
		}

		const FVector RcPoint = Unreal2RecastPoint(WorldLocation);

#if ENGINE_MAJOR_VERSION >= 5
		dtReal DtExtent[3] =
		{
			(dtReal)Extent[0],
			(dtReal)Extent[1],
			(dtReal)Extent[2],
		};

		dtReal DtNearest[3];

		const dtStatus Status = Query->findNearestPoly(
			reinterpret_cast<const dtReal*>(&RcPoint.X),
			DtExtent,
			Filter,
			&OutRef,
			DtNearest);

		if (!dtStatusSucceed(Status) || OutRef == 0)
		{
			return false;
		}

		OutPos[0] = (float)DtNearest[0];
		OutPos[1] = (float)DtNearest[1];
		OutPos[2] = (float)DtNearest[2];
		return true;
#else
		const dtStatus Status = Query->findNearestPoly(
			&RcPoint.X,
			Extent,
			Filter,
			&OutRef,
			OutPos);

		return dtStatusSucceed(Status) && OutRef != 0;
#endif
	}

	/** Simple navmesh line-of-sight between two points on the same poly. */
	static bool HasNavmeshLineOfSight(
		dtNavMeshQuery*      Query,
		const dtQueryFilter* Filter,
		dtPolyRef            StartRef,
		const float          StartPos[3],
		const float          EndPos[3])
	{
		if (!Query || !Filter || StartRef == 0)
		{
			return false;
		}

#if ENGINE_MAJOR_VERSION >= 5
		dtReal DtStart[3] =
		{
			(dtReal)StartPos[0],
			(dtReal)StartPos[1],
			(dtReal)StartPos[2],
		};

		dtReal DtEnd[3] =
		{
			(dtReal)EndPos[0],
			(dtReal)EndPos[1],
			(dtReal)EndPos[2],
		};

		dtReal   HitNormal[3];
		dtPolyRef RayPath[8];
		int      RayPathCount = 0;
		dtReal   T             = (dtReal)0;

		const dtStatus RayStatus = Query->raycast(
			StartRef,
			DtStart,
			DtEnd,
			Filter,
			&T,
			HitNormal,
			RayPath,
			&RayPathCount,
			8);

		return dtStatusSucceed(RayStatus) && T >= (dtReal)0.999;
#else
		float     HitNormal[3];
		dtPolyRef RayPath[8];
		int       RayPathCount = 0;
		float     T             = 0.0f;

		const dtStatus RayStatus = Query->raycast(
			StartRef,
			StartPos,
			EndPos,
			Filter,
			&T,
			HitNormal,
			RayPath,
			&RayPathCount,
			8);

		// If raycast runs all the way to the end point, we treat it as LOS.
		return dtStatusSucceed(RayStatus) && T >= 0.999f;
#endif
	}

	static bool TryGetAreaCostFromPolyRef(
		dtNavMeshQuery*      Query,
		const dtQueryFilter* Filter,
		dtPolyRef            PolyRef,
		float&               OutAreaCost)
	{
		constexpr dtPolyRef InvalidPolyRef = 0; // Detour invalid ref sentinel.
		OutAreaCost = 0.0f;
		if (!Query || !Filter || PolyRef == InvalidPolyRef)
		{
			return false;
		}

		const dtNavMesh* NavMesh = Query->getAttachedNavMesh();
		if (!NavMesh)
		{
			return false;
		}

		const dtMeshTile* Tile = nullptr;
		const dtPoly*     Poly = nullptr;
		const dtStatus    Status = NavMesh->getTileAndPolyByRef(PolyRef, &Tile, &Poly);
		if (dtStatusFailed(Status) || !Poly)
		{
			return false;
		}

		OutAreaCost = Filter->getAreaCost(Poly->getArea());
		return true;
	}

	static bool BuildStraightPathFromResult(
		const dtQueryResult& StraightResult,
		TArray<FNavPathPoint>& OutPoints)
	{
		const int32 Count = StraightResult.size();
		if (Count <= 0)
		{
			return false;
		}

		OutPoints.Reset(Count);
		OutPoints.Reserve(Count);

		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
#if ENGINE_MAJOR_VERSION >= 5
			const dtReal* Pos      = StraightResult.getPos(Idx);
			const FVector WorldPt  = Recast2UnrealPoint(Pos);
#else
			const float*  Pos      = StraightResult.getPos(Idx);
			const FVector WorldPt  = Recast2UnrealPoint(Pos);
#endif
			FNavPathPoint NewPoint(WorldPt);
			NewPoint.NodeRef = StraightResult.getRef(Idx);

			OutPoints.Add(NewPoint);
		}

		return OutPoints.Num() > 0;
	}

	/**
	 * Plain Detour path:
	 *   1) find nearest polys for A/B
	 *   2) optional LOS shortcut (raycast)
	 *   3) our custom A* for poly corridor
	 *   4) Detour straight path along that corridor
	 */
	static bool BuildBaseDetourPath(
		FAPSNavMeshQuery&      APSQuery,
		const dtQueryFilter*   DtFilter,
		const float            Extent[3],
		const FVector&         A,
		const FVector&         B,
		float                  CostLimit,
		bool                   bAllowRaycastShortcut,
		TArray<dtPolyRef>&     OutCorridor,
		TArray<FNavPathPoint>& OutPoints,
		bool&                  bOutPartial)
	{
		OutPoints.Reset();
		OutCorridor.Reset();
		bOutPartial = false;

		if (!APSQuery.InternalQuery || !APSQuery.NavMesh || !DtFilter)
		{
			return false;
		}

		dtNavMeshQuery* Query = APSQuery.InternalQuery;

		// Start poly.
		dtPolyRef StartRef = 0;
		float     StartPt[3];
		if (!FindNearestPoly(Query, DtFilter, Extent, A, StartRef, StartPt))
		{
			return false;
		}

		// End poly.
		dtPolyRef EndRef = 0;
		float     EndPt[3];
		if (!FindNearestPoly(Query, DtFilter, Extent, B, EndRef, EndPt))
		{
			return false;
		}

		// Super cheap case: straight LOS on navmesh, no need to touch A*.
		if (bAllowRaycastShortcut && HasNavmeshLineOfSight(Query, DtFilter, StartRef, StartPt, EndPt))
		{
			OutPoints.Reset(2);
			OutPoints.Reserve(2);
			OutCorridor.Reserve(2);

			FNavPathPoint P0(A);
			FNavPathPoint P1(B);
			P0.NodeRef = StartRef;
			P1.NodeRef = EndRef;

			OutPoints.Add(P0);
			OutPoints.Add(P1);
			OutCorridor.Add(StartRef);
			if (EndRef != StartRef)
			{
				OutCorridor.Add(EndRef);
			}

			return true;
		}

		// Our own A* over Detour nodes.
		TArray<dtPolyRef> Corridor;
		Corridor.Reserve(64);

		float TotalCost = 0.0f;

		const dtStatus PathStatus = APSQuery.FindPath(
			StartRef,
			EndRef,
			StartPt,
			EndPt,
			CostLimit,
			Corridor,
			&TotalCost);

		if (dtStatusFailed(PathStatus) || Corridor.Num() == 0)
		{
			return false;
		}
		OutCorridor = Corridor;

		bOutPartial = (PathStatus & DT_PARTIAL_RESULT) != 0;

		// Turn corridor into a straight line of points.
		dtQueryResult StraightResult;

#if ENGINE_MAJOR_VERSION >= 5
		dtReal DtStartPt[3] =
		{
			(dtReal)StartPt[0],
			(dtReal)StartPt[1],
			(dtReal)StartPt[2],
		};

		dtReal DtEndPt[3] =
		{
			(dtReal)EndPt[0],
			(dtReal)EndPt[1],
			(dtReal)EndPt[2],
		};

		const dtStatus StraightStatus = Query->findStraightPath(
			DtStartPt,
			DtEndPt,
			Corridor.GetData(),
			Corridor.Num(),
			StraightResult,
			0);
#else
		const dtStatus StraightStatus = Query->findStraightPath(
			StartPt,
			EndPt,
			Corridor.GetData(),
			Corridor.Num(),
			StraightResult,
			0);
#endif

		if (dtStatusFailed(StraightStatus))
		{
			return false;
		}

		return BuildStraightPathFromResult(StraightResult, OutPoints);
	}

	/** Quick sanity check: no segment of the path should cut through walls. */
	static bool ValidateNavMeshSegment(
		const FVector&            A,
		const FVector&            B,
		dtNavMeshQuery*           Query,
		const dtQueryFilter*      Filter,
		const float               Extent[3])
	{
		if (!Query || !Filter || !Extent)
		{
			return false;
		}

		dtPolyRef StartRef = 0;
		float     StartPt[3];
		if (!FindNearestPoly(Query, Filter, Extent, A, StartRef, StartPt))
		{
			return false;
		}

		dtPolyRef EndRef = 0;
		float     EndPt[3];
		if (!FindNearestPoly(Query, Filter, Extent, B, EndRef, EndPt))
		{
			return false;
		}

		return HasNavmeshLineOfSight(Query, Filter, StartRef, StartPt, EndPt);
	}

	static bool ValidatePathOnNavMesh(
		const TArray<FNavPathPoint>& Points,
		dtNavMeshQuery*             Query,
		const dtQueryFilter*        Filter,
		const float                 Extent[3])
	{
		if (!Query || !Filter || !Extent || Points.Num() < 2)
		{
			return false;
		}

		const int32 NumPts = Points.Num();

		for (int32 i = 0; i < NumPts - 1; ++i)
		{
			const FVector A = Points[i].Location;
			const FVector B = Points[i + 1].Location;
			if (!ValidateNavMeshSegment(A, B, Query, Filter, Extent))
			{
				return false;
			}
		}

		return true;
	}

	// --------------------------------------------------------------------
	// Resampling the path before we bend it
	// --------------------------------------------------------------------

	/**
	 * We want more "control points" along the path so that the arc
	 * actually looks like a curve, not just one sharp corner.
	 *
	 * NumMidPoints = how many midpoints we want to insert globally.
	 * Result: Start + NumMidPoints + End (but never fewer than original).
	 */
	static void BuildArcSamplingPath(
		const TArray<FNavPathPoint>& InPoints,
		int32                        NumMidPoints,
		TArray<FNavPathPoint>&       OutPoints)
	{
		const int32 InNum = InPoints.Num();
		if (InNum < 2 || NumMidPoints <= 0)
		{
			OutPoints = InPoints;
			return;
		}

		// Length of every segment (2D) and total length.
		TArray<float, TInlineAllocator<64>> SegLen;
		SegLen.SetNumUninitialized(InNum - 1);

		float TotalLen = 0.0f;
		for (int32 i = 0; i < InNum - 1; ++i)
		{
			const FVector& A = InPoints[i].Location;
			const FVector& B = InPoints[i + 1].Location;

			const float Len = FVector::Dist2D(A, B);
			SegLen[i]       = Len;
			TotalLen       += Len;
		}

		if (TotalLen < KINDA_SMALL_NUMBER)
		{
			OutPoints = InPoints;
			return;
		}

		// Distance-driven sample count: long routes get more samples so the arc envelope
		// remains smooth and per-point validation is less likely to reject large jumps.
		// TargetArcSegmentLength keeps short paths identical to their previous behaviour.
		// DesiredTotal respects: original point count, user midpoint request, and route length.
		constexpr float TargetArcSegmentLength = 400.0f;
		const int32 DistanceDrivenTotal = FMath::CeilToInt(TotalLen / TargetArcSegmentLength) + 1;
		const int32 DesiredTotal = FMath::Min(FMath::Max3(NumMidPoints + 2, DistanceDrivenTotal, InNum), 128);

		OutPoints.Reset(DesiredTotal);
		OutPoints.Add(InPoints[0]); // keep exact start

		for (int32 s = 1; s < DesiredTotal - 1; ++s)
		{
			const float TargetDist = TotalLen * (float(s) / float(DesiredTotal - 1));

			float Accum  = 0.0f;
			int32 SegIdx = 0;

			while (SegIdx < SegLen.Num() && Accum + SegLen[SegIdx] < TargetDist)
			{
				Accum += SegLen[SegIdx];
				++SegIdx;
			}

			if (SegIdx >= SegLen.Num())
			{
				OutPoints.Add(InPoints.Last());
				continue;
			}

			const float LocalLen = SegLen[SegIdx];
			const float LocalT   = (LocalLen > KINDA_SMALL_NUMBER)
				? (TargetDist - Accum) / LocalLen
				: 0.0f;

			const FVector& P0  = InPoints[SegIdx].Location;
			const FVector& P1  = InPoints[SegIdx + 1].Location;
			const FVector  Pos = FMath::Lerp(P0, P1, LocalT);

			FNavPathPoint Sample(Pos);
			Sample.NodeRef = InPoints[SegIdx].NodeRef; // will be snapped later anyway

			OutPoints.Add(Sample);
		}

		OutPoints.Add(InPoints.Last()); // keep exact end
	}

	static FVector ComputeCornerRightVector(
		const FVector& DirIn,
		const FVector& DirOut,
		const FVector& FallbackDir2D)
	{
		FVector CornerRight = FVector::ZeroVector;
		const FVector CornerTangent = (DirIn + DirOut).GetSafeNormal2D();
		if (!CornerTangent.IsNearlyZero())
		{
			CornerRight = FVector(-CornerTangent.Y, CornerTangent.X, 0.0f);
		}
		else
		{
			const FVector RightIn(-DirIn.Y, DirIn.X, 0.0f);
			const FVector RightOut(-DirOut.Y, DirOut.X, 0.0f);
			CornerRight = (RightIn + RightOut).GetSafeNormal2D();
			if (CornerRight.IsNearlyZero())
			{
				CornerRight = FVector(-FallbackDir2D.Y, FallbackDir2D.X, 0.0f);
			}
		}

		return CornerRight.GetSafeNormal2D();
	}

	static float ComputePointDeviationFromSegment2D(
		const FVector& SegmentStart,
		const FVector& SegmentEnd,
		const FVector& Point)
	{
		const float SegmentLen = FVector::Dist2D(SegmentStart, SegmentEnd);
		if (SegmentLen <= KINDA_SMALL_NUMBER)
		{
			return FVector::Dist2D(Point, SegmentStart);
		}

		const FVector SegmentDir = (SegmentEnd - SegmentStart).GetSafeNormal2D();
		const FVector FromStart = Point - SegmentStart;
		const float Along = FMath::Clamp(FVector::DotProduct(FromStart, SegmentDir), 0.0f, SegmentLen);
		const FVector Closest = SegmentStart + SegmentDir * Along;
		return FVector::Dist2D(Point, Closest);
	}

	static void AddPointIfNotNear2D(
		TArray<FNavPathPoint>& Points,
		const FVector&         Position,
		dtPolyRef              NodeRef)
	{
		if (Points.Num() > 0 && FVector::Dist2D(Points.Last().Location, Position) <= GMinPathPointDistance2D)
		{
			return;
		}

		FNavPathPoint Pt(Position);
		Pt.NodeRef = NodeRef;
		Points.Add(Pt);
	}

	static void BuildAdaptiveCornerFilletPath(
		const TArray<FNavPathPoint>& InPoints,
		int32                        NumMidPoints,
		float                        LaneOffset,
		dtNavMeshQuery*              Query,
		const dtQueryFilter*         Filter,
		const float                  Extent[3],
		bool                         bDbg,
		TArray<FNavPathPoint>&       OutPoints)
	{
		const int32 InNum = InPoints.Num();
		if (InNum < 3 || NumMidPoints <= 0 || FMath::IsNearlyZero(LaneOffset, KINDA_SMALL_NUMBER))
		{
			OutPoints = InPoints;
			return;
		}

		// Corner Detection v2 tuning constants: selected to favor stable blocked wall-like turns,
		// while demoting short-lived zigzags in dense/navmesh-noisy point sequences.
		// Slope transition tuning complements the XY turn score so flat->ramp / ramp->flat
		// events can still receive lane spreading when the 2D cross is weak or near-zero.
		constexpr float StrongCornerThreshold = 0.35f;
		constexpr float MinWindowStrength = 0.20f; // Window turn floor: below this we treat as local zigzag/noise unless local angle is already strong.
		constexpr float MinVisibilityAnchorSpan = 80.0f;
		constexpr float WindowAngleBlend = 0.70f; // Higher weight on anchor-window turn over immediate triple turn.
		constexpr float CrossMismatchFactor = 0.60f; // Penalize if local and window turn signs disagree.
		constexpr float PersistenceBaseFactor = 0.45f; // Isolated turn baseline; grows with same-direction neighbor support.
		constexpr float PersistenceFullSupportStrength = 0.35f; // Neighbor support level where persistence reaches full weight.
		constexpr float BlockedVisibilityFactor = 1.15f; // Slight boost when anchor-to-anchor shortcut is blocked (wall-like corner).
		constexpr float ClearVisibilityFactor = 0.72f; // Visible shortcut reduces chance of fillet but does not force skip.
		constexpr float NearbyCornerStrengthOverrideFactor = 1.35f; // Close corner needs to be much stronger than previous accepted one.
		constexpr float MinSegmentLenForFillet = 8.0f;
		constexpr float MaxSegmentPortion = 0.45f;
		constexpr float BaseFilletScale = 0.6f;
		constexpr float StrengthFilletScale = 0.4f;
		constexpr float MinFilletDistance = 4.0f;
		constexpr float MinSlopeDistance2D = 16.0f;
		constexpr float MinSlopeChangeForScore = 0.08f;
		constexpr float FullSlopeChangeForScore = 0.45f;
		constexpr float WeakCrossThreshold = 0.05f;
		constexpr int32 MinFilletSamples = 2;
		constexpr int32 MaxFilletSamples = 6;
		constexpr int32 MaxFilletPointsPerCornerEstimate = MaxFilletSamples + 2; // Entry + max interior samples + Exit.
		constexpr int32 MaxTotalPoints = 192;                 // Hard cap to keep per-path processing bounded.
		const int32 DetailSamples = FMath::Clamp(NumMidPoints, MinFilletSamples, MaxFilletSamples);
		const int32 EstimatedFilletCorners = FMath::Max(1, InNum - 2);
		// Scale look/spacing with lane width so wider lanes evaluate broader turns, but keep sane hard bounds.
		const float WindowLookDistance = FMath::Clamp(FMath::Abs(LaneOffset) * 1.25f, 100.0f, 400.0f);
		const float MinCornerSpacing = FMath::Clamp(FMath::Abs(LaneOffset) * 0.9f, 80.0f, 260.0f);

		OutPoints.Reset(FMath::Min(MaxTotalPoints, InNum + EstimatedFilletCorners * MaxFilletPointsPerCornerEstimate));
		OutPoints.Add(InPoints[0]);

		TArray<float, TInlineAllocator<64>> CumulativeDist;
		CumulativeDist.SetNumUninitialized(InNum);
		CumulativeDist[0] = 0.0f;
		float TotalPathLen = 0.0f;
		for (int32 Idx = 1; Idx < InNum; ++Idx)
		{
			const float SegLen = FVector::Dist2D(InPoints[Idx - 1].Location, InPoints[Idx].Location);
			TotalPathLen += SegLen;
			CumulativeDist[Idx] = TotalPathLen;
		}

		auto GetPointAtPathDistance = [&](float Distance) -> FVector
		{
			if (Distance <= 0.0f)
			{
				return InPoints[0].Location;
			}

			if (Distance >= TotalPathLen)
			{
				return InPoints.Last().Location;
			}

			int32 NextIdx = 1;
			while (NextIdx < InNum && CumulativeDist[NextIdx] < Distance)
			{
				++NextIdx;
			}

			if (NextIdx >= InNum)
			{
				return InPoints.Last().Location;
			}

			const int32 PrevIdx = NextIdx - 1;
			const float DistA = CumulativeDist[PrevIdx];
			const float DistB = CumulativeDist[NextIdx];
			const float SegmentLen = DistB - DistA;
			if (SegmentLen <= KINDA_SMALL_NUMBER)
			{
				return InPoints[NextIdx].Location;
			}

			const float Alpha = (Distance - DistA) / SegmentLen;
			return FMath::Lerp(InPoints[PrevIdx].Location, InPoints[NextIdx].Location, Alpha);
		};

		auto ComputeLocalTurn = [&](int32 Index, float& OutStrength, float& OutCrossZ) -> bool
		{
			OutStrength = 0.0f;
			OutCrossZ = 0.0f;
			if (Index <= 0 || Index >= InNum - 1)
			{
				return false;
			}

			const FVector Prev = InPoints[Index - 1].Location;
			const FVector Curr = InPoints[Index].Location;
			const FVector Next = InPoints[Index + 1].Location;
			const FVector LocalDirIn = (Curr - Prev).GetSafeNormal2D();
			const FVector LocalDirOut = (Next - Curr).GetSafeNormal2D();
			const float LocalLenIn = FVector::Dist2D(Prev, Curr);
			const float LocalLenOut = FVector::Dist2D(Curr, Next);
			if (LocalDirIn.IsNearlyZero() || LocalDirOut.IsNearlyZero() ||
				LocalLenIn < MinSegmentLenForFillet || LocalLenOut < MinSegmentLenForFillet)
			{
				return false;
			}

			const float LocalDot = FMath::Clamp(FVector::DotProduct(LocalDirIn, LocalDirOut), -1.0f, 1.0f);
			OutStrength = FMath::Clamp(1.0f - LocalDot, 0.0f, 1.0f);
			OutCrossZ = (LocalDirIn.X * LocalDirOut.Y) - (LocalDirIn.Y * LocalDirOut.X);
			return true;
		};

		auto ComputeSlopeGrade = [&](const FVector& From, const FVector& To) -> float
		{
			const float DistXY = FVector::Dist2D(From, To);
			if (DistXY < MinSlopeDistance2D)
			{
				return 0.0f;
			}

			return (To.Z - From.Z) / DistXY;
		};

		auto ComputeSlopeStrengthFromChange = [&](float SlopeChange) -> float
		{
			const float Denominator = FullSlopeChangeForScore - MinSlopeChangeForScore;
			if (Denominator <= KINDA_SMALL_NUMBER)
			{
				return 0.0f;
			}

			return FMath::Clamp((SlopeChange - MinSlopeChangeForScore) / Denominator, 0.0f, 1.0f);
		};

		auto ResolvePreferredDir2D = [&](const FVector& Primary, const FVector& Secondary, const FVector& Tertiary) -> FVector
		{
			if (!Primary.IsNearlyZero())
			{
				return Primary;
			}
			if (!Secondary.IsNearlyZero())
			{
				return Secondary;
			}
			if (!Tertiary.IsNearlyZero())
			{
				return Tertiary;
			}

			return FVector::ZeroVector;
		};

		bool bHasAcceptedCorner = false;
		float LastAcceptedCornerDistance = 0.0f;
		float LastAcceptedCornerScore = 0.0f;

		for (int32 i = 1; i < InNum - 1; ++i)
		{
			const FVector& PrevLoc = InPoints[i - 1].Location;
			const FVector& CornerLoc = InPoints[i].Location;
			const FVector& NextLoc = InPoints[i + 1].Location;

			const FVector DirIn = (CornerLoc - PrevLoc).GetSafeNormal2D();
			const FVector DirOut = (NextLoc - CornerLoc).GetSafeNormal2D();
			const float LenIn = FVector::Dist2D(PrevLoc, CornerLoc);
			const float LenOut = FVector::Dist2D(CornerLoc, NextLoc);

			if (DirIn.IsNearlyZero() || DirOut.IsNearlyZero() ||
				LenIn < MinSegmentLenForFillet || LenOut < MinSegmentLenForFillet)
			{
				if (bDbg && (LenIn < MinSegmentLenForFillet || LenOut < MinSegmentLenForFillet))
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: fillet skipped at corner %d (short segment, in=%.1f out=%.1f)"),
						i, LenIn, LenOut);
				}

				AddPointIfNotNear2D(OutPoints, CornerLoc, static_cast<dtPolyRef>(InPoints[i].NodeRef));
				continue;
			}

			const float DotInOut = FMath::Clamp(FVector::DotProduct(DirIn, DirOut), -1.0f, 1.0f);
			const float LocalCornerStrength = FMath::Clamp(1.0f - DotInOut, 0.0f, 1.0f);
			const float CrossZ = (DirIn.X * DirOut.Y) - (DirIn.Y * DirOut.X);

			const float CornerDistance = CumulativeDist[i];
			const float PrevAnchorDistance = FMath::Max(0.0f, CornerDistance - WindowLookDistance);
			const float NextAnchorDistance = FMath::Min(TotalPathLen, CornerDistance + WindowLookDistance);
			const FVector PrevAnchor = GetPointAtPathDistance(PrevAnchorDistance);
			const FVector NextAnchor = GetPointAtPathDistance(NextAnchorDistance);

			const FVector WindowDirIn = (CornerLoc - PrevAnchor).GetSafeNormal2D();
			const FVector WindowDirOut = (NextAnchor - CornerLoc).GetSafeNormal2D();
			const bool bValidWindowDirs = !WindowDirIn.IsNearlyZero() && !WindowDirOut.IsNearlyZero();
			const float WindowDot = bValidWindowDirs
				? FMath::Clamp(FVector::DotProduct(WindowDirIn, WindowDirOut), -1.0f, 1.0f)
				: DotInOut;
			const float WindowCornerStrength = bValidWindowDirs
				? FMath::Clamp(1.0f - WindowDot, 0.0f, 1.0f)
				: LocalCornerStrength;
			const float WindowCrossZ = bValidWindowDirs
				? (WindowDirIn.X * WindowDirOut.Y) - (WindowDirIn.Y * WindowDirOut.X)
				: CrossZ;

			const float LocalGradeIn = ComputeSlopeGrade(PrevLoc, CornerLoc);
			const float LocalGradeOut = ComputeSlopeGrade(CornerLoc, NextLoc);
			const float LocalSlopeChange = FMath::Abs(LocalGradeOut - LocalGradeIn);
			const float LocalSlopeStrength = ComputeSlopeStrengthFromChange(LocalSlopeChange);
			const float WindowGradeIn = ComputeSlopeGrade(PrevAnchor, CornerLoc);
			const float WindowGradeOut = ComputeSlopeGrade(CornerLoc, NextAnchor);
			const float WindowSlopeChange = FMath::Abs(WindowGradeOut - WindowGradeIn);
			const float WindowSlopeStrength = ComputeSlopeStrengthFromChange(WindowSlopeChange);
			const float SlopeCornerStrength = FMath::Max(LocalSlopeStrength, WindowSlopeStrength);

			const bool bCrossConsistent = !FMath::IsNearlyZero(WindowCrossZ, KINDA_SMALL_NUMBER) &&
				(FMath::Sign(WindowCrossZ) == FMath::Sign(CrossZ));
			const float CrossConsistencyFactor = bCrossConsistent ? 1.0f : CrossMismatchFactor;

			float MaxNeighborSupport = 0.0f;
			const int32 NeighborIndices[2] = { i - 1, i + 1 };
			for (int32 NeighborArrayIndex = 0; NeighborArrayIndex < 2; ++NeighborArrayIndex)
			{
				const int32 NeighborIndex = NeighborIndices[NeighborArrayIndex];

				float NeighborStrength = 0.0f;
				float NeighborCross = 0.0f;
				if (ComputeLocalTurn(NeighborIndex, NeighborStrength, NeighborCross) &&
					!FMath::IsNearlyZero(NeighborCross, KINDA_SMALL_NUMBER) &&
					FMath::Sign(NeighborCross) == FMath::Sign(CrossZ))
				{
					MaxNeighborSupport = FMath::Max(MaxNeighborSupport, NeighborStrength);
				}
			}

			const float PersistenceFactor = FMath::Lerp(
				PersistenceBaseFactor,
				1.0f,
				FMath::Clamp(MaxNeighborSupport / PersistenceFullSupportStrength, 0.0f, 1.0f));

			bool bHasAnchorVisibility = false;
			bool bAnchorsBlocked = false;
			const float AnchorSpan = FVector::Dist2D(PrevAnchor, NextAnchor);
			if (Query && Filter && Extent && AnchorSpan >= MinVisibilityAnchorSpan)
			{
				bHasAnchorVisibility = true;
				const bool bAnchorsVisible = ValidateNavMeshSegment(PrevAnchor, NextAnchor, Query, Filter, Extent);
				bAnchorsBlocked = !bAnchorsVisible;
			}

			const float VisibilityBlockFactor = !bHasAnchorVisibility ? 1.0f : (bAnchorsBlocked ? BlockedVisibilityFactor : ClearVisibilityFactor);
			const float AngleScore = FMath::Lerp(LocalCornerStrength, WindowCornerStrength, WindowAngleBlend);
			// Multiplicative model: all terms are "confidence" weights [0..~1+] and each can suppress noisy corners.
			// Angle captures turn magnitude, visibility captures wall-like blocking evidence, and persistence/cross keep stable direction.
			// Values above 1.0 are intentional (blocked visibility boost), then fillet width strength is clamped downstream.
			const float FinalCornerScore = AngleScore * VisibilityBlockFactor * CrossConsistencyFactor * PersistenceFactor;
			const float FinalEventScore = FMath::Max(FinalCornerScore, SlopeCornerStrength);
			const bool bSlopeDominantEvent =
				SlopeCornerStrength >= StrongCornerThreshold &&
				(FMath::Abs(CrossZ) < WeakCrossThreshold ||
				 FinalCornerScore < StrongCornerThreshold ||
				 SlopeCornerStrength > FinalCornerScore);
			const bool bMixedEvent = !bSlopeDominantEvent &&
				SlopeCornerStrength >= StrongCornerThreshold &&
				FinalCornerScore >= StrongCornerThreshold;
			const TCHAR* EventType = bMixedEvent ? TEXT("mixed") : (bSlopeDominantEvent ? TEXT("slope") : TEXT("xy"));

			if (WindowCornerStrength < MinWindowStrength &&
				LocalCornerStrength < StrongCornerThreshold &&
				SlopeCornerStrength < StrongCornerThreshold)
			{
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: corner %d skipped as noise (xyLocal=%.3f xyWindow=%.3f xyFinal=%.3f slopeChangeLocal=%.3f slopeChangeWindow=%.3f slopeFinal=%.3f type=%s vis=%s)"),
						i,
						LocalCornerStrength,
						WindowCornerStrength,
						FinalCornerScore,
						LocalSlopeChange,
						WindowSlopeChange,
						SlopeCornerStrength,
						EventType,
						bHasAnchorVisibility ? (bAnchorsBlocked ? TEXT("blocked") : TEXT("clear")) : TEXT("n/a"));
				}
				AddPointIfNotNear2D(OutPoints, CornerLoc, static_cast<dtPolyRef>(InPoints[i].NodeRef));
				continue;
			}

			if (FinalEventScore < StrongCornerThreshold)
			{
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: corner %d skipped weak (xyLocal=%.3f xyWindow=%.3f xyFinal=%.3f slopeChangeLocal=%.3f slopeChangeWindow=%.3f slopeFinal=%.3f finalEvent=%.3f type=%s persist=%.3f vis=%s)"),
						i,
						LocalCornerStrength,
						WindowCornerStrength,
						FinalCornerScore,
						LocalSlopeChange,
						WindowSlopeChange,
						SlopeCornerStrength,
						FinalEventScore,
						EventType,
						PersistenceFactor,
						bHasAnchorVisibility ? (bAnchorsBlocked ? TEXT("blocked") : TEXT("clear")) : TEXT("n/a"));
				}
				AddPointIfNotNear2D(OutPoints, CornerLoc, static_cast<dtPolyRef>(InPoints[i].NodeRef));
				continue;
			}

			if (bHasAcceptedCorner)
			{
				const float DistFromLastAccepted = CornerDistance - LastAcceptedCornerDistance;
				if (DistFromLastAccepted < MinCornerSpacing &&
					FinalEventScore < LastAcceptedCornerScore * NearbyCornerStrengthOverrideFactor)
				{
					if (bDbg)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: corner %d skipped too close (dist=%.1f, score=%.3f, prev=%.3f, type=%s)"),
							i,
							DistFromLastAccepted,
							FinalEventScore,
							LastAcceptedCornerScore,
							EventType);
					}
					AddPointIfNotNear2D(OutPoints, CornerLoc, static_cast<dtPolyRef>(InPoints[i].NodeRef));
					continue;
				}
			}

			const float CornerStrength = FMath::Clamp(FinalEventScore, 0.0f, 1.0f);

			const float MaxFilletDistance = MaxSegmentPortion * FMath::Min(LenIn, LenOut);
			const float DesiredWidth = FMath::Abs(LaneOffset) * (bSlopeDominantEvent ? SlopeCornerStrength : CornerStrength);
			const float FilletDistance = FMath::Clamp(DesiredWidth * (BaseFilletScale + StrengthFilletScale * CornerStrength), 0.0f, MaxFilletDistance);

			if (FilletDistance < MinFilletDistance)
			{
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: corner %d skipped tiny fillet (type=%s width=%.1f fillet=%.1f score=%.3f)"),
						i,
						EventType,
						DesiredWidth,
						FilletDistance,
						FinalEventScore);
				}
				AddPointIfNotNear2D(OutPoints, CornerLoc, static_cast<dtPolyRef>(InPoints[i].NodeRef));
				continue;
			}

			const FVector Entry = CornerLoc - (DirIn * FilletDistance);
			const FVector Exit = CornerLoc + (DirOut * FilletDistance);
			const FVector FallbackDir2D = (NextLoc - PrevLoc).GetSafeNormal2D();
			const float LaneSign = (LaneOffset >= 0.0f) ? 1.0f : -1.0f;
			FVector PreferredBaseSide = FVector::ZeroVector;
			const TCHAR* SelectedSide = TEXT("outside");
			float RightFreeScale = 0.0f;
			float LeftFreeScale = 0.0f;

			if (bSlopeDominantEvent)
			{
				const FVector TravelDir2D = ResolvePreferredDir2D(
					(NextAnchor - PrevAnchor).GetSafeNormal2D(),
					FallbackDir2D,
					!DirIn.IsNearlyZero() ? DirIn : DirOut);

				const FVector TravelRight(-TravelDir2D.Y, TravelDir2D.X, 0.0f);
				auto EvaluateSideFreeScale = [&](const FVector& SideDir) -> float
				{
					const bool bValidForSideEvaluation =
						Query && Filter && Extent && !SideDir.IsNearlyZero() && DesiredWidth > KINDA_SMALL_NUMBER;
					if (!bValidForSideEvaluation)
					{
						return 0.0f;
					}

					for (const float OffsetScale : GOffsetScales)
					{
						const FVector CandidateControl = CornerLoc + (SideDir * DesiredWidth * OffsetScale);
						if (ValidateNavMeshSegment(Entry, CandidateControl, Query, Filter, Extent) &&
							ValidateNavMeshSegment(CandidateControl, Exit, Query, Filter, Extent))
						{
							return OffsetScale;
						}
					}

					return 0.0f;
				};

				if (!TravelRight.IsNearlyZero())
				{
					RightFreeScale = EvaluateSideFreeScale(TravelRight);
					LeftFreeScale = EvaluateSideFreeScale(-TravelRight);
				}

				PreferredBaseSide = (RightFreeScale >= LeftFreeScale) ? TravelRight : -TravelRight;
				SelectedSide = (RightFreeScale > LeftFreeScale) ? TEXT("right-free") :
					(LeftFreeScale > RightFreeScale) ? TEXT("left-free") : TEXT("right-tie");

				if (PreferredBaseSide.IsNearlyZero())
				{
					const FVector GeometryRight = ComputeCornerRightVector(DirIn, DirOut, FallbackDir2D);
					PreferredBaseSide = !TravelRight.IsNearlyZero() ? TravelRight : GeometryRight;
				}
			}
			else
			{
				const FVector CornerRight = ComputeCornerRightVector(DirIn, DirOut, FallbackDir2D);
				PreferredBaseSide = (CrossZ > 0.0f) ? CornerRight : -CornerRight;
				SelectedSide = (CrossZ > 0.0f) ? TEXT("outside-right") : TEXT("outside-left");
			}

			const FVector Control = CornerLoc + (PreferredBaseSide * LaneSign * DesiredWidth);

			const int32 FilletSamples = FMath::Clamp(
				FMath::RoundToInt(FMath::Lerp(static_cast<float>(MinFilletSamples), static_cast<float>(DetailSamples), CornerStrength)),
				MinFilletSamples,
				DetailSamples);

			constexpr int32 FinalPointReserve = 1;
			if (OutPoints.Num() + FilletSamples + 2 + FinalPointReserve > MaxTotalPoints)
			{
				AddPointIfNotNear2D(OutPoints, CornerLoc, static_cast<dtPolyRef>(InPoints[i].NodeRef));
				continue;
			}

			AddPointIfNotNear2D(OutPoints, Entry, static_cast<dtPolyRef>(InPoints[i].NodeRef));

			for (int32 SampleIdx = 1; SampleIdx <= FilletSamples; ++SampleIdx)
			{
				// U intentionally stays in (0..1): Entry/Exit are already added explicitly, so interior Bezier samples avoid endpoint duplication.
				const float U = static_cast<float>(SampleIdx) / static_cast<float>(FilletSamples + 1);
				const float OneMinusU = 1.0f - U;
				const FVector CurvePoint =
					(Entry * (OneMinusU * OneMinusU)) +
					(Control * (2.0f * OneMinusU * U)) +
					(Exit * (U * U));

				AddPointIfNotNear2D(OutPoints, CurvePoint, static_cast<dtPolyRef>(InPoints[i].NodeRef));
			}

			AddPointIfNotNear2D(OutPoints, Exit, static_cast<dtPolyRef>(InPoints[i].NodeRef));

			if (bDbg)
			{
				UE_LOG(LogAPSNav, Log,
					TEXT("LowLevelArc: fillet corner %d type=%s side=%s freeL=%.3f freeR=%.3f xyLocal=%.3f xyWindow=%.3f xyFinal=%.3f gradeIn=%.3f gradeOut=%.3f slopeChangeLocal=%.3f windowGradeIn=%.3f windowGradeOut=%.3f slopeChangeWindow=%.3f slopeFinal=%.3f finalEvent=%.3f vis=%s inserted=%d lane=%.1f"),
					i,
					EventType,
					SelectedSide,
					LeftFreeScale,
					RightFreeScale,
					LocalCornerStrength,
					WindowCornerStrength,
					FinalCornerScore,
					LocalGradeIn,
					LocalGradeOut,
					LocalSlopeChange,
					WindowGradeIn,
					WindowGradeOut,
					WindowSlopeChange,
					SlopeCornerStrength,
					FinalEventScore,
					bHasAnchorVisibility ? (bAnchorsBlocked ? TEXT("blocked") : TEXT("clear")) : TEXT("n/a"),
					FilletSamples + 2,
					LaneOffset);
			}

			LastAcceptedCornerDistance = CornerDistance;
			LastAcceptedCornerScore = FinalEventScore;
			bHasAcceptedCorner = true;
		}

		if (OutPoints.Num() < MaxTotalPoints)
		{
			AddPointIfNotNear2D(OutPoints, InPoints.Last().Location, static_cast<dtPolyRef>(InPoints.Last().NodeRef));
		}
		else
		{
			OutPoints.Last().Location = InPoints.Last().Location;
			OutPoints.Last().NodeRef = static_cast<dtPolyRef>(InPoints.Last().NodeRef);
		}
	}

	template<typename Allocator>
	static bool BuildCumulativeDistances(
		const TArray<FNavPathPoint>& Points,
		TArray<float, Allocator>& OutCumulative,
		float& OutTotalLen)
	{
		const int32 NumPts = Points.Num();
		if (NumPts < 2)
		{
			OutTotalLen = 0.0f;
			return false;
		}

		OutCumulative.SetNumUninitialized(NumPts);
		OutCumulative[0] = 0.0f;

		float TotalLen = 0.0f;
		for (int32 i = 1; i < NumPts; ++i)
		{
			const FVector& Prev = Points[i - 1].Location;
			const FVector& Curr = Points[i].Location;

			const float SegLen = FVector::Dist2D(Prev, Curr);
			TotalLen += SegLen;
			OutCumulative[i] = TotalLen;
		}

		OutTotalLen = TotalLen;
		return TotalLen >= KINDA_SMALL_NUMBER;
	}

	static const TCHAR* ManeuverZoneTypeToString(EAPSManeuverZoneType Type)
	{
		switch (Type)
		{
		case EAPSManeuverZoneType::XYTurn: return TEXT("XYTurn");
		case EAPSManeuverZoneType::Slope:  return TEXT("Slope");
		case EAPSManeuverZoneType::Mixed:  return TEXT("Mixed");
		default:                           return TEXT("Unknown");
		}
	}

	static float ComputeManeuverZoneEnvelope(float Distance, const FAPSManeuverZone& Zone)
	{
		if (Distance <= Zone.StartDistance || Distance >= Zone.EndDistance)
		{
			return 0.0f;
		}

		if (Distance <= Zone.PeakDistance)
		{
			const float Denominator = Zone.PeakDistance - Zone.StartDistance;
			if (Denominator <= KINDA_SMALL_NUMBER)
			{
				return 1.0f;
			}

			const float Alpha = FMath::Clamp((Distance - Zone.StartDistance) / Denominator, 0.0f, 1.0f);
			return FMath::SmoothStep(0.0f, 1.0f, Alpha);
		}

		const float Denominator = Zone.EndDistance - Zone.PeakDistance;
		if (Denominator <= KINDA_SMALL_NUMBER)
		{
			return 1.0f;
		}

		const float Alpha = FMath::Clamp((Zone.EndDistance - Distance) / Denominator, 0.0f, 1.0f);
		return FMath::SmoothStep(0.0f, 1.0f, Alpha);
	}

	static const TCHAR* ManeuverZoneSourceToString(const FAPSManeuverZone& Zone)
	{
		if (Zone.bFromSyntheticFallback)
		{
			return TEXT("synthetic");
		}
		return Zone.bFromDirectCorner ? TEXT("corner") : TEXT("zone");
	}

	template<typename Allocator>
	static FVector GetPointAtPathDistance(
		const TArray<FNavPathPoint>&     Points,
		const TArray<float, Allocator>&  CumulativeDist,
		float                            TotalPathLen,
		float                            Distance)
	{
		const int32 NumPoints = Points.Num();
		if (NumPoints <= 0)
		{
			return FVector::ZeroVector;
		}

		if (Distance <= 0.0f)
		{
			return Points[0].Location;
		}

		if (Distance >= TotalPathLen)
		{
			return Points.Last().Location;
		}

		int32 NextIdx = 1;
		while (NextIdx < NumPoints && CumulativeDist[NextIdx] < Distance)
		{
			++NextIdx;
		}

		if (NextIdx >= NumPoints)
		{
			return Points.Last().Location;
		}

		const int32 PrevIdx = NextIdx - 1;
		const float DistA = CumulativeDist[PrevIdx];
		const float DistB = CumulativeDist[NextIdx];
		const float SegmentLen = DistB - DistA;
		if (SegmentLen <= KINDA_SMALL_NUMBER)
		{
			return Points[NextIdx].Location;
		}

		const float Alpha = (Distance - DistA) / SegmentLen;
		return FMath::Lerp(Points[PrevIdx].Location, Points[NextIdx].Location, Alpha);
	}

	static float EvaluateManeuverSideFreeScale(
		const FVector&            PeakLoc,
		const FVector&            PrevAnchor,
		const FVector&            NextAnchor,
		const FVector&            SideDir,
		float                     RequestedWidth,
		dtNavMeshQuery*           Query,
		const dtQueryFilter*      Filter,
		const float               Extent[3])
	{
		if (!Query || !Filter || !Extent || SideDir.IsNearlyZero() || RequestedWidth <= KINDA_SMALL_NUMBER)
		{
			return 0.0f;
		}

		for (const float OffsetScale : GOffsetScales)
		{
			const FVector Candidate = PeakLoc + (SideDir * RequestedWidth * OffsetScale);
			if (ValidateNavMeshSegment(PrevAnchor, Candidate, Query, Filter, Extent) &&
				ValidateNavMeshSegment(Candidate, NextAnchor, Query, Filter, Extent))
			{
				return OffsetScale;
			}
		}

		return 0.0f;
	}

	static void BuildManeuverZones(
		const TArray<FNavPathPoint>& InPoints,
		int32                        NumMidPoints,
		float                        LaneOffset,
		dtNavMeshQuery*              Query,
		const dtQueryFilter*         Filter,
		const float                  Extent[3],
		bool                         bDbg,
		TArray<FAPSManeuverZone>&    OutZones,
		float&                       OutSourceTotalLen)
	{
		OutZones.Reset();
		OutSourceTotalLen = 0.0f;

		const int32 InNum = InPoints.Num();
		if (InNum < 3 || NumMidPoints <= 0 || FMath::IsNearlyZero(LaneOffset, KINDA_SMALL_NUMBER))
		{
			return;
		}

		TArray<float, TInlineAllocator<64>> CumulativeDist;
		float TotalPathLen = 0.0f;
		if (!BuildCumulativeDistances(InPoints, CumulativeDist, TotalPathLen))
		{
			return;
		}
		OutSourceTotalLen = TotalPathLen;

		const float LaneAbs = FMath::Abs(LaneOffset);
		constexpr float StrongEventThreshold = 0.35f;
		constexpr float MinWindowStrength = 0.20f;
		constexpr float MinSegmentLen = 8.0f;
		constexpr float MinSlopeDistance2D = 16.0f;
		constexpr float MinSlopeChangeForScore = 0.08f;
		constexpr float FullSlopeChangeForScore = 0.45f;
		constexpr float WeakCrossThreshold = 0.05f;
		constexpr float FreeSideSwitchMargin = 0.20f;
		constexpr float WindowLookDistanceMultiplier = 1.35f; // Distance-window anchors need broad context to capture full turn+slope transitions.
		constexpr float WindowLookDistanceMin = 110.0f;
		constexpr float WindowLookDistanceMax = 480.0f;
		const float WindowLookDistance = FMath::Clamp(LaneAbs * WindowLookDistanceMultiplier, WindowLookDistanceMin, WindowLookDistanceMax);

		constexpr float MergeGapLaneScale = 0.90f; // Nearby maneuver events should merge into one stable zone.
		constexpr float MergeGapMidpointScale = 16.0f;
		constexpr float MergeGapMin = 80.0f;
		constexpr float MergeGapMax = 260.0f;
		const float MergeGapDistance = FMath::Clamp(
			LaneAbs * MergeGapLaneScale + static_cast<float>(NumMidPoints) * MergeGapMidpointScale,
			MergeGapMin,
			MergeGapMax);

		constexpr float RequestedWidthMinScale = 0.72f; // Weak accepted zones still keep substantial lane width; strength mostly changes confidence/zone length.
		constexpr float RequestedWidthMaxScale = 1.0f;

		constexpr float ZoneHalfLengthLaneScale = 1.40f;
		constexpr float ZoneHalfLengthMidpointScale = 20.0f;
		constexpr float ZoneHalfLengthBaseBias = 90.0f;
		constexpr float ZoneHalfLengthStrengthMinScale = 0.78f;
		constexpr float ZoneHalfLengthStrengthMaxScale = 1.08f;
		constexpr float ZoneHalfLengthMin = 90.0f;
		constexpr float ZoneHalfLengthMax = 640.0f;
		auto ComputeSlopeGrade = [&](const FVector& A, const FVector& B) -> float
		{
			const float DistXY = FVector::Dist2D(A, B);
			if (DistXY < MinSlopeDistance2D)
			{
				return 0.0f;
			}
			return (B.Z - A.Z) / DistXY;
		};

		auto ComputeSlopeStrength = [&](float GradeChange) -> float
		{
			const float Denominator = FullSlopeChangeForScore - MinSlopeChangeForScore;
			if (Denominator <= KINDA_SMALL_NUMBER)
			{
				return 0.0f;
			}
			return FMath::Clamp((GradeChange - MinSlopeChangeForScore) / Denominator, 0.0f, 1.0f);
		};

		OutZones.Reserve(FMath::Min(InNum, 32));

		for (int32 i = 1; i < InNum - 1; ++i)
		{
			const FVector PrevLoc = InPoints[i - 1].Location;
			const FVector CurrLoc = InPoints[i].Location;
			const FVector NextLoc = InPoints[i + 1].Location;

			const FVector DirIn = (CurrLoc - PrevLoc).GetSafeNormal2D();
			const FVector DirOut = (NextLoc - CurrLoc).GetSafeNormal2D();
			const float LenIn = FVector::Dist2D(PrevLoc, CurrLoc);
			const float LenOut = FVector::Dist2D(CurrLoc, NextLoc);
			if (DirIn.IsNearlyZero() || DirOut.IsNearlyZero() || LenIn < MinSegmentLen || LenOut < MinSegmentLen)
			{
				continue;
			}

			const float DotInOut = FMath::Clamp(FVector::DotProduct(DirIn, DirOut), -1.0f, 1.0f);
			const float LocalXYStrength = FMath::Clamp(1.0f - DotInOut, 0.0f, 1.0f);
			const float LocalCrossZ = (DirIn.X * DirOut.Y) - (DirIn.Y * DirOut.X);

			const float PeakDist = CumulativeDist[i];
			const float PrevAnchorDist = FMath::Max(0.0f, PeakDist - WindowLookDistance);
			const float NextAnchorDist = FMath::Min(TotalPathLen, PeakDist + WindowLookDistance);
			const FVector PrevAnchor = GetPointAtPathDistance(InPoints, CumulativeDist, TotalPathLen, PrevAnchorDist);
			const FVector NextAnchor = GetPointAtPathDistance(InPoints, CumulativeDist, TotalPathLen, NextAnchorDist);

			const FVector WindowIn = (CurrLoc - PrevAnchor).GetSafeNormal2D();
			const FVector WindowOut = (NextAnchor - CurrLoc).GetSafeNormal2D();
			const bool bValidWindow = !WindowIn.IsNearlyZero() && !WindowOut.IsNearlyZero();
			const float WindowDot = bValidWindow ? FMath::Clamp(FVector::DotProduct(WindowIn, WindowOut), -1.0f, 1.0f) : DotInOut;
			const float WindowXYStrength = bValidWindow ? FMath::Clamp(1.0f - WindowDot, 0.0f, 1.0f) : LocalXYStrength;
			const float XYStrength = FMath::Lerp(LocalXYStrength, WindowXYStrength, 0.70f);

			const float LocalGradeIn = ComputeSlopeGrade(PrevLoc, CurrLoc);
			const float LocalGradeOut = ComputeSlopeGrade(CurrLoc, NextLoc);
			const float LocalSlopeChange = FMath::Abs(LocalGradeOut - LocalGradeIn);
			const float WindowGradeIn = ComputeSlopeGrade(PrevAnchor, CurrLoc);
			const float WindowGradeOut = ComputeSlopeGrade(CurrLoc, NextAnchor);
			const float WindowSlopeChange = FMath::Abs(WindowGradeOut - WindowGradeIn);
			const float SlopeStrength = FMath::Max(ComputeSlopeStrength(LocalSlopeChange), ComputeSlopeStrength(WindowSlopeChange));

			const bool bHasXY = XYStrength >= StrongEventThreshold;
			const bool bHasSlope = SlopeStrength >= StrongEventThreshold;
			if (WindowXYStrength < MinWindowStrength && LocalXYStrength < StrongEventThreshold && !bHasSlope)
			{
				continue;
			}

			if (!bHasXY && !bHasSlope)
			{
				continue;
			}

			EAPSManeuverZoneType ZoneType = EAPSManeuverZoneType::XYTurn;
			if (bHasXY && bHasSlope)
			{
				ZoneType = EAPSManeuverZoneType::Mixed;
			}
			else if (bHasSlope && (!bHasXY || FMath::Abs(LocalCrossZ) < WeakCrossThreshold))
			{
				ZoneType = EAPSManeuverZoneType::Slope;
			}

			const float Strength = FMath::Clamp(FMath::Max(XYStrength, SlopeStrength), 0.0f, 1.0f);
			const float RequestedWidth = LaneAbs * FMath::Lerp(RequestedWidthMinScale, RequestedWidthMaxScale, Strength);

			const float TypeLengthMultiplier =
				(ZoneType == EAPSManeuverZoneType::Mixed) ? 1.40f :
				(ZoneType == EAPSManeuverZoneType::Slope) ? 1.25f : 1.0f;
			const float ZoneHalfLength = FMath::Clamp(
				(LaneAbs * ZoneHalfLengthLaneScale + static_cast<float>(NumMidPoints) * ZoneHalfLengthMidpointScale + ZoneHalfLengthBaseBias) *
				TypeLengthMultiplier *
				FMath::Lerp(ZoneHalfLengthStrengthMinScale, ZoneHalfLengthStrengthMaxScale, Strength),
				ZoneHalfLengthMin,
				ZoneHalfLengthMax);

			const float StartDistance = FMath::Max(0.0f, PeakDist - ZoneHalfLength);
			const float EndDistance = FMath::Min(TotalPathLen, PeakDist + ZoneHalfLength);
			if (EndDistance - StartDistance <= KINDA_SMALL_NUMBER)
			{
				continue;
			}

			const FVector TravelDir2D = (NextAnchor - PrevAnchor).GetSafeNormal2D();
			const FVector FallbackDir = (NextLoc - PrevLoc).GetSafeNormal2D();
			const FVector DirForSides = !TravelDir2D.IsNearlyZero() ? TravelDir2D : (!FallbackDir.IsNearlyZero() ? FallbackDir : DirIn);
			const FVector TravelRight = FVector(-DirForSides.Y, DirForSides.X, 0.0f).GetSafeNormal2D();

			const FVector CornerRight = ComputeCornerRightVector(DirIn, DirOut, DirForSides);
			const FVector OutsideSide = (LocalCrossZ > 0.0f) ? CornerRight : -CornerRight;

			float RightFreeScale = 0.0f;
			float LeftFreeScale = 0.0f;
			if (!TravelRight.IsNearlyZero())
			{
				RightFreeScale = EvaluateManeuverSideFreeScale(CurrLoc, PrevAnchor, NextAnchor, TravelRight, RequestedWidth, Query, Filter, Extent);
				LeftFreeScale = EvaluateManeuverSideFreeScale(CurrLoc, PrevAnchor, NextAnchor, -TravelRight, RequestedWidth, Query, Filter, Extent);
			}

			FVector SelectedSide = OutsideSide;
			if (ZoneType == EAPSManeuverZoneType::Slope)
			{
				SelectedSide = (RightFreeScale >= LeftFreeScale) ? TravelRight : -TravelRight;
				if (SelectedSide.IsNearlyZero())
				{
					SelectedSide = !OutsideSide.IsNearlyZero() ? OutsideSide : TravelRight;
				}
			}
			else if (ZoneType == EAPSManeuverZoneType::Mixed)
			{
				const bool bPreferRightFree = (RightFreeScale > LeftFreeScale + FreeSideSwitchMargin);
				const bool bPreferLeftFree = (LeftFreeScale > RightFreeScale + FreeSideSwitchMargin);
				if (bPreferRightFree)
				{
					SelectedSide = TravelRight;
				}
				else if (bPreferLeftFree)
				{
					SelectedSide = -TravelRight;
				}
				else
				{
					SelectedSide = !OutsideSide.IsNearlyZero()
						? OutsideSide
						: ((RightFreeScale >= LeftFreeScale) ? TravelRight : -TravelRight);
				}
			}
			else
			{
				SelectedSide = !OutsideSide.IsNearlyZero()
					? OutsideSide
					: ((RightFreeScale >= LeftFreeScale) ? TravelRight : -TravelRight);
				if (SelectedSide.IsNearlyZero())
				{
					SelectedSide = (RightFreeScale >= LeftFreeScale) ? TravelRight : -TravelRight;
				}
			}

			if (SelectedSide.IsNearlyZero())
			{
				continue;
			}

			FAPSManeuverZone NewZone;
			NewZone.StartDistance = StartDistance;
			NewZone.PeakDistance = PeakDist;
			NewZone.EndDistance = EndDistance;
			NewZone.Strength = Strength;
			NewZone.RequestedWidth = RequestedWidth;
			NewZone.SideDir = SelectedSide.GetSafeNormal2D();
			NewZone.Type = ZoneType;
			OutZones.Add(NewZone);

			if (bDbg)
			{
				UE_LOG(LogAPSNav, Log,
					TEXT("LowLevelArc: zone create type=%s start=%.1f peak=%.1f end=%.1f strength=%.3f req=%.1f side=(%.2f,%.2f,%.2f) freeL=%.3f freeR=%.3f"),
					ManeuverZoneTypeToString(NewZone.Type),
					NewZone.StartDistance,
					NewZone.PeakDistance,
					NewZone.EndDistance,
					NewZone.Strength,
					NewZone.RequestedWidth,
					NewZone.SideDir.X,
					NewZone.SideDir.Y,
					NewZone.SideDir.Z,
					LeftFreeScale,
					RightFreeScale);
			}
		}

		if (OutZones.Num() <= 1)
		{
			return;
		}

		TArray<FAPSManeuverZone> Merged;
		Merged.Reserve(OutZones.Num());
		FAPSManeuverZone Current = OutZones[0];

		for (int32 Index = 1; Index < OutZones.Num(); ++Index)
		{
			const FAPSManeuverZone& Candidate = OutZones[Index];
			const bool bShouldMerge = Candidate.StartDistance <= (Current.EndDistance + MergeGapDistance);
			if (!bShouldMerge)
			{
				Merged.Add(Current);
				Current = Candidate;
				continue;
			}

			FAPSManeuverZone BeforeMerge = Current;
			Current.StartDistance = FMath::Min(Current.StartDistance, Candidate.StartDistance);
			Current.EndDistance = FMath::Max(Current.EndDistance, Candidate.EndDistance);
			Current.Strength = FMath::Max(Current.Strength, Candidate.Strength);
			Current.RequestedWidth = FMath::Max(Current.RequestedWidth, Candidate.RequestedWidth);
			if (Candidate.Strength >= BeforeMerge.Strength)
			{
				Current.PeakDistance = Candidate.PeakDistance;
			}

			Current.Type = (Current.Type == Candidate.Type) ? Current.Type : EAPSManeuverZoneType::Mixed;

			const float CurrentWeight = FMath::Max(GManeuverZoneMinWeightStrength, BeforeMerge.Strength * BeforeMerge.RequestedWidth);
			const float CandidateWeight = FMath::Max(GManeuverZoneMinWeightStrength, Candidate.Strength * Candidate.RequestedWidth);
			const FVector BlendedSide = (BeforeMerge.SideDir * CurrentWeight) + (Candidate.SideDir * CandidateWeight);
			if (!BlendedSide.IsNearlyZero())
			{
				Current.SideDir = BlendedSide.GetSafeNormal2D();
			}
			else if (CandidateWeight > CurrentWeight)
			{
				Current.SideDir = Candidate.SideDir;
			}

			if (bDbg)
			{
				UE_LOG(LogAPSNav, Log,
					TEXT("LowLevelArc: zone merge A[%s %.1f/%.1f/%.1f] + B[%s %.1f/%.1f/%.1f] -> [%s %.1f/%.1f/%.1f req=%.1f str=%.3f side=(%.2f,%.2f,%.2f)]"),
					ManeuverZoneTypeToString(BeforeMerge.Type),
					BeforeMerge.StartDistance,
					BeforeMerge.PeakDistance,
					BeforeMerge.EndDistance,
					ManeuverZoneTypeToString(Candidate.Type),
					Candidate.StartDistance,
					Candidate.PeakDistance,
					Candidate.EndDistance,
					ManeuverZoneTypeToString(Current.Type),
					Current.StartDistance,
					Current.PeakDistance,
					Current.EndDistance,
					Current.RequestedWidth,
					Current.Strength,
					Current.SideDir.X,
					Current.SideDir.Y,
					Current.SideDir.Z);
			}
		}

		Merged.Add(Current);
		OutZones = MoveTemp(Merged);
	}

	static int32 AppendDirectCornerSideRouteZones(
		const TArray<FNavPathPoint>& InPoints,
		float                        LaneOffset,
		dtNavMeshQuery*              Query,
		const dtQueryFilter*         Filter,
		const float                  Extent[3],
		bool                         bDbg,
		const TArray<FAPSManeuverZone>& ExistingZones,
		TArray<FAPSManeuverZone>&    InOutZones)
	{
		const int32 InNum = InPoints.Num();
		if (InNum < 3)
		{
			return 0;
		}

		TArray<float, TInlineAllocator<64>> CumulativeDist;
		float TotalPathLen = 0.0f;
		if (!BuildCumulativeDistances(InPoints, CumulativeDist, TotalPathLen))
		{
			return 0;
		}

		const float LaneAbs = FMath::Abs(LaneOffset);
		const float HalfLength = FMath::Clamp(LaneAbs * 0.90f, 150.0f, 700.0f);
		const float AnchorLookDistance = FMath::Clamp(
			LaneAbs * GDirectCornerBlockedAnchorDistanceScale,
			GDirectCornerBlockedAnchorDistanceMin,
			GDirectCornerBlockedAnchorDistanceMax);
		constexpr float MinBlockedAnchorSpan = 120.0f;

		int32 AddedCount = 0;
		for (int32 i = 1; i < InNum - 1; ++i)
		{
			const FVector PrevLoc = InPoints[i - 1].Location;
			const FVector CurrLoc = InPoints[i].Location;
			const FVector NextLoc = InPoints[i + 1].Location;

			const FVector DirIn = (CurrLoc - PrevLoc).GetSafeNormal2D();
			const FVector DirOut = (NextLoc - CurrLoc).GetSafeNormal2D();
			if (DirIn.IsNearlyZero() || DirOut.IsNearlyZero())
			{
				continue;
			}

			const float DotInOut = FMath::Clamp(FVector::DotProduct(DirIn, DirOut), -1.0f, 1.0f);
			const float CornerStrength = FMath::Clamp(1.0f - DotInOut, 0.0f, 1.0f);
			const bool bAngleCorner = CornerStrength >= GDirectCornerStrengthMin;

			const float PeakDist = CumulativeDist[i];
			const float PrevAnchorDist = FMath::Max(0.0f, PeakDist - AnchorLookDistance);
			const float NextAnchorDist = FMath::Min(TotalPathLen, PeakDist + AnchorLookDistance);
			const FVector PrevAnchor = GetPointAtPathDistance(InPoints, CumulativeDist, TotalPathLen, PrevAnchorDist);
			const FVector NextAnchor = GetPointAtPathDistance(InPoints, CumulativeDist, TotalPathLen, NextAnchorDist);
			const float AnchorSpan = FVector::Dist2D(PrevAnchor, NextAnchor);
			const float MidDeviationFromAnchorChord = ComputePointDeviationFromSegment2D(PrevAnchor, NextAnchor, CurrLoc);
			const float MeaningfulDeviationMin = FMath::Clamp(LaneAbs * 0.08f, 24.0f, 80.0f);
			const float AlmostStraightDeviationMax = FMath::Clamp(LaneAbs * 0.05f, 16.0f, 60.0f);
			const bool bHasMeaningfulDeviation = MidDeviationFromAnchorChord >= MeaningfulDeviationMin;
			const bool bAlmostStraight = CornerStrength < 0.025f && MidDeviationFromAnchorChord <= AlmostStraightDeviationMax;
			const bool bCanValidateAnchors = Query && Filter && Extent;
			const bool bBlockedCorner =
				bCanValidateAnchors &&
				AnchorSpan >= MinBlockedAnchorSpan &&
				!ValidateNavMeshSegment(PrevAnchor, NextAnchor, Query, Filter, Extent);
			const bool bBlockedObstacleCorner =
				bBlockedCorner &&
				bHasMeaningfulDeviation &&
				CornerStrength >= 0.03f;

			if (bAlmostStraight)
			{
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: direct-corner skipped idx=%d peak=%.1f almost-straight strength=%.3f deviation=%.1f threshold=%.1f blocked=%s"),
						i,
						PeakDist,
						CornerStrength,
						MidDeviationFromAnchorChord,
						AlmostStraightDeviationMax,
						bBlockedCorner ? TEXT("true") : TEXT("false"));
				}
				continue;
			}

			if (!bAngleCorner && !bBlockedObstacleCorner)
			{
				if (bDbg && bBlockedCorner)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: direct-corner skipped idx=%d peak=%.1f blocked-without-deviation strength=%.3f deviation=%.1f needDeviation=%.1f"),
						i,
						PeakDist,
						CornerStrength,
						MidDeviationFromAnchorChord,
						MeaningfulDeviationMin);
				}
				continue;
			}

			bool bCoveredByStrongZone = false;
			for (const FAPSManeuverZone& Zone : ExistingZones)
			{
				if (PeakDist >= Zone.StartDistance && PeakDist <= Zone.EndDistance && Zone.Strength >= GManeuverSideRouteMinStrength)
				{
					bCoveredByStrongZone = true;
					break;
				}
			}
			if (bCoveredByStrongZone)
			{
				continue;
			}

			const float CrossZ = (DirIn.X * DirOut.Y) - (DirIn.Y * DirOut.X);
			const FVector AnchorSpanDir = (NextAnchor - PrevAnchor).GetSafeNormal2D();
			FVector SpanDir = AnchorSpanDir;
			if (SpanDir.IsNearlyZero())
			{
				SpanDir = (NextLoc - PrevLoc).GetSafeNormal2D();
			}
			const FVector CornerRight = ComputeCornerRightVector(DirIn, DirOut, SpanDir);
			FVector OutsideSide = FVector::ZeroVector;
			if (FMath::Abs(CrossZ) > KINDA_SMALL_NUMBER)
			{
				OutsideSide = (CrossZ > 0.0f) ? CornerRight : -CornerRight;
			}
			if (OutsideSide.IsNearlyZero())
			{
				// 90-degree 2D rotation gives us a stable fallback side when the corner winding is
				// too weak to determine a reliable outside direction.
				const FVector PerpendicularDir(-SpanDir.Y, SpanDir.X, 0.0f);
				OutsideSide = PerpendicularDir.GetSafeNormal2D();
			}
			if (OutsideSide.IsNearlyZero())
			{
				continue;
			}

			FAPSManeuverZone CornerZone;
			CornerZone.StartDistance = FMath::Max(0.0f, PeakDist - HalfLength);
			CornerZone.PeakDistance = PeakDist;
			CornerZone.EndDistance = FMath::Min(TotalPathLen, PeakDist + HalfLength);
			// Direct-corner candidates should reliably reach the side-route attempt stage once
			// detected; blocked anchor visibility is treated as strong obstacle evidence only when
			// the local path shape has meaningful turn/deviation characteristics.
			CornerZone.Strength = bBlockedObstacleCorner ? 1.0f : FMath::Max(CornerStrength, GManeuverSideRouteMinStrength);
			CornerZone.RequestedWidth = LaneAbs;
			CornerZone.SideDir = OutsideSide.GetSafeNormal2D();
			CornerZone.Type = EAPSManeuverZoneType::XYTurn;
			CornerZone.bFromDirectCorner = true;
			if (CornerZone.EndDistance > CornerZone.StartDistance)
			{
				InOutZones.Add(CornerZone);
				++AddedCount;
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: direct-corner candidate idx=%d peak=%.1f strength=%.3f angle=%s blocked=%s blockedObstacle=%s deviation=%.1f"),
						i,
						PeakDist,
						CornerStrength,
						bAngleCorner ? TEXT("true") : TEXT("false"),
						bBlockedCorner ? TEXT("true") : TEXT("false"),
						bBlockedObstacleCorner ? TEXT("true") : TEXT("false"),
						MidDeviationFromAnchorChord);
				}
			}
		}

		if (AddedCount > 0)
		{
			InOutZones.Sort([](const FAPSManeuverZone& A, const FAPSManeuverZone& B)
			{
				return A.PeakDistance < B.PeakDistance;
			});
		}
		return AddedCount;
	}

	static int32 AppendSyntheticFallbackSideRouteZone(
		const TArray<FNavPathPoint>& InPoints,
		float                        LaneOffset,
		TArray<FAPSManeuverZone>&    InOutZones)
	{
		if (InPoints.Num() < 2)
		{
			return 0;
		}

		TArray<float, TInlineAllocator<64>> CumulativeDist;
		float TotalPathLen = 0.0f;
		if (!BuildCumulativeDistances(InPoints, CumulativeDist, TotalPathLen))
		{
			return 0;
		}

		const float StartDistance = TotalPathLen * GSyntheticFallbackStartRatio;
		const float PeakDistance = TotalPathLen * GSyntheticFallbackPeakRatio;
		const float EndDistance = TotalPathLen * GSyntheticFallbackEndRatio;
		if (EndDistance - StartDistance <= KINDA_SMALL_NUMBER)
		{
			return 0;
		}

		const FVector StartLoc = GetPointAtPathDistance(InPoints, CumulativeDist, TotalPathLen, StartDistance);
		const FVector EndLoc = GetPointAtPathDistance(InPoints, CumulativeDist, TotalPathLen, EndDistance);
		FVector SpanDir = (EndLoc - StartLoc).GetSafeNormal2D();
		if (SpanDir.IsNearlyZero())
		{
			// Sparse/shortcut paths can collapse the interior sample span, so fall back to the
			// overall path direction before giving up on the synthetic candidate.
			SpanDir = (InPoints.Last().Location - InPoints[0].Location).GetSafeNormal2D();
		}
		if (SpanDir.IsNearlyZero())
		{
			return 0;
		}

		const FVector SideDir = FVector(-SpanDir.Y, SpanDir.X, 0.0f).GetSafeNormal2D();
		if (SideDir.IsNearlyZero())
		{
			return 0;
		}

		FAPSManeuverZone FallbackZone;
		FallbackZone.StartDistance = StartDistance;
		FallbackZone.PeakDistance = PeakDistance;
		FallbackZone.EndDistance = EndDistance;
		FallbackZone.Strength = 1.0f;
		FallbackZone.RequestedWidth = FMath::Abs(LaneOffset);
		FallbackZone.SideDir = SideDir;
		FallbackZone.Type = EAPSManeuverZoneType::XYTurn;
		FallbackZone.bFromSyntheticFallback = true;
		InOutZones.Add(FallbackZone);
		return 1;
	}

	/**
	 * How far from the center line should we be at parameter t [0..1].
	 * SmoothCurve  = one bell-shaped bump.
	 * Wavy         = several bumps along the way.
	 */
	static float ComputeArcShape(
		float               t,
		int32               NumMidPoints,
		EAPSArcMidpointMode Mode)
	{
		// Outer bell: always zero at ends and max in the middle.
		const float Outer = FMath::Sin(PI * t); // 0 -> 1 -> 0
		if (Outer <= 0.0f)
		{
			return 0.0f;
		}

		if (Mode == EAPSArcMidpointMode::SmoothCurve)
		{
			const int32 Detail = FMath::Clamp(NumMidPoints, 1, 6);
			const float Exp    = 1.0f + (static_cast<float>(Detail) - 1.0f) * 0.5f; // 1.0, 1.5, 2.0 ...

			// Higher exponent = more “round” belly and softer near ends.
			return FMath::Pow(Outer, Exp);
		}
		else
		{
			const int32 Humps = FMath::Clamp(NumMidPoints, 1, 6);
			const float Wave  = FMath::Sin(static_cast<float>(Humps) * PI * t);
			const float Inner = FMath::Abs(Wave);

			// Outer keeps zero at ends, Inner adds humps in the middle.
			return Outer * Inner;
		}
	}

	/**
	 * Left/right sign for a given t.
	 * SmoothCurve: always one side.
	 * Wavy: flip sign a few times so the path snakes around the center.
	 */
	static float ComputeLocalSideSign(
		float               GlobalSideSign,
		float               t,
		int32               NumMidPoints,
		EAPSArcMidpointMode Mode)
	{
		if (Mode != EAPSArcMidpointMode::Wavy)
		{
			return GlobalSideSign;
		}

		const int32 Humps = FMath::Clamp(NumMidPoints, 1, 6);
		const float Wave  = FMath::Sin(static_cast<float>(Humps) * PI * t);

		if (FMath::IsNearlyZero(Wave))
		{
			return GlobalSideSign;
		}

		// Wave says left or right, global sign says which side is “positive”.
		return GlobalSideSign * FMath::Sign(Wave);
	}

	/**
	 * For each ManeuverZone that exceeds the large-offset thresholds, attempt to inject a local
	 * side-route (ZoneStartLoc -> SideWaypoint -> ZoneEndLoc) into the base Detour path before
	 * arc-warp / fillet processing.
	 *
	 * Pipeline position:
	 *   BuildBaseDetourPath -> BuildManeuverZones -> TryInjectManeuverSideRoutes
	 *     -> BuildAdaptiveCornerFilletPath -> BuildArcSamplingPath -> WarpPathToArc
	 *
	 * Zones are processed last-to-first so that earlier splice indices remain valid after each
	 * insertion.  If a zone's injection fails, the path is left unchanged and the existing
	 * per-point offset / shrink fallback inside WarpPathToArc handles it.
	 */
	static void TryInjectManeuverSideRoutes(
		TArray<FNavPathPoint>&          InOutPoints,
		const TArray<FAPSManeuverZone>& ManeuverZones,
		float                           ManeuverSourceTotalLen,
		float                           LaneOffset,
		FAPSNavMeshQuery&               APSQuery,
		const dtQueryFilter*            DtFilter,
		const float                     Extent[3],
		float                           CostLimit,
		bool                            bRespectNavAreaCosts,
		bool                            bDbg,
		TArray<FAPSSideRouteDebugInfo>* OutDebugInfos,
		FAPSSideRouteStats*             OutStats)
	{
		if (OutStats)
		{
			*OutStats = FAPSSideRouteStats();
			OutStats->CandidateCount = ManeuverZones.Num();
		}

		// Early-out: injection is only meaningful for large lane offsets.
		if (FMath::Abs(LaneOffset) < GManeuverSideRouteMinLaneOffset)
		{
			return;
		}

		// 2-point paths are supported: when there is no interior replace range, we use insert fallback.
		if (InOutPoints.Num() < 2 || ManeuverZones.Num() == 0)
		{
			return;
		}

		if (!APSQuery.InternalQuery || !DtFilter)
		{
			return;
		}

		dtNavMeshQuery* Query    = APSQuery.InternalQuery;
		const float     LaneSign = FMath::Sign(LaneOffset);
		int32 CandidateDebugCount = 0;
		int32 FailedDebugCount = 0;
		if (OutDebugInfos)
		{
			for (const FAPSSideRouteDebugInfo& DebugInfo : *OutDebugInfos)
			{
				if (DebugInfo.bCandidateOnly)
				{
					++CandidateDebugCount;
				}
				else if (!DebugInfo.bInjected)
				{
					++FailedDebugCount;
				}
			}
		}

		// Build initial cumulative distances for the current path.
		TArray<float, TInlineAllocator<GManeuverSideRouteBasePathCap>> CumDist;
		float TotalLen = 0.0f;
		if (!BuildCumulativeDistances(InOutPoints, CumDist, TotalLen))
		{
			return;
		}

		// Scale zone distances from ManeuverSourceTotalLen to current TotalLen (defensive; should
		// be ~1.0 when called directly after BuildBaseDetourPath, before any resampling).
		const float DistanceScale =
			(ManeuverSourceTotalLen > KINDA_SMALL_NUMBER && TotalLen > KINDA_SMALL_NUMBER)
			? (TotalLen / ManeuverSourceTotalLen)
			: 1.0f;

		// Work on a scaled copy so we never mutate the caller's zone list.
		TArray<FAPSManeuverZone> ScaledZones = ManeuverZones;
		for (FAPSManeuverZone& Z : ScaledZones)
		{
			Z.StartDistance = FMath::Clamp(Z.StartDistance * DistanceScale, 0.0f, TotalLen);
			Z.PeakDistance  = FMath::Clamp(Z.PeakDistance  * DistanceScale, 0.0f, TotalLen);
			Z.EndDistance   = FMath::Clamp(Z.EndDistance   * DistanceScale, 0.0f, TotalLen);
			Z.PeakDistance  = FMath::Clamp(Z.PeakDistance, Z.StartDistance, Z.EndDistance);
		}

		bool bAnyInjected = false;

		// Process zones from last to first: injecting a later zone does not affect the cumulative
		// distances of earlier (smaller-index) zones because those points are untouched.
		for (int32 ZoneIdx = ScaledZones.Num() - 1; ZoneIdx >= 0; --ZoneIdx)
		{
			const FAPSManeuverZone& Zone = ScaledZones[ZoneIdx];

			// Rebuild cumulative distances if a previous injection changed the path.
			if (bAnyInjected)
			{
				CumDist.Reset();
				TotalLen = 0.0f;
				if (!BuildCumulativeDistances(InOutPoints, CumDist, TotalLen))
				{
					break;
				}
			}

			const int32 NumPts = InOutPoints.Num();

			const FVector ZoneStartLoc = GetPointAtPathDistance(InOutPoints, CumDist, TotalLen, Zone.StartDistance);
			const FVector ZonePeakLoc  = GetPointAtPathDistance(InOutPoints, CumDist, TotalLen, Zone.PeakDistance);
			const FVector ZoneEndLoc   = GetPointAtPathDistance(InOutPoints, CumDist, TotalLen, Zone.EndDistance);

			if (OutDebugInfos && CandidateDebugCount < GManeuverSideRouteMaxCandidateDebugOverlays)
			{
				FAPSSideRouteDebugInfo& CandidateDebugInfo = OutDebugInfos->AddDefaulted_GetRef();
				CandidateDebugInfo.ZoneStartLoc = ZoneStartLoc;
				CandidateDebugInfo.ZonePeakLoc = ZonePeakLoc;
				CandidateDebugInfo.ZoneEndLoc = ZoneEndLoc;
				CandidateDebugInfo.CandidateWidth = Zone.RequestedWidth;
				CandidateDebugInfo.ZoneType = Zone.Type;
				CandidateDebugInfo.bCandidateOnly = true;
				CandidateDebugInfo.bFromDirectCorner = Zone.bFromDirectCorner;
				CandidateDebugInfo.bFromSyntheticFallback = Zone.bFromSyntheticFallback;
				++CandidateDebugCount;
			}

			// ---- eligibility ------------------------------------------------
			if (Zone.RequestedWidth < GManeuverSideRouteMinRequestedWidth)
			{
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: side-route zone %d skip (RequestedWidth=%.1f < min=%.1f)"),
						ZoneIdx, Zone.RequestedWidth, GManeuverSideRouteMinRequestedWidth);
				}
				continue;
			}

			if (Zone.Strength < GManeuverSideRouteMinStrength)
			{
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: side-route zone %d skip (Strength=%.3f < min=%.3f)"),
						ZoneIdx, Zone.Strength, GManeuverSideRouteMinStrength);
				}
				continue;
			}

			if (Zone.SideDir.IsNearlyZero())
			{
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: side-route zone %d skip (SideDir is zero)"),
						ZoneIdx);
				}
				continue;
			}

			if (bDbg)
			{
				UE_LOG(LogAPSNav, Log,
					TEXT("LowLevelArc: side-route eligible zone %d type=%s start=%.1f peak=%.1f end=%.1f req=%.1f strength=%.3f side=(%.2f,%.2f)"),
					ZoneIdx,
					ManeuverZoneTypeToString(Zone.Type),
					Zone.StartDistance, Zone.PeakDistance, Zone.EndDistance,
					Zone.RequestedWidth, Zone.Strength,
					Zone.SideDir.X, Zone.SideDir.Y);
			}

			// ---- splice indices --------------------------------------------
			// startSpliceIdx: first interior point at or beyond Zone.StartDistance.
			int32 startSpliceIdx = 1;
			while (startSpliceIdx < NumPts - 1 && CumDist[startSpliceIdx] < Zone.StartDistance)
			{
				++startSpliceIdx;
			}

			// endSpliceIdx: last interior point at or before Zone.EndDistance.
			int32 endSpliceIdx = NumPts - 2;
			while (endSpliceIdx > 0 && CumDist[endSpliceIdx] > Zone.EndDistance)
			{
				--endSpliceIdx;
			}

			// Fallback for sparse paths: if no valid interior replace range exists, insert route
			// between surrounding points instead of skipping.
			const bool bCanReplaceRange =
				(startSpliceIdx <= endSpliceIdx) &&
				(startSpliceIdx >= 1) &&
				(endSpliceIdx <= NumPts - 2);
			const int32 InsertIdx = FMath::Clamp(startSpliceIdx, 1, NumPts - 1);

			// ---- candidate widths: relative scales then absolute fallbacks ----
			const int32 NumScales       = UE_ARRAY_COUNT(GOffsetScales);
			const int32 NumAbsFallbacks = UE_ARRAY_COUNT(GArcAbsFallbackAmps);
			const float MinRelWidth     = Zone.RequestedWidth * GOffsetScales[NumScales - 1];

			bool bInjected = false;
			bool bTriedOppositeSide = false;
			bool bHasPendingFailedDebug = false;
			FAPSSideRouteDebugInfo PendingFailedDebugInfo;

			for (int32 CandIdx = 0; CandIdx < NumScales + NumAbsFallbacks && !bInjected; ++CandIdx)
			{
				float CandidateWidth;
				bool  bIsAbsFallback;

				if (CandIdx < NumScales)
				{
					CandidateWidth = Zone.RequestedWidth * GOffsetScales[CandIdx];
					bIsAbsFallback = false;
				}
				else
				{
					const float AbsWidth = GArcAbsFallbackAmps[CandIdx - NumScales];
					if (AbsWidth >= MinRelWidth)
					{
						continue; // already covered by relative-scale loop
					}
					CandidateWidth = AbsWidth;
					bIsAbsFallback = true;
				}

				if (CandidateWidth <= KINDA_SMALL_NUMBER)
				{
					continue;
				}

				for (int32 SideTry = 0; SideTry < GManeuverSideRouteMaxSideTries && !bInjected; ++SideTry)
				{
					const bool bUseOppositeSide = (SideTry == 1);
					if (bUseOppositeSide)
					{
						bTriedOppositeSide = true;
					}

					if (OutStats)
					{
						++OutStats->AttemptedCount;
					}

					const FVector SideDirForTry = (bUseOppositeSide ? -Zone.SideDir : Zone.SideDir);
					const FVector RawSideWpt = ZonePeakLoc + SideDirForTry * LaneSign * CandidateWidth;
					FVector SideWpt = RawSideWpt;
					FString FailureReason;
					dtPolyRef WptRef = 0;
					float     WptNearest[3];
					if (!FindNearestPoly(Query, DtFilter, Extent, RawSideWpt, WptRef, WptNearest))
					{
						FailureReason = TEXT("no-navmesh-poly");
					}
					else
					{
						SideWpt = Recast2UnrealPoint(WptNearest);
					}

					// Optional nav-area cost check: reject side waypoints in higher-cost regions.
					if (FailureReason.IsEmpty() && bRespectNavAreaCosts)
					{
						dtPolyRef PeakRef = 0;
						float     PeakNearest[3];
						float     PeakAreaCost = 0.0f;
						float     WptAreaCost  = 0.0f;

						const bool bHasPeakCost =
							FindNearestPoly(Query, DtFilter, Extent, ZonePeakLoc, PeakRef, PeakNearest) &&
							TryGetAreaCostFromPolyRef(Query, DtFilter, PeakRef, PeakAreaCost);
						const bool bHasWptCost  = TryGetAreaCostFromPolyRef(Query, DtFilter, WptRef, WptAreaCost);
						if (bHasPeakCost && bHasWptCost && WptAreaCost > PeakAreaCost + GNavAreaCostTolerance)
						{
							FailureReason = TEXT("area-cost");
						}
					}

					// ---- direct segment validation ---------------------------------
					const bool bStartOk = FailureReason.IsEmpty() && ValidateNavMeshSegment(ZoneStartLoc, SideWpt, Query, DtFilter, Extent);
					const bool bEndOk   = FailureReason.IsEmpty() && ValidateNavMeshSegment(SideWpt, ZoneEndLoc, Query, DtFilter, Extent);

					if (bDbg)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: side-route zone %d cand %.0f (%s,%s): direct start->wp=%s wp->end=%s"),
							ZoneIdx, CandidateWidth, bIsAbsFallback ? TEXT("abs") : TEXT("rel"),
							bUseOppositeSide ? TEXT("opposite") : TEXT("preferred"),
							bStartOk ? TEXT("ok") : TEXT("fail"),
							bEndOk   ? TEXT("ok") : TEXT("fail"));
					}

					TArray<FNavPathPoint> InjectedRoute;
					bool                  bRouteOk = false;

					if (bStartOk && bEndOk)
					{
						// Simple 3-point route: ZoneStart -> SideWaypoint -> ZoneEnd.
						InjectedRoute.Reset(3);
						FNavPathPoint WP0(ZoneStartLoc), WP1(SideWpt), WP2(ZoneEndLoc);
						WP1.NodeRef = WptRef;
						InjectedRoute.Add(WP0);
						InjectedRoute.Add(WP1);
						InjectedRoute.Add(WP2);
						bRouteOk = true;
					}
					else if (FailureReason.IsEmpty())
					{
						// ---- local Detour pathfinding fallback ----------------------
						TArray<FNavPathPoint> Seg1Pts, Seg2Pts;
						TArray<dtPolyRef>     Seg1Cor, Seg2Cor;
						bool                  bSeg1Partial = false, bSeg2Partial = false;

						const bool bSeg1Ok = BuildBaseDetourPath(
							APSQuery, DtFilter, Extent,
							ZoneStartLoc, SideWpt,
							CostLimit, false /*bAllowRaycastShortcut*/,
							Seg1Cor, Seg1Pts, bSeg1Partial);

						const bool bSeg2Ok =
							bSeg1Ok && !bSeg1Partial &&
							BuildBaseDetourPath(
								APSQuery, DtFilter, Extent,
								SideWpt, ZoneEndLoc,
								CostLimit, false,
								Seg2Cor, Seg2Pts, bSeg2Partial);

						if (bDbg)
						{
							UE_LOG(LogAPSNav, Log,
								TEXT("LowLevelArc: side-route zone %d cand %.0f (%s): local path seg1=%s(%d pts) seg2=%s(%d pts)"),
								ZoneIdx, CandidateWidth, bUseOppositeSide ? TEXT("opposite") : TEXT("preferred"),
								bSeg1Ok ? TEXT("ok") : TEXT("fail"), Seg1Pts.Num(),
								bSeg2Ok ? TEXT("ok") : TEXT("fail"), Seg2Pts.Num());
						}

						if (bSeg1Ok && bSeg2Ok && !bSeg1Partial && !bSeg2Partial)
						{
							// Sanity-check: side route must not be excessively longer than the
							// straight zone span (guards against going the long way around).
							auto CalcPathLen2D = [](const TArray<FNavPathPoint>& Pts) -> float
							{
								float Len = 0.0f;
								for (int32 Pi = 1; Pi < Pts.Num(); ++Pi)
								{
									Len += FVector::Dist2D(Pts[Pi - 1].Location, Pts[Pi].Location);
								}
								return Len;
							};
							const float TotalSideLen = CalcPathLen2D(Seg1Pts) + CalcPathLen2D(Seg2Pts);
							const float DirectLen    = FVector::Dist2D(ZoneStartLoc, ZoneEndLoc);

							if (TotalSideLen <= DirectLen * GManeuverSideRouteMaxLengthRatio + GManeuverSideRouteMaxLengthOffset)
							{
								InjectedRoute.Reset(Seg1Pts.Num() + Seg2Pts.Num());
								for (const FNavPathPoint& P : Seg1Pts)
								{
									AddPointIfNotNear2D(InjectedRoute, P.Location, static_cast<dtPolyRef>(P.NodeRef));
								}
								for (int32 Pi = 1; Pi < Seg2Pts.Num(); ++Pi)
								{
									AddPointIfNotNear2D(InjectedRoute, Seg2Pts[Pi].Location, static_cast<dtPolyRef>(Seg2Pts[Pi].NodeRef));
								}
								if (InjectedRoute.Num() >= 2)
								{
									bRouteOk = true;
								}
							}
							else
							{
								FailureReason = TEXT("too-long");
							}
						}
						else
						{
							FailureReason = TEXT("local-path-fail");
						}
					}

					if (bRouteOk && !ValidatePathOnNavMesh(InjectedRoute, Query, DtFilter, Extent))
					{
						bRouteOk = false;
						FailureReason = TEXT("segment-validation");
					}

					if (!bRouteOk || InjectedRoute.Num() < 2)
					{
						PendingFailedDebugInfo.ZoneStartLoc = ZoneStartLoc;
						PendingFailedDebugInfo.ZonePeakLoc = ZonePeakLoc;
						PendingFailedDebugInfo.ZoneEndLoc = ZoneEndLoc;
						PendingFailedDebugInfo.SideWaypoint = SideWpt;
						PendingFailedDebugInfo.CandidateWidth = CandidateWidth;
						PendingFailedDebugInfo.ZoneType = Zone.Type;
						PendingFailedDebugInfo.bInjected = false;
						PendingFailedDebugInfo.bTriedOppositeSide = bUseOppositeSide;
						PendingFailedDebugInfo.bFromDirectCorner = Zone.bFromDirectCorner;
						PendingFailedDebugInfo.bFromSyntheticFallback = Zone.bFromSyntheticFallback;
						PendingFailedDebugInfo.FailureReason = FailureReason.IsEmpty() ? TEXT("route-rejected") : FailureReason;
						bHasPendingFailedDebug = true;
						continue;
					}

					// ---- point-count budget check ----------------------------------
					if (InjectedRoute.Num() > GManeuverSideRouteMaxInjectedPts)
					{
						continue;
					}

					int32 EffectiveInsertCount = InjectedRoute.Num();
					if (!bCanReplaceRange)
					{
						const FVector PrevKeep = InOutPoints[InsertIdx - 1].Location;
						const FVector NextKeep = InOutPoints[InsertIdx].Location;
						int32 TrimStart = 0;
						while (TrimStart < InjectedRoute.Num() &&
							FVector::Dist2D(InjectedRoute[TrimStart].Location, PrevKeep) <= GMinPathPointDistance2D)
						{
							++TrimStart;
						}
						int32 TrimEnd = InjectedRoute.Num();
						while (TrimEnd > TrimStart &&
							FVector::Dist2D(InjectedRoute[TrimEnd - 1].Location, NextKeep) <= GMinPathPointDistance2D)
						{
							--TrimEnd;
						}

						if (TrimStart > 0 || TrimEnd < InjectedRoute.Num())
						{
							TArray<FNavPathPoint> TrimmedRoute;
							TrimmedRoute.Reserve(TrimEnd - TrimStart);
							for (int32 TrimIdx = TrimStart; TrimIdx < TrimEnd; ++TrimIdx)
							{
								TrimmedRoute.Add(InjectedRoute[TrimIdx]);
							}
							InjectedRoute = MoveTemp(TrimmedRoute);
						}
						EffectiveInsertCount = InjectedRoute.Num();
					}

					if (EffectiveInsertCount <= 0)
					{
						continue;
					}

					const int32 RemoveCount = bCanReplaceRange ? (endSpliceIdx - startSpliceIdx + 1) : 0;
					const int32 NewTotalPts = InOutPoints.Num() - RemoveCount + EffectiveInsertCount;
					if (NewTotalPts > GManeuverSideRouteBasePathCap)
					{
						continue;
					}

					if (bCanReplaceRange)
					{
						APSCompat::RemoveAtNoShrink(InOutPoints, startSpliceIdx, RemoveCount);
						InOutPoints.Insert(InjectedRoute.GetData(), EffectiveInsertCount, startSpliceIdx);
					}
					else
					{
						InOutPoints.Insert(InjectedRoute.GetData(), EffectiveInsertCount, InsertIdx);
					}

					bInjected    = true;
					bAnyInjected = true;
					if (OutStats)
					{
						++OutStats->InjectedCount;
					}

					if (OutDebugInfos)
					{
						FAPSSideRouteDebugInfo& InjectionDebugInfo = OutDebugInfos->AddDefaulted_GetRef();
						InjectionDebugInfo.ZoneStartLoc = ZoneStartLoc;
						InjectionDebugInfo.ZonePeakLoc = ZonePeakLoc;
						InjectionDebugInfo.ZoneEndLoc = ZoneEndLoc;
						InjectionDebugInfo.SideWaypoint = SideWpt;
						InjectionDebugInfo.CandidateWidth = CandidateWidth;
						InjectionDebugInfo.ZoneType = Zone.Type;
						InjectionDebugInfo.bInjected = true;
						InjectionDebugInfo.bTriedOppositeSide = bTriedOppositeSide;
						InjectionDebugInfo.bFromDirectCorner = Zone.bFromDirectCorner;
						InjectionDebugInfo.bFromSyntheticFallback = Zone.bFromSyntheticFallback;
						InjectionDebugInfo.InjectedPoints.Reserve(InjectedRoute.Num());
						for (const FNavPathPoint& Point : InjectedRoute)
						{
							InjectionDebugInfo.InjectedPoints.Add(Point.Location);
						}
					}

					if (bDbg)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: side-route zone %d INJECTED cand=%.0f side=%s source=%s injectedPts=%d removed=%d totalPts=%d"),
							ZoneIdx, CandidateWidth,
							bUseOppositeSide ? TEXT("opposite") : TEXT("preferred"),
							ManeuverZoneSourceToString(Zone),
							EffectiveInsertCount, RemoveCount, InOutPoints.Num());
					}
				} // end side try loop
			} // end candidate loop

			if (!bInjected)
			{
				if (OutStats)
				{
					++OutStats->FailedCount;
				}
				if (bHasPendingFailedDebug && OutDebugInfos && FailedDebugCount < GManeuverSideRouteMaxFailedDebugOverlays)
				{
					PendingFailedDebugInfo.bTriedOppositeSide = bTriedOppositeSide;
					OutDebugInfos->Add(PendingFailedDebugInfo);
					++FailedDebugCount;
				}

				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: side-route zone %d: no valid side route found (%s source), keeping per-point offset fallback"),
						ZoneIdx,
						ManeuverZoneSourceToString(Zone));
				}
			}
		} // end zone loop
	}

	/**
	 * The actual “bend this straight path into an arc” function.
	 *
	 * Important points:
	 *   * We never modify the original array until the very end.
	 *   * Every warped point is snapped back to the navmesh.
	 *   * If final shape fails raycast validation, we keep the straight path.
	 */
	static void WarpPathToArc(
		TArray<FNavPathPoint>& Points,
		float                  ArcStrength,
		int32                  NumMidPoints,
		EAPSArcMidpointMode    MidpointMode,
		float                  LaneOffset,
		dtNavMeshQuery*        Query,
		const dtQueryFilter*   Filter,
		const float            Extent[3],
		bool                   bRespectNavAreaCosts)
	{
		if (!Query || !Filter || !Extent)
		{
			return;
		}

		const bool bDbg = (CVarAPSNavDebug.GetValueOnAnyThread() != 0);
		// "Reduced offset" is anything below full-strength 1.0.
		const float FullOffsetScaleThreshold = 0.999f;

		const float StrengthAbs = FMath::Abs(ArcStrength);
		if (StrengthAbs < 0.1f)
		{
			return; // arc is basically disabled
		}

		// Step 1: resample path so that we have enough midpoints to actually bend.
		TArray<FAPSManeuverZone> ManeuverZones;
		float ManeuverSourceTotalLen = 0.0f;
		BuildManeuverZones(Points, NumMidPoints, LaneOffset, Query, Filter, Extent, bDbg, ManeuverZones, ManeuverSourceTotalLen);

		TArray<FNavPathPoint> FilletPoints;
		BuildAdaptiveCornerFilletPath(Points, NumMidPoints, LaneOffset, Query, Filter, Extent, bDbg, FilletPoints);

		TArray<FNavPathPoint> BasePoints;
		BuildArcSamplingPath(FilletPoints, NumMidPoints, BasePoints);

		const int32 NumPts = BasePoints.Num();
		if (NumPts <= 2)
		{
			return;
		}

		const FVector Start = BasePoints[0].Location;
		const FVector End   = BasePoints.Last().Location;

		const float Dist2D = FVector::Dist2D(Start, End);
		if (Dist2D < GetMinArcDistance())
		{
			return; // too short for any meaningful arc
		}

		// Work on a copy so we can bail out if something goes wrong.
		TArray<FNavPathPoint> WarpedPoints = BasePoints;

		TArray<float, TInlineAllocator<64>> CumulativeDist;
		float TotalLen = 0.0f;
		if (!BuildCumulativeDistances(WarpedPoints, CumulativeDist, TotalLen))
		{
			return;
		}

		if (ManeuverZones.Num() > 0 &&
			ManeuverSourceTotalLen > KINDA_SMALL_NUMBER &&
			TotalLen > KINDA_SMALL_NUMBER &&
			!FMath::IsNearlyEqual(ManeuverSourceTotalLen, TotalLen, KINDA_SMALL_NUMBER))
		{
			const float DistanceScale = TotalLen / ManeuverSourceTotalLen;
			for (FAPSManeuverZone& Zone : ManeuverZones)
			{
				Zone.StartDistance = FMath::Clamp(Zone.StartDistance * DistanceScale, 0.0f, TotalLen);
				Zone.PeakDistance = FMath::Clamp(Zone.PeakDistance * DistanceScale, 0.0f, TotalLen);
				Zone.EndDistance = FMath::Clamp(Zone.EndDistance * DistanceScale, 0.0f, TotalLen);
				if (Zone.StartDistance > Zone.EndDistance)
				{
					// Defensive reorder: after independent clamp/scale at hard [0..TotalLen] bounds, precision can invert near-zero spans.
					if (bDbg)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: zone distance reorder triggered after remap (preSwapStart=%.1f preSwapEnd=%.1f scale=%.3f)"),
							Zone.StartDistance,
							Zone.EndDistance,
							DistanceScale);
					}
					Swap(Zone.StartDistance, Zone.EndDistance);
				}
				Zone.PeakDistance = FMath::Clamp(Zone.PeakDistance, Zone.StartDistance, Zone.EndDistance);
			}
		}

		const float GlobalSideSign = (ArcStrength >= 0.0f) ? 1.0f : -1.0f;

		const float StrengthClamped = FMath::Clamp(StrengthAbs, 0.0f, 3.0f);
		const float StrengthNorm    = StrengthClamped / 3.0f;
		const float BaseAmp         = 0.25f * Dist2D * StrengthNorm;
		const bool  bUseLaneOffset  = !FMath::IsNearlyZero(LaneOffset, KINDA_SMALL_NUMBER);

		if (BaseAmp <= KINDA_SMALL_NUMBER)
		{
			return;
		}

		// Smallest amplitude that the relative-scale loop will try (BaseAmp * 12.5%).
		// Absolute fallback candidates are only attempted for values below this threshold.
		const float MinRelativeAmp = BaseAmp * GOffsetScales[UE_ARRAY_COUNT(GOffsetScales) - 1];

		if (bDbg)
		{
			UE_LOG(LogAPSNav, Log,
				TEXT("LowLevelArc: WarpPathToArc NumPts=%d TotalLen=%.1f Dist2D=%.1f ArcStrength=%.3f BaseAmp=%.1f MinRelAmp=%.1f"),
				NumPts, TotalLen, Dist2D, ArcStrength, BaseAmp, MinRelativeAmp);
		}

		// Sparse skip is only applied for low-strength arcs on very dense paths;
		// high-strength or long-distance arcs warp all points to preserve visible curvature.
		const bool bSparseWarp = (NumPts > 64) && (StrengthAbs < 1.5f);

		for (int32 i = 1; i < NumPts - 1; ++i)
		{
			if (bSparseWarp && (i & 1) == 0)
			{
				continue;
			}

			const float t = CumulativeDist[i] / TotalLen;  // 0..1 along path
			if (t <= 0.0f || t >= 1.0f)
			{
				continue;
			}

			const float Shape = ComputeArcShape(t, NumMidPoints, MidpointMode);
			if (Shape <= 0.0f)
			{
				continue;
			}

			const FVector& PrevBaseLoc = BasePoints[i - 1].Location;
			const FVector  BaseLoc     = BasePoints[i].Location;
			const FVector& NextBaseLoc = BasePoints[i + 1].Location;

			const FVector Dir2D = (NextBaseLoc - PrevBaseLoc).GetSafeNormal2D();
			if (Dir2D.IsNearlyZero())
			{
				continue;
			}

			// Right vector in XY plane.
			const FVector Right(-Dir2D.Y, Dir2D.X, 0.0f);

			const float LocalSideSign = ComputeLocalSideSign(
				GlobalSideSign,
				t,
				NumMidPoints,
				MidpointMode);

			const float   OffsetSize = LocalSideSign * BaseAmp * Shape;
			const FVector ArcOffset  = Right * OffsetSize;
			// Pre-compute the arc direction factor (right vector scaled by sign and shape envelope).
			// Used by the absolute fallback candidate loop to reconstruct arc offset at a fixed width.
			const FVector ArcDir = Right * (LocalSideSign * Shape);

			FVector CornerLaneOffset = FVector::ZeroVector;
			float CornerLaneStrength = 0.0f;
			if (bUseLaneOffset)
			{
				const FVector DirIn = (BaseLoc - PrevBaseLoc).GetSafeNormal2D();
				const FVector DirOut = (NextBaseLoc - BaseLoc).GetSafeNormal2D();
				if (!DirIn.IsNearlyZero() && !DirOut.IsNearlyZero())
				{
					const float DotInOut = FVector::DotProduct(DirIn, DirOut);
					// Dot is +1 for same-direction parallel segments (our straight case) and decreases as the turn gets sharper.
					const float CornerStrength = FMath::Clamp(1.0f - DotInOut, 0.0f, 1.0f);
					CornerLaneStrength = CornerStrength;
					const float CrossZ = (DirIn.X * DirOut.Y) - (DirIn.Y * DirOut.X);

					if (CornerStrength > KINDA_SMALL_NUMBER && !FMath::IsNearlyZero(CrossZ, KINDA_SMALL_NUMBER))
					{
						const FVector CornerRight = ComputeCornerRightVector(DirIn, DirOut, Dir2D);

						// In XY-plane 2D cross convention, positive CrossZ means CCW (left) turn.
						// CrossZ sign then selects outer side: left turn uses CornerRight, right turn uses opposite.
						const FVector CornerOffsetDir = (CrossZ > 0.0f) ? CornerRight : -CornerRight;
						CornerLaneOffset = CornerOffsetDir * (LaneOffset * CornerStrength);
					}
				}
			}

			FVector ZoneLaneOffset = FVector::ZeroVector;
			float ZoneAlpha = 0.0f;
			float ZoneRequestedWidth = 0.0f;
			const FAPSManeuverZone* DominantZone = nullptr;
			if (bUseLaneOffset && ManeuverZones.Num() > 0)
			{
				const float DistanceAlongPath = CumulativeDist[i];
				float BestZoneWeight = 0.0f;
				for (const FAPSManeuverZone& Zone : ManeuverZones)
				{
					const float Alpha = ComputeManeuverZoneEnvelope(DistanceAlongPath, Zone);
					if (Alpha <= KINDA_SMALL_NUMBER)
					{
						continue;
					}

					const float ZoneWeight = Alpha * FMath::Max(Zone.Strength, GManeuverZoneMinWeightStrength);
					if (ZoneWeight > BestZoneWeight)
					{
						BestZoneWeight = ZoneWeight;
						ZoneAlpha = Alpha;
						ZoneRequestedWidth = Zone.RequestedWidth;
						DominantZone = &Zone;
					}
				}

				if (DominantZone)
				{
					const float LaneSign = (LaneOffset >= 0.0f) ? 1.0f : -1.0f;
					ZoneLaneOffset = DominantZone->SideDir * (LaneSign * DominantZone->RequestedWidth * ZoneAlpha);
				}
			}

			// When a maneuver zone is active, fade local corner offset to avoid double-counting lane displacement.
			CornerLaneOffset *= (1.0f - ZoneAlpha);

			const FVector Offset = ArcOffset + CornerLaneOffset + ZoneLaneOffset;

			float       BaseAreaCost      = 0.0f;
			dtPolyRef   BasePolyRef       = static_cast<dtPolyRef>(BasePoints[i].NodeRef);
			bool        bHasBaseAreaCost  = TryGetAreaCostFromPolyRef(Query, Filter, BasePolyRef, BaseAreaCost);

			if (bRespectNavAreaCosts && !bHasBaseAreaCost)
			{
				float BaseNearest[3];
				dtPolyRef ResolvedBaseRef = 0;
				if (FindNearestPoly(Query, Filter, Extent, BaseLoc, ResolvedBaseRef, BaseNearest))
				{
					BasePolyRef = ResolvedBaseRef;
					bHasBaseAreaCost = TryGetAreaCostFromPolyRef(Query, Filter, BasePolyRef, BaseAreaCost);
				}
			}

			// Try full offset first, then progressively shrink it.
			// Scales are always attempted to provide geometry-aware shrinking even when area-cost checks are disabled.
			const int32 NumOffsetScales = UE_ARRAY_COUNT(GOffsetScales);

			bool bAcceptedCandidate = false;
			bool bRejectedByGeometry = false;
			bool bRejectedByCost     = false;
			float AcceptedScale = 0.0f;
			float AcceptedActualLateral = 0.0f;

			for (int32 ScaleIdx = 0; ScaleIdx < NumOffsetScales; ++ScaleIdx)
			{
				const float   OffsetScale = GOffsetScales[ScaleIdx];
				const FVector WarpedLoc   = BaseLoc + (Offset * OffsetScale);

				float CandidateNearest[3];
				dtPolyRef CandidatePolyRef = 0;
				if (!FindNearestPoly(Query, Filter, Extent, WarpedLoc, CandidatePolyRef, CandidateNearest))
				{
					continue;
				}

				const FVector CandidateLoc = Recast2UnrealPoint(CandidateNearest);

				const FVector& PrevAcceptedLoc = WarpedPoints[i - 1].Location;
				if (!ValidateNavMeshSegment(PrevAcceptedLoc, CandidateLoc, Query, Filter, Extent))
				{
					bRejectedByGeometry = true;
					if (bDbg)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: reject warped point %d (scale=%.3f, geometry failed: prev->candidate)"),
							i,
							OffsetScale);
					}
					continue;
				}

				if (!ValidateNavMeshSegment(CandidateLoc, NextBaseLoc, Query, Filter, Extent))
				{
					bRejectedByGeometry = true;
					if (bDbg)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: reject warped point %d (scale=%.3f, geometry failed: candidate->next)"),
							i,
							OffsetScale);
					}
					continue;
				}

				if (bRespectNavAreaCosts)
				{
					float CandidateAreaCost = 0.0f;
					const bool bHasCandidateAreaCost = TryGetAreaCostFromPolyRef(Query, Filter, CandidatePolyRef, CandidateAreaCost);

					if (!bHasBaseAreaCost || !bHasCandidateAreaCost)
					{
						if (bDbg)
						{
							UE_LOG(LogAPSNav, Log,
								TEXT("LowLevelArc: keep base point %d (cost unavailable, baseRef=%llu candidateRef=%llu)"),
								i,
								static_cast<unsigned long long>(BasePolyRef),
								static_cast<unsigned long long>(CandidatePolyRef));
						}
						continue;
					}

					if (CandidateAreaCost > BaseAreaCost + GNavAreaCostTolerance)
					{
						bRejectedByCost = true;
						if (bDbg)
						{
							UE_LOG(LogAPSNav, Log,
								TEXT("LowLevelArc: reject warped point %d (scale=%.2f, candidateCost=%.3f > baseCost=%.3f)"),
								i,
								OffsetScale,
								CandidateAreaCost,
								BaseAreaCost);
						}
						continue;
					}
				}

				if (bDbg && OffsetScale < FullOffsetScaleThreshold)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: accept reduced warped point %d with scale=%.3f (geometryRejectedEarlier=%s, costRejectedEarlier=%s)"),
						i,
						OffsetScale,
						bRejectedByGeometry ? TEXT("true") : TEXT("false"),
						bRejectedByCost ? TEXT("true") : TEXT("false"));
				}

				WarpedPoints[i].Location = CandidateLoc;
				WarpedPoints[i].NodeRef  = CandidatePolyRef;
				bAcceptedCandidate       = true;
				AcceptedScale            = OffsetScale;
				AcceptedActualLateral    = DominantZone
					? FVector::DotProduct(CandidateLoc - BaseLoc, DominantZone->SideDir)
					: FVector::DotProduct(CandidateLoc - BaseLoc, Right);
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: accept warped point %d scale=%.3f baseAmp=%.1f zoneType=%s zoneAlpha=%.3f zoneReq=%.1f corner=%.3f actualLat=%.1f"),
						i,
						AcceptedScale,
						BaseAmp,
						DominantZone ? ManeuverZoneTypeToString(DominantZone->Type) : TEXT("None"),
						ZoneAlpha,
						ZoneRequestedWidth,
						CornerLaneStrength,
						AcceptedActualLateral);
				}
				break;
			}

			// If all relative-scale candidates failed, try absolute-width fallback arcs.
			// This prevents long-distance arcs from collapsing to the base path when BaseAmp
			// is too large for any relative scale to pass geometry validation.
			if (!bAcceptedCandidate)
			{
				// GArcAbsFallbackAmps is in descending order; advance past entries already covered
				// by the relative scale candidates (those >= MinRelativeAmp).
				const int32 NumAbsFallbacks = UE_ARRAY_COUNT(GArcAbsFallbackAmps);
				int32 AbsFallbackStart = 0;
				while (AbsFallbackStart < NumAbsFallbacks && GArcAbsFallbackAmps[AbsFallbackStart] >= MinRelativeAmp)
				{
					++AbsFallbackStart;
				}
				for (int32 AbsIdx = AbsFallbackStart; AbsIdx < NumAbsFallbacks && !bAcceptedCandidate; ++AbsIdx)
				{
					const float AbsAmp = GArcAbsFallbackAmps[AbsIdx];

					// Build the candidate with the arc at this absolute width; LaneOffset parts are kept full.
					const FVector AbsWarpedLoc = BaseLoc + ArcDir * AbsAmp + CornerLaneOffset + ZoneLaneOffset;

					float CandidateNearest[3];
					dtPolyRef CandidatePolyRef = 0;
					if (!FindNearestPoly(Query, Filter, Extent, AbsWarpedLoc, CandidatePolyRef, CandidateNearest))
					{
						continue;
					}

					const FVector CandidateLoc = Recast2UnrealPoint(CandidateNearest);

					const FVector& PrevAcceptedLoc = WarpedPoints[i - 1].Location;
					if (!ValidateNavMeshSegment(PrevAcceptedLoc, CandidateLoc, Query, Filter, Extent))
					{
						bRejectedByGeometry = true;
						if (bDbg)
						{
							UE_LOG(LogAPSNav, Log,
								TEXT("LowLevelArc: reject warped point %d (absFallback=%.0f, geometry failed: prev->candidate)"),
								i, AbsAmp);
						}
						continue;
					}

					if (!ValidateNavMeshSegment(CandidateLoc, NextBaseLoc, Query, Filter, Extent))
					{
						bRejectedByGeometry = true;
						if (bDbg)
						{
							UE_LOG(LogAPSNav, Log,
								TEXT("LowLevelArc: reject warped point %d (absFallback=%.0f, geometry failed: candidate->next)"),
								i, AbsAmp);
						}
						continue;
					}

					if (bRespectNavAreaCosts)
					{
						float CandidateAreaCost = 0.0f;
						const bool bHasCandidateAreaCost = TryGetAreaCostFromPolyRef(Query, Filter, CandidatePolyRef, CandidateAreaCost);

						if (!bHasBaseAreaCost || !bHasCandidateAreaCost)
						{
							continue;
						}

						if (CandidateAreaCost > BaseAreaCost + GNavAreaCostTolerance)
						{
							bRejectedByCost = true;
							if (bDbg)
							{
								UE_LOG(LogAPSNav, Log,
									TEXT("LowLevelArc: reject warped point %d (absFallback=%.0f, candidateCost=%.3f > baseCost=%.3f)"),
									i, AbsAmp, CandidateAreaCost, BaseAreaCost);
							}
							continue;
						}
					}

					WarpedPoints[i].Location = CandidateLoc;
					WarpedPoints[i].NodeRef  = CandidatePolyRef;
					bAcceptedCandidate       = true;
					AcceptedActualLateral    = DominantZone
						? FVector::DotProduct(CandidateLoc - BaseLoc, DominantZone->SideDir)
						: FVector::DotProduct(CandidateLoc - BaseLoc, Right);
					if (bDbg)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: accept warped point %d absFallback=%.0f baseAmp=%.1f zoneType=%s zoneAlpha=%.3f zoneReq=%.1f actualLat=%.1f"),
							i, AbsAmp, BaseAmp,
							DominantZone ? ManeuverZoneTypeToString(DominantZone->Type) : TEXT("None"),
							ZoneAlpha, ZoneRequestedWidth, AcceptedActualLateral);
					}
				}
			}
			if (!bAcceptedCandidate)
			{
				WarpedPoints[i].Location = BaseLoc;
				WarpedPoints[i].NodeRef  = BasePolyRef;
				if (bDbg)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: keep base point %d (no acceptable warped candidate, zoneType=%s zoneAlpha=%.3f zoneReq=%.1f)"),
						i,
						DominantZone ? ManeuverZoneTypeToString(DominantZone->Type) : TEXT("None"),
						ZoneAlpha,
						ZoneRequestedWidth);
				}
			}
		}

		// Last gate: if curved path cuts through geometry, keep the straight one.
		if (ValidatePathOnNavMesh(WarpedPoints, Query, Filter, Extent))
		{
			Points = MoveTemp(WarpedPoints);
		}
		else if (bDbg)
		{
			UE_LOG(LogAPSNav, Log, TEXT("LowLevelArc: final warped path validation failed, fallback to base path"));
		}
	}

	// --------------------------------------------------------------------
	// Interface helpers (custom path + arc params)
	// --------------------------------------------------------------------

	struct FAPSArcSettings
	{
		float               ArcStrength  = 0.0f;
		int32               NumMidPoints = 0;
		EAPSArcMidpointMode Mode         = EAPSArcMidpointMode::SmoothCurve;
		float               LaneOffset   = 0.0f;
		bool                bUseArc      = false;
	};

	static bool TryBuildCustomPathFromInterface(
		AActor*                OwnerActor,
		const FVector&         Start,
		const FVector&         End,
		bool                   bDbg,
		FAPSArcSettings&       InOutSettings,
		TArray<FNavPathPoint>& OutPathPoints)
	{
		if (!OwnerActor || !OwnerActor->GetClass()->ImplementsInterface(UAPS_MoveToInterface::StaticClass()))
		{
			return false;
		}

		TArray<FVector> CustomPath;
		bool bUseCustomPath = false;
		bool bGenerated     = false;

		IAPS_MoveToInterface::Execute_FindCustomPath(
			OwnerActor,
			Start,
			End,
			CustomPath,
			InOutSettings.ArcStrength,
			OwnerActor->GetWorld(),
			bUseCustomPath,
			bGenerated);
		
		if (bDbg)
		{
			UE_LOG(LogAPSNav, Log,
				TEXT("LowLevelArc: FindCustomPath -> Use=%d Gen=%d ArcStrength=%.3f Points=%d"),
				bUseCustomPath ? 1 : 0,
				bGenerated ? 1 : 0,
				InOutSettings.ArcStrength,
				CustomPath.Num());
		}

		if (!bUseCustomPath || !bGenerated || CustomPath.Num() == 0)
		{
			return false;
		}

		OutPathPoints.Reset(CustomPath.Num());
		for (const FVector& P : CustomPath)
		{
			OutPathPoints.Add(FNavPathPoint(P));
		}

		return true;
	}

	static void PullArcSettingsFromInterface(
		AActor*          OwnerActor,
		bool             bDbg,
		FAPSArcSettings& OutSettings)
	{
		if (!OwnerActor || !OwnerActor->GetClass()->ImplementsInterface(UAPS_MoveToInterface::StaticClass()))
		{
			OutSettings = FAPSArcSettings();
			return;
		}

		IAPS_MoveToInterface::Execute_GetMoveToParameters(
			OwnerActor,
			OutSettings.ArcStrength,
			OutSettings.NumMidPoints,
			OutSettings.Mode,
			OutSettings.LaneOffset);

		if (bDbg)
		{
			UE_LOG(LogAPSNav, Log,
				TEXT("Interface params -> Arc=%.3f MidPoints=%d Mode=%s LaneOffset=%.3f"),
				OutSettings.ArcStrength,
				OutSettings.NumMidPoints,
				(OutSettings.Mode == EAPSArcMidpointMode::SmoothCurve ? TEXT("Smooth") : TEXT("Wavy")),
				OutSettings.LaneOffset);
		}

		const float StrengthAbs = FMath::Abs(OutSettings.ArcStrength);
		OutSettings.bUseArc =
			(StrengthAbs > 0.1f) &&
			(OutSettings.NumMidPoints > 0);
	}

} // anonymous namespace


// ============================================================================
//  APSPathModeLowLevelArc::FindPath
// ============================================================================

namespace APSPathModeLowLevelArc
{
	FPathFindingResult FindPath(
		const AAPS_NavigationData* NavData,
		const FNavAgentProperties& AgentProperties,
		const FPathFindingQuery&   Query)
	{
		const bool bDbg = (CVarAPSNavDebug.GetValueOnAnyThread() != 0);

		FPathFindingResult Result(ENavigationQueryResult::Error);

		if (!NavData)
		{
			UE_LOG(LogAPSNav, Error, TEXT("LowLevelArc::FindPath: NavData is null"));
			return Result;
		}

		const dtNavMesh* DetourNavMesh = NavData->GetRecastMesh();
		if (!DetourNavMesh)
		{
			UE_LOG(LogAPSNav, Error, TEXT("LowLevelArc::FindPath: Detour navmesh is null"));
			return Result;
		}

		const FNavigationQueryFilter* NavFilter = Query.QueryFilter.Get();
		if (!NavFilter)
		{
			UE_LOG(LogAPSNav, Error, TEXT("LowLevelArc::FindPath: NavFilter is null"));
			return Result;
		}

		// Path instance: reuse when possible.
		FNavigationPath* ExistingPath = Query.PathInstanceToFill.Get();
		FNavMeshPath*    NavMeshPath  = ExistingPath ? ExistingPath->CastPath<FNavMeshPath>() : nullptr;

		if (NavMeshPath)
		{
			Result.Path = Query.PathInstanceToFill;
			NavMeshPath->ResetForRepath();
		}
		else
		{
			Result.Path  = NavData->CreatePathInstance<FNavMeshPath>(Query);
			ExistingPath = Result.Path.Get();
			NavMeshPath  = ExistingPath ? ExistingPath->CastPath<FNavMeshPath>() : nullptr;
		}

		if (!NavMeshPath)
		{
			UE_LOG(LogAPSNav, Error, TEXT("LowLevelArc::FindPath: NavMeshPath is null"));
			return Result;
		}

		NavMeshPath->ApplyFlags(Query.NavDataFlags);

		// Degenerate case: start and end are basically the same.
		const FVector AdjustedEnd = NavFilter->GetAdjustedEndLocation(Query.EndLocation);
		if (Query.StartLocation.Equals(AdjustedEnd, KINDA_SMALL_NUMBER))
		{
			TArray<FNavPathPoint>& Points = NavMeshPath->GetPathPoints();
			Points.Reset();
			Points.Add(FNavPathPoint(AdjustedEnd));

			if (ExistingPath)
			{
				ExistingPath->MarkReady();
			}

			Result.Result = ENavigationQueryResult::Success;
			return Result;
		}

		const FVector Start  = Query.StartLocation;
		const FVector End    = AdjustedEnd;
		const float   Dist2D = FVector::Dist2D(Start, End);

		// --------------------------------------------------------------------
		// Talk to APS_MoveToInterface (custom path + arc params)
		// --------------------------------------------------------------------

		AActor*           QueryActor  = GetQueryActor(Query);
		FAPSArcSettings   ArcSettings;
		TArray<FNavPathPoint>& PathPoints = NavMeshPath->GetPathPoints();
		TArray<FAPSSideRouteDebugInfo> SideRouteDebugInfos;
		FAPSSideRouteStats SideRouteStats;
		int32 BuiltManeuverZonesCount = 0;
		int32 DirectCornerCandidatesCount = 0;
		int32 SyntheticFallbackCandidatesCount = 0;

		// 1) Let gameplay completely override the path if it wants.
		if (TryBuildCustomPathFromInterface(
			QueryActor,
			Start,
			End,
			bDbg,
			ArcSettings,
			PathPoints))
		{
			if (ExistingPath)
			{
				ExistingPath->MarkReady();
			}

			Result.Result = ENavigationQueryResult::Success;
			return Result;
		}

		// 2) Otherwise just pull arc settings (strength / number of mids / mode).
		PullArcSettingsFromInterface(QueryActor, bDbg, ArcSettings);

		const bool bUseArc =
			ArcSettings.bUseArc &&
			(Dist2D >= GetMinArcDistance());

		// --------------------------------------------------------------------
		// Plain Detour path (our custom A* under the hood)
		// --------------------------------------------------------------------

		const dtQueryFilter* DtFilter = FAPSPathModeLowLevelBase::GetDetourFilter(*NavFilter);
		if (!DtFilter)
		{
			UE_LOG(LogAPSNav, Error, TEXT("LowLevelArc::FindPath: dtQueryFilter is null"));
			return Result;
		}

		FAPSNavMeshQuery APSQuery;
		const int32      MaxNodesHint = EstimateNodesForDistance(Dist2D);

		if (!APSQuery.Init(DetourNavMesh, DtFilter, MaxNodesHint))
		{
			UE_LOG(LogAPSNav, Error, TEXT("LowLevelArc::FindPath: FAPSNavMeshQuery::Init failed"));
			return Result;
		}

		const FVector DefaultExtent = NavData->GetModifiedQueryExtent(NavData->GetDefaultQueryExtent());
		float         DtExtent[3];
		UnrealExtentToDetour(DefaultExtent, DtExtent);

		bool bPartial = false;
		TArray<dtPolyRef> BaseCorridor;
		const bool bUseSideRoute = FMath::Abs(ArcSettings.LaneOffset) >= GManeuverSideRouteMinLaneOffset;
		const bool bModeRequestsShortcut = !bUseArc && !bUseSideRoute;
		const bool bAllowRaycastShortcut = FAPSPathModeLowLevelBase::ShouldUseLineOfSightShortcut(
			bModeRequestsShortcut,
			NavData->bRespectNavAreaCosts);

		if (bDbg)
		{
			UE_LOG(LogAPSNav, Log,
				TEXT("LowLevelArc: shortcut policy (RespectNavAreaCosts=%s, AllowRaycastShortcut=%s)"),
				NavData->bRespectNavAreaCosts ? TEXT("true") : TEXT("false"),
				bAllowRaycastShortcut ? TEXT("true") : TEXT("false"));
		}

		if (!BuildBaseDetourPath(
			APSQuery,
			DtFilter,
			DtExtent,
			Start,
			End,
			Query.CostLimit,
			bAllowRaycastShortcut,
			BaseCorridor,
			PathPoints,
			bPartial))
		{
			Result.Result = ENavigationQueryResult::Fail;
			return Result;
		}

		// --------------------------------------------------------------------
		// Arc post-process on top of a valid Detour path
		// --------------------------------------------------------------------

		if (bUseSideRoute)
		{
			if (bDbg)
			{
				UE_LOG(LogAPSNav, Log,
					TEXT("LowLevelArc: side-route precheck lane=%.1f useSideRoute=%s basePts=%d baseCorridor=%d useArc=%s allowShortcut=%s"),
					ArcSettings.LaneOffset,
					bUseSideRoute ? TEXT("true") : TEXT("false"),
					PathPoints.Num(),
					BaseCorridor.Num(),
					bUseArc ? TEXT("true") : TEXT("false"),
					bAllowRaycastShortcut ? TEXT("true") : TEXT("false"));
			}

			// Pre-warp side-route injection: runs for large LaneOffset even when arc warp is off.
			TArray<FAPSManeuverZone> InjectionZones;
			float InjectionSourceTotalLen = 0.0f;
			BuildManeuverZones(
				PathPoints, ArcSettings.NumMidPoints, ArcSettings.LaneOffset,
				APSQuery.InternalQuery, DtFilter, DtExtent, bDbg,
				InjectionZones, InjectionSourceTotalLen);
			BuiltManeuverZonesCount = InjectionZones.Num();

			DirectCornerCandidatesCount = AppendDirectCornerSideRouteZones(
				PathPoints,
				ArcSettings.LaneOffset,
				APSQuery.InternalQuery,
				DtFilter,
				DtExtent,
				bDbg,
				InjectionZones,
				InjectionZones);

			if (InjectionZones.Num() == 0)
			{
				if (bDbg)
				{
					if (PathPoints.Num() < 3)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: side-route no candidates: base path has fewer than 3 points"));
					}
					else
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: side-route no candidates: no direct corners above threshold and no maneuver zones"));
					}
				}

				const bool bCanValidateDirectSpan =
					PathPoints.Num() >= 2 &&
					APSQuery.InternalQuery &&
					DtFilter &&
					DtExtent;
				const bool bDirectSpanBlocked =
					bCanValidateDirectSpan &&
					!ValidateNavMeshSegment(
						PathPoints[0].Location,
						PathPoints.Last().Location,
						APSQuery.InternalQuery,
						DtFilter,
						DtExtent);
				const bool bAllowSyntheticFallback = !bCanValidateDirectSpan || bDirectSpanBlocked;

				if (!bAllowSyntheticFallback)
				{
					if (bDbg)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: side-route synthetic fallback skipped: direct span clear/straight"));
					}
				}
				else
				{
					if (bDbg && !bCanValidateDirectSpan)
					{
						UE_LOG(LogAPSNav, Log,
							TEXT("LowLevelArc: side-route synthetic fallback validation unavailable, allowing conservative fallback"));
					}

					SyntheticFallbackCandidatesCount = AppendSyntheticFallbackSideRouteZone(
						PathPoints,
						ArcSettings.LaneOffset,
						InjectionZones);
				}

				if (bDbg && SyntheticFallbackCandidatesCount > 0)
				{
					UE_LOG(LogAPSNav, Log,
						TEXT("LowLevelArc: side-route synthetic fallback candidate added lane=%.1f totalCandidates=%d"),
						ArcSettings.LaneOffset,
						InjectionZones.Num());
				}
			}

			TryInjectManeuverSideRoutes(
				PathPoints,
				InjectionZones,
				InjectionSourceTotalLen,
				ArcSettings.LaneOffset,
				APSQuery,
				DtFilter,
				DtExtent,
				Query.CostLimit,
				NavData->bRespectNavAreaCosts,
				bDbg,
				&SideRouteDebugInfos,
				&SideRouteStats);
		}

		if (bUseArc)
		{

			WarpPathToArc(
				PathPoints,
				ArcSettings.ArcStrength,
				ArcSettings.NumMidPoints,
				ArcSettings.Mode,
				ArcSettings.LaneOffset,
				APSQuery.InternalQuery,
				DtFilter,
				DtExtent,
				NavData->bRespectNavAreaCosts);
		}

		if (bDbg)
		{
			UE_LOG(LogAPSNav, Log,
				TEXT("LowLevelArc: side-route summary zonesBuilt=%d directCorners=%d candidates=%d attempted=%d injected=%d failed=%d"),
				BuiltManeuverZonesCount,
				DirectCornerCandidatesCount,
				SideRouteStats.CandidateCount,
				SideRouteStats.AttemptedCount,
				SideRouteStats.InjectedCount,
				SideRouteStats.FailedCount);
		}

		const TCHAR* CrowdCorridorMode = TEXT("disabled");
		if (NavData->bBuildCrowdCompatiblePathCorridor)
		{
			if (NavData->bRebuildCrowdCorridorAfterArc)
			{
				const bool bRebuildOk = FAPSPathModeLowLevelBase::RebuildNavMeshPathCorridorFromPoints(
					NavMeshPath,
					APSQuery,
					DtFilter,
					DtExtent,
					PathPoints,
					Query.CostLimit);
				if (!bRebuildOk)
				{
					UE_LOG(LogAPSNav, Warning,
						TEXT("LowLevelArc: crowd corridor rebuild from final points produced empty corridor"));
				}
				CrowdCorridorMode = bRebuildOk ? TEXT("rebuilt-from-final-points") : TEXT("rebuild-failed-see-logs");
			}
			else
			{
				FAPSPathModeLowLevelBase::FillNavMeshPathCorridorFromRefs(NavMeshPath, BaseCorridor);
				CrowdCorridorMode = TEXT("base-corridor-from-detour");
			}
		}
		else
		{
			NavMeshPath->SetWantsPathCorridor(false);
			NavMeshPath->PathCorridor.Reset();
			NavMeshPath->PathCorridorCost.Reset();
			NavMeshPath->OnPathCorridorUpdated();
		}

		if (ExistingPath)
		{
			ExistingPath->MarkReady();
		}

		if (bPartial && !Query.bAllowPartialPaths)
		{
			Result.Result = ENavigationQueryResult::Fail;
		}
		else
		{
			Result.Result = ENavigationQueryResult::Success;
		}

		DebugDrawPathIfNeeded(NavData, Query, PathPoints);
		DebugDrawSideRouteInjectionsIfNeeded(NavData, Query, PathPoints, SideRouteDebugInfos);
		
		if (bDbg)
		{
			UE_LOG(LogAPSNav, Log,
				TEXT("LowLevelArc: path built (points=%d, partial=%s, ArcStrength=%.3f, NumMidPoints=%d, Mode=%s, Dist2D=%.1f, UseArc=%s)"),
				PathPoints.Num(),
				bPartial ? TEXT("true") : TEXT("false"),
				ArcSettings.ArcStrength,
				ArcSettings.NumMidPoints,
				(ArcSettings.Mode == EAPSArcMidpointMode::SmoothCurve ? TEXT("Smooth") : TEXT("Wavy")),
				Dist2D,
				bUseArc ? TEXT("true") : TEXT("false"));

			UE_LOG(LogAPSNav, Log,
				TEXT("LowLevelArc: crowd corridor policy (BuildCrowdCompatiblePathCorridor=%s, RebuildAfterArc=%s, Mode=%s)"),
				NavData->bBuildCrowdCompatiblePathCorridor ? TEXT("true") : TEXT("false"),
				NavData->bRebuildCrowdCorridorAfterArc ? TEXT("true") : TEXT("false"),
				CrowdCorridorMode);
		}

		return Result;
	}
} // namespace APSPathModeLowLevelArc

#else // !WITH_RECAST

namespace APSPathModeLowLevelArc
{
	FPathFindingResult FindPath(
		const AAPS_NavigationData* NavData,
		const FNavAgentProperties& AgentProperties,
		const FPathFindingQuery&   Query)
	{
		UE_LOG(LogAPSNav, Warning, TEXT("APS_PathMode_LowLevelArc::FindPath: WITH_RECAST is disabled"));
		return FPathFindingResult(ENavigationQueryResult::Error);
	}
}

#endif // WITH_RECAST
