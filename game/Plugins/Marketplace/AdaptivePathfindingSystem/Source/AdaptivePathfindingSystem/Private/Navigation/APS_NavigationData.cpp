/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
 * This plugin is downloadable from the Unreal Engine Marketplace
 */

#include "Navigation/APS_NavigationData.h"

#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"

#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"

#include "Debug/APS_NavLogs.h"

// path modes
#include "Path/APS_PathMode_Engine.h"
#include "Path/APS_PathMode_APSArc.h"
#include "Path/APS_PathMode_LowLevelTiles.h"
#include "Path/APS_PathMode_LowLevelArc.h"
#include "Path/APS_PathMode_Experimental.h"



// Globals (storing DefaultNavData at will)
ANavigationData* AAPS_NavigationData::DefaultNavData = nullptr;

AAPS_NavigationData::AAPS_NavigationData()
{
    bCanBeMainNavData = true;

    // The nav system will call this static method directly
    FindPathImplementation = FindPath;

    // By default we use the arc-based APS mode
    PathAlgorithm = EAPSPathAlgorithm::APS_LowLevelArc;
    
}

void AAPS_NavigationData::BeginPlay()
{
    Super::BeginPlay();
    InitializeDefaultNavData(GetWorld());
}

// -----------------------------------------------------------------
// Public wrapper around GetRecastNavMeshImpl()
// -----------------------------------------------------------------

const FPImplRecastNavMesh* AAPS_NavigationData::GetRecastImpl() const
{
    // GetRecastNavMeshImpl() — protected в ARecastNavMesh, но
    // здесь мы внутри наследника, поэтому доступ разрешён.
    return GetRecastNavMeshImpl();
}

// -----------------------------------------------------------------
// Static FindPath — dispatcher for PathAlgorithm
// -----------------------------------------------------------------

FPathFindingResult AAPS_NavigationData::FindPath(const FNavAgentProperties& AgentProperties,
                                                 const FPathFindingQuery&   Query)
{
    SCOPE_CYCLE_COUNTER(STAT_APSFindPath);
    
   TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("[AAPS_NavigationData::FindPath]"));
    
    const ANavigationData* Self = Query.NavData.Get();
    if (!Self)
    {
        UE_LOG(LogAPSNav, Error, TEXT("AAPS_NavigationData::FindPath: NavData is null"));
        return ENavigationQueryResult::Error;
    }

    const ARecastNavMesh* RecastSelf = Cast<const ARecastNavMesh>(Self);
    if (!RecastSelf)
    {
        UE_LOG(LogAPSNav, Error, TEXT("AAPS_NavigationData::FindPath: NavData is not ARecastNavMesh"));
        return ENavigationQueryResult::Error;
    }

    const AAPS_NavigationData* APSNav = Cast<const AAPS_NavigationData>(Self);
    if (!APSNav)
    {
        // Just in case the delegate was somehow bound to a different class
        return const_cast<ARecastNavMesh*>(RecastSelf)->ARecastNavMesh::FindPath(AgentProperties, Query);
    }

    
    switch (APSNav->PathAlgorithm)
    {
    case EAPSPathAlgorithm::EngineOnly:
        return APSPathModeEngine::FindPath(APSNav, AgentProperties, Query);

    case EAPSPathAlgorithm::APS_Arc:
        return APSPathModeAPSArc::FindPath(APSNav, AgentProperties, Query);

    case EAPSPathAlgorithm::APS_LowLevelTiles:
        return APSPathModeLowLevelTiles::FindPath(APSNav, AgentProperties, Query);
        
    case EAPSPathAlgorithm::APS_LowLevelArc:
        return APSPathModeLowLevelArc::FindPath(APSNav, AgentProperties, Query);

    case EAPSPathAlgorithm::APS_Experimental:
        return APSPathModeExperimental::FindPath(APSNav, AgentProperties, Query);

    default:
        return APSPathModeEngine::FindPath(APSNav, AgentProperties, Query);
    }
    
}

// -----------------------------------------------------------------
//  Initialization of DefaultNavData
// -----------------------------------------------------------------

void AAPS_NavigationData::InitializeDefaultNavData(const UObject* WorldContextObject)
{
    const UWorld* World = Cast<UWorld>(WorldContextObject);
    if (!World)
    {
        UE_LOG(LogAPSNav, Warning, TEXT("InitializeDefaultNavData: World is null"));
        return;
    }

    TArray<AActor*> All;
    UGameplayStatics::GetAllActorsOfClass(const_cast<UObject*>(WorldContextObject),
        ARecastNavMesh::StaticClass(), All);

    for (AActor* A : All)
    {
        ARecastNavMesh* N = Cast<ARecastNavMesh>(A);
        if (N && N != this)
        {
            DefaultNavData = N;
            break;
        }
    }

    if (!DefaultNavData)
    {
        UE_LOG(LogAPSNav, Warning, TEXT("InitializeDefaultNavData: Default NavData not found"));
    }
}
