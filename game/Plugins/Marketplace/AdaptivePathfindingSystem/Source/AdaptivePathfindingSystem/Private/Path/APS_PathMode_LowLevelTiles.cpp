/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Path/APS_PathMode_LowLevelTiles.h"

#include "Path/APS_PathMode_LowLevelBase.h"
#include "Navigation/APS_NavigationData.h"
#include "Debug/APS_NavLogs.h"

#include "NavMesh/RecastHelpers.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMesh/RecastQueryFilter.h"

#include "Detour/DetourNavMesh.h"
#include "Detour/DetourNavMeshQuery.h"
#include "Detour/APS_DetourNode.h"

#include "Runtime/Launch/Resources/Version.h"

#if WITH_RECAST

namespace APSPathModeLowLevelTiles
{
	// Simple marker type inheriting the low-level base.
	struct FLowLevelTilesMode : public FAPSPathModeLowLevelBase
	{
		using FAPSNavMeshQuery = FAPSPathModeLowLevelBase::FAPSNavMeshQuery;
	};

	FPathFindingResult FindPath(
		const AAPS_NavigationData* NavData,
		const FNavAgentProperties& AgentProperties,
		const FPathFindingQuery&   Query)
	{
		const bool bDbg = (CVarAPSNavDebug.GetValueOnAnyThread() != 0);
		FPathFindingResult Result(ENavigationQueryResult::Error);

		if (!NavData)
		{
			UE_LOG(LogAPSNav, Error, TEXT("APSPathModeLowLevelTiles::FindPath: NavData is null"));
			return Result;
		}

		const dtNavMesh* DetourNavMesh = NavData->GetRecastMesh();
		if (!DetourNavMesh)
		{
			UE_LOG(LogAPSNav, Error, TEXT("APSPathModeLowLevelTiles::FindPath: Detour navmesh is null"));
			return Result;
		}

		// Reuse existing FNavMeshPath if possible.
		FNavigationPath* ExistingPath = Query.PathInstanceToFill.Get();
		FNavMeshPath*    NavMeshPath  = ExistingPath ? ExistingPath->CastPath<FNavMeshPath>() : nullptr;

		if (NavMeshPath)
		{
			Result.Path = Query.PathInstanceToFill;
			NavMeshPath->ResetForRepath();
		}
		else
		{
			Result.Path   = NavData->CreatePathInstance<FNavMeshPath>(Query);
			ExistingPath  = Result.Path.Get();
			NavMeshPath   = ExistingPath ? ExistingPath->CastPath<FNavMeshPath>() : nullptr;
		}

		const FNavigationQueryFilter* NavFilter = Query.QueryFilter.Get();
		if (!NavMeshPath || !NavFilter)
		{
			UE_LOG(LogAPSNav, Error, TEXT("APSPathModeLowLevelTiles::FindPath: NavMeshPath or NavFilter is null"));
			return Result;
		}

		NavMeshPath->ApplyFlags(Query.NavDataFlags);

		const FVector AdjustedEnd = NavFilter->GetAdjustedEndLocation(Query.EndLocation);

		// Trivial path: start ~= end.
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

		// Build Detour filter from UE navigation filter.
		const dtQueryFilter* DtFilter = FLowLevelTilesMode::GetDetourFilter(*NavFilter);
		if (!DtFilter)
		{
			UE_LOG(LogAPSNav, Error, TEXT("APSPathModeLowLevelTiles::FindPath: dtQueryFilter is null"));
			return Result;
		}

		// Initialize custom Detour query wrapper.
		// MaxNodes = 0 => use Detour default (matches engine behavior).
		const int32 MaxNodes = 0;
		FLowLevelTilesMode::FAPSNavMeshQuery APSQuery;
		if (!APSQuery.Init(DetourNavMesh, DtFilter, MaxNodes))
		{
			UE_LOG(LogAPSNav, Error, TEXT("APSPathModeLowLevelTiles::FindPath: FAPSNavMeshQuery::Init failed"));
			return Result;
		}

		const FVector NavExtent = NavData->GetModifiedQueryExtent(NavData->GetDefaultQueryExtent());

		float Extent[3];
		Extent[0] = static_cast<float>(NavExtent.X);
		Extent[1] = static_cast<float>(NavExtent.Z);
		Extent[2] = static_cast<float>(NavExtent.Y);


		// Convert UE world positions to Recast space.
		const FVector RcStart = Unreal2RecastPoint(Query.StartLocation);
		const FVector RcEnd   = Unreal2RecastPoint(AdjustedEnd);

		dtPolyRef StartRef = 0;
		dtPolyRef EndRef   = 0;
		float     StartPt[3];
		float     EndPt[3];

#if ENGINE_MAJOR_VERSION >= 5
		// UE5 Detour API uses dtReal for coordinates.
		dtReal DtExtent[3] =
		{
			(dtReal)Extent[0],
			(dtReal)Extent[1],
			(dtReal)Extent[2],
		};

		dtReal DtStart[3] =
		{
			(dtReal)RcStart.X,
			(dtReal)RcStart.Y,
			(dtReal)RcStart.Z,
		};

		dtReal DtEnd[3] =
		{
			(dtReal)RcEnd.X,
			(dtReal)RcEnd.Y,
			(dtReal)RcEnd.Z,
		};

		dtReal DtStartPt[3];
		dtReal DtEndPt[3];

		// Find nearest polys for start.
		{
			const dtStatus S = APSQuery.InternalQuery->findNearestPoly(
				DtStart,
				DtExtent,
				DtFilter,
				&StartRef,
				DtStartPt);

			if (dtStatusFailed(S) || !StartRef)
			{
				Result.Result = ENavigationQueryResult::Fail;
				return Result;
			}

			StartPt[0] = (float)DtStartPt[0];
			StartPt[1] = (float)DtStartPt[1];
			StartPt[2] = (float)DtStartPt[2];
		}

		// Find nearest polys for end.
		{
			const dtStatus S = APSQuery.InternalQuery->findNearestPoly(
				DtEnd,
				DtExtent,
				DtFilter,
				&EndRef,
				DtEndPt);

			if (dtStatusFailed(S) || !EndRef)
			{
				Result.Result = ENavigationQueryResult::Fail;
				return Result;
			}

			EndPt[0] = (float)DtEndPt[0];
			EndPt[1] = (float)DtEndPt[1];
			EndPt[2] = (float)DtEndPt[2];
		}
#else
		// UE4 Detour API uses float directly.

		// Find nearest polys for start.
		{
			const dtStatus S = APSQuery.InternalQuery->findNearestPoly(
				&RcStart.X,
				Extent,
				DtFilter,
				&StartRef,
				StartPt);

			if (dtStatusFailed(S) || !StartRef)
			{
				Result.Result = ENavigationQueryResult::Fail;
				return Result;
			}
		}

		// Find nearest polys for end.
		{
			const dtStatus S = APSQuery.InternalQuery->findNearestPoly(
				&RcEnd.X,
				Extent,
				DtFilter,
				&EndRef,
				EndPt);

			if (dtStatusFailed(S) || !EndRef)
			{
				Result.Result = ENavigationQueryResult::Fail;
				return Result;
			}
		}
#endif // ENGINE_MAJOR_VERSION

		// --- Custom APS A* (wrapper over Detour findPath) -> corridor ---

		TArray<dtPolyRef> Corridor;
		float             TotalCost = 0.0f;

		const float CostLimit = Query.CostLimit;

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
			Result.Result = ENavigationQueryResult::Fail;
			return Result;
		}

		const bool bPartialDetour = (PathStatus & DT_PARTIAL_RESULT) != 0;

		// --- Build a straight path along this corridor ---

		dtQueryResult StraightResult;

#if ENGINE_MAJOR_VERSION >= 5
		dtReal DtStartPt2[3] =
		{
			(dtReal)StartPt[0],
			(dtReal)StartPt[1],
			(dtReal)StartPt[2],
		};

		dtReal DtEndPt2[3] =
		{
			(dtReal)EndPt[0],
			(dtReal)EndPt[1],
			(dtReal)EndPt[2],
		};

		const dtStatus StraightStatus = APSQuery.InternalQuery->findStraightPath(
			DtStartPt2,
			DtEndPt2,
			Corridor.GetData(),
			Corridor.Num(),
			StraightResult,
			0); // DT_STRAIGHTPATH_* flags
#else
		const dtStatus StraightStatus = APSQuery.InternalQuery->findStraightPath(
			StartPt,
			EndPt,
			Corridor.GetData(),
			Corridor.Num(),
			StraightResult,
			0); // DT_STRAIGHTPATH_* flags
#endif

		if (dtStatusFailed(StraightStatus) || StraightResult.size() <= 0)
		{
			Result.Result = ENavigationQueryResult::Fail;
			return Result;
		}

		const int32 StraightCount = StraightResult.size();

		TArray<FNavPathPoint>& PathPoints = NavMeshPath->GetPathPoints();
		PathPoints.Reset(StraightCount);
		PathPoints.Reserve(StraightCount);

		for (int32 Idx = 0; Idx < StraightCount; ++Idx)
		{
#if ENGINE_MAJOR_VERSION >= 5
			const dtReal* Pos      = StraightResult.getPos(Idx);
			const FVector WorldPos = Recast2UnrealPoint(Pos);
#else
			const float*  Pos      = StraightResult.getPos(Idx);
			const FVector WorldPos = Recast2UnrealPoint(Pos);
#endif
			FNavPathPoint NewPoint(WorldPos);
			NewPoint.NodeRef = StraightResult.getRef(Idx);

			PathPoints.Add(NewPoint);
		}

		// Make the path look as close as possible to an engine Recast path.
		if (PathPoints.Num() > 0)
		{
			PathPoints[0].Location = Query.StartLocation;
			PathPoints[0].NodeRef = StartRef;

			PathPoints.Last().Location = AdjustedEnd;
			PathPoints.Last().NodeRef = EndRef;
		}

		if (NavData->bBuildCrowdCompatiblePathCorridor)
		{
			FLowLevelTilesMode::FillNavMeshPathCorridorFromRefs(NavMeshPath, Corridor);
		}
		else
		{
			NavMeshPath->SetWantsPathCorridor(false);
			NavMeshPath->PathCorridor.Reset();
			NavMeshPath->PathCorridorCost.Reset();
			NavMeshPath->OnPathCorridorUpdated();
		}

		if (bDbg)
		{
			UE_LOG(LogAPSNav, Log,
				TEXT("LowLevelTiles: crowd corridor policy (BuildCrowdCompatiblePathCorridor=%s, CorridorMode=%s)"),
				NavData->bBuildCrowdCompatiblePathCorridor ? TEXT("true") : TEXT("false"),
				NavData->bBuildCrowdCompatiblePathCorridor ? TEXT("base-corridor-from-detour") : TEXT("disabled"));
		}

		// Handle partial path according to Query.bAllowPartialPaths.
		if (bPartialDetour && !Query.bAllowPartialPaths)
		{
			Result.Result = ENavigationQueryResult::Fail;
		}
		else
		{
			Result.Result = ENavigationQueryResult::Success;
		}

		if (Result.Result == ENavigationQueryResult::Success && ExistingPath)
		{
			ExistingPath->MarkReady();
		}

		return Result;
	}
} // namespace APSPathModeLowLevelTiles

#else // WITH_RECAST

namespace APSPathModeLowLevelTiles
{
	FPathFindingResult FindPath(
		const AAPS_NavigationData* NavData,
		const FNavAgentProperties& AgentProperties,
		const FPathFindingQuery&   Query)
	{
		UE_LOG(LogAPSNav, Warning,
			TEXT("APSPathModeLowLevelTiles::FindPath: WITH_RECAST is disabled"));
		return FPathFindingResult(ENavigationQueryResult::Error);
	}
}

#endif // WITH_RECAST
