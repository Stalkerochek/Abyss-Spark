/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Path/APS_PathMode_LowLevelBase.h"

#include "NavMesh/RecastNavMesh.h"
#include "NavMesh/RecastQueryFilter.h"
#include "NavMesh/RecastHelpers.h"
#include "NavMesh/NavMeshPath.h"
#include "Detour/APS_DetourNode.h"
#include "Detour/DetourNavMesh.h"
#include "Detour/DetourNavMeshQuery.h"

// Engine version macros (ENGINE_MAJOR_VERSION / ENGINE_MINOR_VERSION)
#include "Runtime/Launch/Resources/Version.h"
#include <cfloat>

using namespace APSDetour;

// ---------------------------------------------------------------------
//  FAPSPathModeLowLevelBase utilities
// ---------------------------------------------------------------------

const dtQueryFilter* FAPSPathModeLowLevelBase::GetDetourFilter(const FNavigationQueryFilter& NavFilter)
{
	const INavigationQueryFilterInterface* Impl = NavFilter.GetImplementation();
	const FRecastQueryFilter* RecastFilter = static_cast<const FRecastQueryFilter*>(Impl);
	return RecastFilter ? RecastFilter->GetAsDetourQueryFilter() : nullptr;
}

ENavigationQueryResult::Type FAPSPathModeLowLevelBase::DTStatusToNavResult(dtStatus Status)
{
	if (dtStatusSucceed(Status))
	{
		return dtStatusDetail(Status, DT_PARTIAL_RESULT)
			? ENavigationQueryResult::Fail
			: ENavigationQueryResult::Success;
	}

	return dtStatusDetail(Status, DT_INVALID_PARAM)
		? ENavigationQueryResult::Error
		: ENavigationQueryResult::Fail;
}

bool FAPSPathModeLowLevelBase::ShouldUseLineOfSightShortcut(bool bRequestedByMode, bool bRespectNavAreaCosts)
{
	return bRequestedByMode && !bRespectNavAreaCosts;
}

void FAPSPathModeLowLevelBase::FillNavMeshPathCorridorFromRefs(
	FNavMeshPath* NavMeshPath,
	const TArray<dtPolyRef>& Corridor)
{
	if (!NavMeshPath)
	{
		return;
	}

	NavMeshPath->SetWantsPathCorridor(true);
	NavMeshPath->PathCorridor.Reset();
	NavMeshPath->PathCorridorCost.Reset();

	for (dtPolyRef PolyRef : Corridor)
	{
		if (PolyRef != 0)
		{
			NavMeshPath->PathCorridor.Add(static_cast<NavNodeRef>(PolyRef));
			NavMeshPath->PathCorridorCost.Add(0.0f);
		}
	}

	NavMeshPath->OnPathCorridorUpdated();
}

bool FAPSPathModeLowLevelBase::RebuildNavMeshPathCorridorFromPoints(
	FNavMeshPath* NavMeshPath,
	FAPSNavMeshQuery& APSQuery,
	const dtQueryFilter* DtFilter,
	const float Extent[3],
	TArray<FNavPathPoint>& PathPoints,
	float SegmentCostLimit)
{
	if (!NavMeshPath || !APSQuery.InternalQuery || !APSQuery.NavMesh || !DtFilter || !Extent || PathPoints.Num() == 0)
	{
		return false;
	}
	if (Extent[0] <= 0.0f || Extent[1] <= 0.0f || Extent[2] <= 0.0f)
	{
		return false;
	}

	NavMeshPath->SetWantsPathCorridor(true);
	NavMeshPath->PathCorridor.Reset();
	NavMeshPath->PathCorridorCost.Reset();

	auto AddCorridorRef = [&NavMeshPath](dtPolyRef Ref)
	{
		if (Ref == 0)
		{
			return;
		}

		if (NavMeshPath->PathCorridor.Num() > 0 &&
			NavMeshPath->PathCorridor.Last() == static_cast<NavNodeRef>(Ref))
		{
			return;
		}

		NavMeshPath->PathCorridor.Add(static_cast<NavNodeRef>(Ref));
		NavMeshPath->PathCorridorCost.Add(0.0f);
	};

	auto FindNearestPolyForPoint = [&APSQuery, &DtFilter, &Extent](
		const FVector& WorldPoint,
		dtPolyRef& OutRef,
		float OutPoint[3]) -> bool
	{
		OutRef = 0;
		const FVector Rc = Unreal2RecastPoint(WorldPoint);

#if ENGINE_MAJOR_VERSION >= 5
		dtReal DtPos[3] =
		{
			(dtReal)Rc.X,
			(dtReal)Rc.Y,
			(dtReal)Rc.Z
		};

		dtReal DtExtent[3] =
		{
			(dtReal)Extent[0],
			(dtReal)Extent[1],
			(dtReal)Extent[2]
		};

		dtReal DtNearest[3];
		const dtStatus Status = APSQuery.InternalQuery->findNearestPoly(
			DtPos,
			DtExtent,
			DtFilter,
			&OutRef,
			DtNearest);

		if (dtStatusFailed(Status) || OutRef == 0)
		{
			return false;
		}

		OutPoint[0] = (float)DtNearest[0];
		OutPoint[1] = (float)DtNearest[1];
		OutPoint[2] = (float)DtNearest[2];
#else
		const dtStatus Status = APSQuery.InternalQuery->findNearestPoly(
			&Rc.X,
			Extent,
			DtFilter,
			&OutRef,
			OutPoint);
#endif
		return dtStatusSucceed(Status) && OutRef != 0;
	};

	if (PathPoints.Num() == 1)
	{
		dtPolyRef SingleRef = 0;
		float NearestPt[3];
		if (!FindNearestPolyForPoint(PathPoints[0].Location, SingleRef, NearestPt))
		{
			return false;
		}

		PathPoints[0].NodeRef = SingleRef;
		AddCorridorRef(SingleRef);
		NavMeshPath->OnPathCorridorUpdated();
		return NavMeshPath->PathCorridor.Num() > 0;
	}

	const float EffectiveSegmentCostLimit = (SegmentCostLimit > 0.0f) ? SegmentCostLimit : FLT_MAX;

	for (int32 Idx = 0; Idx < PathPoints.Num() - 1; ++Idx)
	{
		dtPolyRef StartRef = 0;
		dtPolyRef EndRef = 0;
		float StartPt[3];
		float EndPt[3];

		if (!FindNearestPolyForPoint(PathPoints[Idx].Location, StartRef, StartPt) ||
			!FindNearestPolyForPoint(PathPoints[Idx + 1].Location, EndRef, EndPt))
		{
			AddCorridorRef(static_cast<dtPolyRef>(PathPoints[Idx].NodeRef));
			AddCorridorRef(static_cast<dtPolyRef>(PathPoints[Idx + 1].NodeRef));
			continue;
		}

		PathPoints[Idx].NodeRef = StartRef;
		PathPoints[Idx + 1].NodeRef = EndRef;

		TArray<dtPolyRef> SegmentCorridor;
		const dtStatus SegmentStatus = APSQuery.FindPath(
			StartRef,
			EndRef,
			StartPt,
			EndPt,
			EffectiveSegmentCostLimit,
			SegmentCorridor,
			nullptr);

		if (dtStatusSucceed(SegmentStatus) && SegmentCorridor.Num() > 0)
		{
			for (dtPolyRef Ref : SegmentCorridor)
			{
				AddCorridorRef(Ref);
			}
		}
		else
		{
			AddCorridorRef(StartRef);
			AddCorridorRef(EndRef);
		}
	}

	NavMeshPath->OnPathCorridorUpdated();
	return NavMeshPath->PathCorridor.Num() > 0;
}

// ---------------------------------------------------------------------
//  FAPSNavMeshQuery
// ---------------------------------------------------------------------

FAPSPathModeLowLevelBase::FAPSNavMeshQuery::FAPSNavMeshQuery()
	: NavMesh(nullptr)
	, InternalQuery(nullptr)
	, Filter(nullptr)
	, MaxNodes(0)
{
}

FAPSPathModeLowLevelBase::FAPSNavMeshQuery::~FAPSNavMeshQuery()
{
	if (InternalQuery)
	{
		delete InternalQuery;
		InternalQuery = nullptr;
	}
}

bool FAPSPathModeLowLevelBase::FAPSNavMeshQuery::Init(
	const dtNavMesh* InNavMesh,
	const dtQueryFilter* InFilter,
	int32 InMaxNodes)
{
	NavMesh = InNavMesh;
	Filter  = InFilter;

	// If MaxNodes <= 0, fall back to a safe default.
	MaxNodes = (InMaxNodes > 0) ? InMaxNodes : 2048;

	if (!NavMesh || !Filter)
	{
		return false;
	}

	if (!InternalQuery)
	{
		InternalQuery = new dtNavMeshQuery();
	}

	const dtStatus InitStatus =
		InternalQuery->init(const_cast<dtNavMesh*>(NavMesh), MaxNodes);

	if (dtStatusFailed(InitStatus))
	{
		delete InternalQuery;
		InternalQuery = nullptr;
		return false;
	}

	return true;
}

/**
 * Custom wrapper around dtNavMeshQuery::findPath.
 * Produces a polygon corridor in OutCorridor.
 *
 * The function is written to be compatible with both UE4 (Detour uses float)
 * and UE5 (Detour uses dtReal for query coordinates).
 */
dtStatus FAPSPathModeLowLevelBase::FAPSNavMeshQuery::FindPath(
	dtPolyRef StartRef,
	dtPolyRef EndRef,
	const float* StartPos,
	const float* EndPos,
	float CostLimit,
	TArray<dtPolyRef>& OutCorridor,
	float* OutTotalCost)
{
	OutCorridor.Reset();

	if (!NavMesh || !Filter || !InternalQuery)
	{
		return DT_FAILURE | DT_INVALID_PARAM;
	}

	if (!StartRef || !EndRef)
	{
		return DT_FAILURE | DT_INVALID_PARAM;
	}

	// Validate poly refs.
	if (!NavMesh->isValidPolyRef(StartRef) ||
		!NavMesh->isValidPolyRef(EndRef))
	{
		return DT_FAILURE | DT_INVALID_PARAM;
	}

	// Trivial case: same polygon.
	if (StartRef == EndRef)
	{
		OutCorridor.Add(StartRef);
		if (OutTotalCost)
		{
			*OutTotalCost = 0.0f;
		}
		return DT_SUCCESS;
	}

	dtQueryResult Result;

#if ENGINE_MAJOR_VERSION >= 5
	// UE5 Detour query API uses dtReal for positions and cost.
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

	const dtReal CostLimitReal = (dtReal)CostLimit;
	dtReal TotalCostReal = 0;
	dtReal* TotalCostPtr = OutTotalCost ? &TotalCostReal : nullptr;

	const dtStatus Status = InternalQuery->findPath(
		StartRef,
		EndRef,
		DtStart,
		DtEnd,
		CostLimitReal,
		Filter,
		Result,
		TotalCostPtr);

	if (OutTotalCost)
	{
		*OutTotalCost = (float)TotalCostReal;
	}
#else
	// UE4 Detour uses float directly.
	float LocalTotalCost = 0.0f;
	float* TotalCostPtr = OutTotalCost ? &LocalTotalCost : nullptr;

	const dtStatus Status = InternalQuery->findPath(
		StartRef,
		EndRef,
		StartPos,
		EndPos,
		CostLimit,
		Filter,
		Result,
		TotalCostPtr);

	if (OutTotalCost)
	{
		*OutTotalCost = LocalTotalCost;
	}
#endif

	if (dtStatusFailed(Status))
	{
		return Status;
	}

	const int32 NumPolys = Result.size();
	if (NumPolys <= 0)
	{
		return Status;
	}

	OutCorridor.SetNum(NumPolys);
	Result.copyRefs(OutCorridor.GetData(), NumPolys);

	return Status;
}
