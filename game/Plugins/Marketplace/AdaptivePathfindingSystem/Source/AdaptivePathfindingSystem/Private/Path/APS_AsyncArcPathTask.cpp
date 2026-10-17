/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Path/APS_AsyncArcPathTask.h"

#include "Path/APS_PathMode_LowLevelBase.h"
#include "Debug/APS_NavLogs.h"

#include "NavMesh/RecastHelpers.h"

#include "Detour/DetourNavMesh.h"
#include "Detour/DetourNavMeshQuery.h"

#include "Runtime/Launch/Resources/Version.h"

#if WITH_RECAST

// Convenient alias for the low-level query wrapper.
using FAPSNavMeshQuery = FAPSPathModeLowLevelBase::FAPSNavMeshQuery;

namespace
{
	/**
	 * Build a straight path segment A -> B using Detour and APS custom path find.
	 * Outputs an array of world-space points (pure FVector).
	 */
	static bool BuildDetourSegmentPure(
		FAPSNavMeshQuery&                                      APSQuery,
		const dtQueryFilter*                                   DtFilter,
		const float                                            Extent[3],
		const FVector&                                         A,
		const FVector&                                         B,
		float                                                  CostLimit,
		TArray<FVector, TInlineAllocator<32>>&                 OutPoints,
		bool&                                                  bOutPartial)
	{
		OutPoints.Reset();
		bOutPartial = false;

		if (!APSQuery.InternalQuery || !APSQuery.NavMesh || !DtFilter)
		{
			return false;
		}

		// Convert UE points to Detour (Recast) space.
		const FVector RcStart = Unreal2RecastPoint(A);
		const FVector RcEnd   = Unreal2RecastPoint(B);

		dtPolyRef StartRef = 0;
		dtPolyRef EndRef   = 0;

		// APS side uses float buffers to match FAPSNavMeshQuery::FindPath.
		float StartPt[3];
		float EndPt[3];

#if ENGINE_MAJOR_VERSION >= 5
		// UE5 Detour query API uses dtReal for input/output positions.
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

		// Find nearest poly for start.
		{
			const dtStatus S = APSQuery.InternalQuery->findNearestPoly(
				DtStart,
				DtExtent,
				DtFilter,
				&StartRef,
				DtStartPt);

			if (dtStatusFailed(S) || !StartRef)
			{
				return false;
			}

			StartPt[0] = (float)DtStartPt[0];
			StartPt[1] = (float)DtStartPt[1];
			StartPt[2] = (float)DtStartPt[2];
		}

		// Find nearest poly for end.
		{
			const dtStatus S = APSQuery.InternalQuery->findNearestPoly(
				DtEnd,
				DtExtent,
				DtFilter,
				&EndRef,
				DtEndPt);

			if (dtStatusFailed(S) || !EndRef)
			{
				return false;
			}

			EndPt[0] = (float)DtEndPt[0];
			EndPt[1] = (float)DtEndPt[1];
			EndPt[2] = (float)DtEndPt[2];
		}
#else
		// UE4 Detour uses float directly.
		// Find nearest poly for start.
		{
			const dtStatus S = APSQuery.InternalQuery->findNearestPoly(
				&RcStart.X,
				Extent,
				DtFilter,
				&StartRef,
				StartPt);

			if (dtStatusFailed(S) || !StartRef)
			{
				return false;
			}
		}

		// Find nearest poly for end.
		{
			const dtStatus S = APSQuery.InternalQuery->findNearestPoly(
				&RcEnd.X,
				Extent,
				DtFilter,
				&EndRef,
				EndPt);

			if (dtStatusFailed(S) || !EndRef)
			{
				return false;
			}
		}
#endif // ENGINE_MAJOR_VERSION

		// Run custom path find (APS wrapper over dtNavMeshQuery::findPath).
		TArray<dtPolyRef> Corridor;
		float             TotalCost = 0.0f;

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

		bOutPartial = (PathStatus & DT_PARTIAL_RESULT) != 0;

		// Convert corridor to a straight path in world space via Detour straight path query.
		dtQueryResult StraightResult;

#if ENGINE_MAJOR_VERSION >= 5
		// Convert float endpoints back to dtReal for the query.
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
			0 /* options */);
#else
		const dtStatus StraightStatus = APSQuery.InternalQuery->findStraightPath(
			StartPt,
			EndPt,
			Corridor.GetData(),
			Corridor.Num(),
			StraightResult,
			0 /* options */);
#endif

		if (dtStatusFailed(StraightStatus) || StraightResult.size() <= 0)
		{
			return false;
		}

		const int32 StraightCount = StraightResult.size();
		OutPoints.Reset(StraightCount);
		OutPoints.Reserve(StraightCount);

		for (int32 Idx = 0; Idx < StraightCount; ++Idx)
		{
#if ENGINE_MAJOR_VERSION >= 5
			// dtQueryResult stores positions as dtReal in UE5.
			const dtReal* PosReal = StraightResult.getPos(Idx);
			const FVector WorldPoint = Recast2UnrealPoint(PosReal);
#else
			const float* Pos = StraightResult.getPos(Idx);
			const FVector WorldPoint = Recast2UnrealPoint(Pos);
#endif
			OutPoints.Add(WorldPoint);
		}

		return OutPoints.Num() > 0;
	}

	/** Append segment points to the final path, skipping duplicates at joins. */
	static void AppendSegment(
		const TArray<FVector, TInlineAllocator<32>>& Segment,
		TArray<FVector>&                             OutPath)
	{
		if (Segment.Num() == 0)
		{
			return;
		}

		int32 StartIdx = 0;
		if (OutPath.Num() > 0 && OutPath.Last().Equals(Segment[0], 1.0f))
		{
			StartIdx = 1;
		}

		for (int32 Idx = StartIdx; Idx < Segment.Num(); ++Idx)
		{
			OutPath.Add(Segment[Idx]);
		}
	}

} // anonymous namespace


void FAPSArcPathTask::DoWork()
{
	Result.Reset();

	if (!Request.DetourNavMesh || !Request.QueryFilter)
	{
		UE_LOG(LogAPSNav, Warning, TEXT("FAPSArcPathTask::DoWork: missing DetourNavMesh or QueryFilter"));
		return;
	}

	// Initialize low-level Detour query wrapper with our custom path find.
	FAPSNavMeshQuery APSQuery;
	const int32 MaxNodes = 0; // 0 => Detour default.

	if (!APSQuery.Init(Request.DetourNavMesh, Request.QueryFilter, MaxNodes))
	{
		UE_LOG(LogAPSNav, Warning, TEXT("FAPSArcPathTask::DoWork: FAPSNavMeshQuery::Init failed"));
		return;
	}

	float DtExtent[3];
	APS_UnrealExtentToDetour(Request.QueryExtent, DtExtent);

	const FVector Start = Request.Start;
	const FVector End   = Request.End;

	const float CostLimit      = Request.CostLimit;
	const float MinArcDistance = Request.MinArcDistance > 0.0f
		? Request.MinArcDistance
		: APS_DefaultMinArcDistance;
	const float ArcStrength    = Request.ArcStrength;
	const int32 NumMidPoints   = Request.NumMidPoints;

	const float StraightDistance = FVector::Dist(Start, End);
	const bool  bCanUseArc =
		(FMath::Abs(ArcStrength) > KINDA_SMALL_NUMBER) &&
		(NumMidPoints > 0) &&
		(StraightDistance >= MinArcDistance);

	// Straight path case.
	if (!bCanUseArc)
	{
		TArray<FVector, TInlineAllocator<32>> Segment;
		bool bPartial = false;

		if (!BuildDetourSegmentPure(APSQuery, Request.QueryFilter, DtExtent, Start, End, CostLimit, Segment, bPartial))
		{
			return;
		}

		if (!Request.bAllowPartial && bPartial)
		{
			return;
		}

		Result.Points   = Segment;
		Result.bSuccess = true;
		Result.bPartial = bPartial;
		return;
	}

	// Arc mode: build an arc by stitching multiple Detour segments via arc anchors.

	const FVector Fwd2D = (End - Start).GetSafeNormal2D();
	if (Fwd2D.IsNearlyZero())
	{
		// Degenerate case, fall back to straight path.
		TArray<FVector, TInlineAllocator<32>> Segment;
		bool bPartial = false;

		if (!BuildDetourSegmentPure(APSQuery, Request.QueryFilter, DtExtent, Start, End, CostLimit, Segment, bPartial))
		{
			return;
		}

		if (!Request.bAllowPartial && bPartial)
		{
			return;
		}

		Result.Points   = Segment;
		Result.bSuccess = true;
		Result.bPartial = bPartial;
		return;
	}

	// Build list of arc anchors: [Start, mid1, mid2, ..., End].
	TArray<FVector, TInlineAllocator<16>> Anchors;
	Anchors.Add(Start);

	if (NumMidPoints > 0)
	{
		const FVector Dir = (End - Start).GetSafeNormal();
		const float   TotalDist = StraightDistance;
		const float   Step = TotalDist / (NumMidPoints + 1);

		for (int32 Idx = 1; Idx <= NumMidPoints; ++Idx)
		{
			const float T = Step * Idx;
			const FVector BasePoint = Start + Dir * T;

			// Simple perpendicular offset in 2D to form an arc.
			const FVector Perp2D(-Fwd2D.Y, Fwd2D.X, 0.0f);
			const float   OffsetScale = ArcStrength * FMath::Clamp(T / TotalDist, 0.0f, 1.0f);
			const FVector Offset = Perp2D * OffsetScale * MinArcDistance;

			Anchors.Add(BasePoint + Offset);
		}
	}

	Anchors.Add(End);

	// Build Detour segments between anchors and stitch them together.
	TArray<FVector> FullPath;
	FullPath.Reserve(Anchors.Num() * 8);

	bool bAnyPartial = false;

	for (int32 Idx = 0; Idx + 1 < Anchors.Num(); ++Idx)
	{
		const FVector& From = Anchors[Idx];
		const FVector& To   = Anchors[Idx + 1];

		TArray<FVector, TInlineAllocator<32>> SegmentPoints;
		bool bPartial = false;

		if (!BuildDetourSegmentPure(APSQuery, Request.QueryFilter, DtExtent, From, To, CostLimit, SegmentPoints, bPartial))
		{
			Result.Reset();
			return;
		}

		if (!Request.bAllowPartial && bPartial)
		{
			Result.Reset();
			return;
		}

		AppendSegment(SegmentPoints, FullPath);

		if (bPartial)
		{
			bAnyPartial = true;
		}
	}

	Result.Points   = FullPath;
	Result.bSuccess = true;
	Result.bPartial = bAnyPartial;
}

#else  // WITH_RECAST

void FAPSArcPathTask::DoWork()
{
	Result.Reset();
	UE_LOG(LogAPSNav, Warning, TEXT("FAPSArcPathTask::DoWork: WITH_RECAST is disabled"));
}

#endif // WITH_RECAST
