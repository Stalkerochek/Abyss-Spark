/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Path/APS_PathMode_Engine.h"

#include "Navigation/APS_NavigationData.h"
#include "NavMesh/RecastNavMesh.h"
#include "Debug/APS_NavLogs.h"

FPathFindingResult APSPathModeEngine::FindPath(const AAPS_NavigationData* NavData,
											   const FNavAgentProperties& AgentProperties,
											   const FPathFindingQuery&   Query)
{
	if (!NavData)
	{
		UE_LOG(LogAPSNav, Error, TEXT("APSPathModeEngine::FindPath: NavData is null"));
		return ENavigationQueryResult::Error;
	}

	// A completely vanilla engine Recast path
	return NavData->ARecastNavMesh::FindPath(AgentProperties, Query);
}
