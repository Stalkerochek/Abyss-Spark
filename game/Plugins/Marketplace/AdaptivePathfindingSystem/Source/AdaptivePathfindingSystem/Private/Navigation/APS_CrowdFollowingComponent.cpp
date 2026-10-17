/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
 * This plugin is downloadable from the Unreal Engine Marketplace
 */

#include "Navigation/APS_CrowdFollowingComponent.h"

#include "Debug/APS_NavLogs.h"

#include "Navigation/CrowdManager.h"
#include "NavMesh/NavMeshPath.h"
#include "NavMesh/RecastNavMesh.h"

UAPS_CrowdFollowingComponent::UAPS_CrowdFollowingComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UAPS_CrowdFollowingComponent::IsStrictAPSFollowingActive() const
{
	if (!bFollowAPSPathPointsStrictly || !Path.IsValid())
	{
		return false;
	}

	if (!Path->IsValid())
	{
		return false;
	}

	const int32 RequiredPoints = FMath::Max(2, MinPathPointsForStrictFollowing);
	return Path->GetPathPoints().Num() >= RequiredPoints;
}

void UAPS_CrowdFollowingComponent::ApplyStrictCrowdOptimizationPolicy(bool bStrictActive)
{
	if (!bDisableCrowdShortcutOptimizationsWhenStrict)
	{
		if (bHasCachedCrowdOptimizationState)
		{
			SetCrowdOptimizeVisibility(bCachedOptimizeVisibility);
			SetCrowdOptimizeTopology(bCachedOptimizeTopology);
			bHasCachedCrowdOptimizationState = false;
		}
		return;
	}

	if (bStrictActive)
	{
		if (!bHasCachedCrowdOptimizationState)
		{
			bCachedOptimizeVisibility = IsCrowdOptimizeVisibilityEnabled();
			bCachedOptimizeTopology = IsCrowdOptimizeTopologyEnabled();
			bHasCachedCrowdOptimizationState = true;
		}

		SetCrowdOptimizeVisibility(false);
		SetCrowdOptimizeTopology(false);
	}
	else if (bHasCachedCrowdOptimizationState)
	{
		SetCrowdOptimizeVisibility(bCachedOptimizeVisibility);
		SetCrowdOptimizeTopology(bCachedOptimizeTopology);
		bHasCachedCrowdOptimizationState = false;
	}
}

void UAPS_CrowdFollowingComponent::DebugLogStrictState(const TCHAR* Context, int32 SegmentStartIndex, int32 CorridorSize) const
{
	const bool bDbg = (CVarAPSNavDebug.GetValueOnAnyThread() != 0);
	if (!bLogAPSStrictFollowing && !bDbg)
	{
		return;
	}

	const int32 PathPointsNum = Path.IsValid() ? Path->GetPathPoints().Num() : 0;
	UE_LOG(LogAPSNav, Log,
		TEXT("APS_CrowdFollowing[%s]: Strict=%s Sim=%s PathPoints=%d SegmentStart=%d CorridorSize=%d"),
		Context,
		IsStrictAPSFollowingActive() ? TEXT("true") : TEXT("false"),
		IsCrowdSimulationEnabled() ? TEXT("enabled") : TEXT("disabled"),
		PathPointsNum,
		SegmentStartIndex,
		CorridorSize);
}

void UAPS_CrowdFollowingComponent::OnPathUpdated()
{
	Super::OnPathUpdated();

	const bool bStrictActive = IsStrictAPSFollowingActive();
	ApplyStrictCrowdOptimizationPolicy(bStrictActive);
	DebugLogStrictState(TEXT("OnPathUpdated"), INDEX_NONE, INDEX_NONE);
}

void UAPS_CrowdFollowingComponent::OnPathFinished(const FPathFollowingResult& Result)
{
	ApplyStrictCrowdOptimizationPolicy(false);
	Super::OnPathFinished(Result);
	DebugLogStrictState(TEXT("OnPathFinished"), INDEX_NONE, INDEX_NONE);
}

void UAPS_CrowdFollowingComponent::SetMoveSegment(int32 SegmentStartIndex)
{
	if (!IsCrowdSimulationEnabled() || !IsStrictAPSFollowingActive())
	{
		Super::SetMoveSegment(SegmentStartIndex);
		DebugLogStrictState(TEXT("SetMoveSegment-Fallback"), SegmentStartIndex, INDEX_NONE);
		return;
	}

#if WITH_RECAST
	FNavMeshPath* NavMeshPath = Path.IsValid() ? Path->CastPath<FNavMeshPath>() : nullptr;
	UCrowdManager* CrowdManager = UCrowdManager::GetCurrent(GetWorld());
	ARecastNavMesh* RecastNavData = NavMeshPath ? Cast<ARecastNavMesh>(NavMeshPath->GetNavigationDataUsed()) : nullptr;
	if (!NavMeshPath || NavMeshPath->PathCorridor.Num() == 0 || !CrowdManager || !RecastNavData || CrowdManager->GetNavData() != RecastNavData)
	{
		Super::SetMoveSegment(SegmentStartIndex);
		DebugLogStrictState(TEXT("SetMoveSegment-FallbackInvalid"), SegmentStartIndex, INDEX_NONE);
		return;
	}

	const int32 LastPolyIdx = NavMeshPath->PathCorridor.Num() - 1;
	PathStartIndex = FMath::Clamp(SegmentStartIndex, 0, LastPolyIdx);
	LastPathPolyIndex = PathStartIndex;

	const int32 StrictPartSize = FMath::Max(1, StrictPathPartSizeInPolys);
	int32 PathPartEndIdx = FMath::Min(PathStartIndex + StrictPartSize, LastPolyIdx);
	bFinalPathPart = (PathPartEndIdx == LastPolyIdx);

	FVector CurrentTargetPt = Path->GetPathPoints().Last().Location;
	FVector PtA, PtB;
	const bool bStartIsNavLink = RecastNavData->GetLinkEndPoints(NavMeshPath->PathCorridor[PathStartIndex], PtA, PtB);
	if (bStartIsNavLink)
	{
		PathStartIndex = FMath::Max(0, PathStartIndex - 1);
	}

	if (!bFinalPathPart)
	{
		const bool bEndIsNavLink = RecastNavData->GetLinkEndPoints(NavMeshPath->PathCorridor[PathPartEndIdx], PtA, PtB);
		const bool bSwitchIsNavLink = (PathPartEndIdx > 0) ? RecastNavData->GetLinkEndPoints(NavMeshPath->PathCorridor[PathPartEndIdx - 1], PtA, PtB) : false;
		if (bEndIsNavLink)
		{
			PathPartEndIdx = FMath::Max(0, PathPartEndIdx - 1);
		}
		if (bSwitchIsNavLink)
		{
			PathPartEndIdx = FMath::Max(0, PathPartEndIdx - 2);
		}
		PathPartEndIdx = FMath::Max(PathStartIndex, PathPartEndIdx);

		RecastNavData->GetPolyCenter(NavMeshPath->PathCorridor[PathPartEndIdx], CurrentTargetPt);
	}
	else if (NavMeshPath->IsPartial())
	{
		RecastNavData->GetClosestPointOnPoly(NavMeshPath->PathCorridor[PathPartEndIdx], Path->GetPathPoints().Last().Location, CurrentTargetPt);
	}

	CrowdAgentMoveDirection = FVector::ZeroVector;
	MoveSegmentDirection = FVector::ZeroVector;
	CurrentDestination.Set(Path->GetBaseActor(), CurrentTargetPt);
	CrowdManager->SetAgentMovePath(this, NavMeshPath, PathStartIndex, PathPartEndIdx, CurrentTargetPt);

	DebugLogStrictState(TEXT("SetMoveSegment-Strict"), PathStartIndex, PathPartEndIdx - PathStartIndex + 1);
#else
	Super::SetMoveSegment(SegmentStartIndex);
	DebugLogStrictState(TEXT("SetMoveSegment-NoRecastFallback"), SegmentStartIndex, INDEX_NONE);
#endif
}

void UAPS_CrowdFollowingComponent::OnNavNodeChanged(NavNodeRef NewPolyRef, NavNodeRef PrevPolyRef, int32 CorridorSize)
{
	if (!IsCrowdSimulationEnabled() || !IsStrictAPSFollowingActive() || Status == EPathFollowingStatus::Idle)
	{
		Super::OnNavNodeChanged(NewPolyRef, PrevPolyRef, CorridorSize);
		return;
	}

	FNavMeshPath* NavPath = Path.IsValid() ? Path->CastPath<FNavMeshPath>() : nullptr;
	if (NavPath)
	{
		for (int32 Index = LastPathPolyIndex; Index < NavPath->PathCorridor.Num(); ++Index)
		{
			if (NavPath->PathCorridor[Index] == NewPolyRef)
			{
				LastPathPolyIndex = Index;
				break;
			}
		}
	}

	const int32 SwitchThreshold = FMath::Max(1, StrictPartSwitchCorridorThreshold);
	if (!bFinalPathPart && CorridorSize <= SwitchThreshold)
	{
		SwitchToNextPathPart();
		DebugLogStrictState(TEXT("OnNavNodeChanged-SwitchPart"), LastPathPolyIndex, CorridorSize);
	}
	else
	{
		DebugLogStrictState(TEXT("OnNavNodeChanged-Continue"), LastPathPolyIndex, CorridorSize);
	}
}
