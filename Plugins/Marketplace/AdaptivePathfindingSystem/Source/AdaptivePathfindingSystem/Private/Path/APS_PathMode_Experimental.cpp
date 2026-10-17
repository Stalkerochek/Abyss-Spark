/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Path/APS_PathMode_Experimental.h"

#include "Navigation/APS_NavigationData.h"
#include "Path/APS_PathMode_APSArc.h"
#include "Debug/APS_NavLogs.h"

FPathFindingResult APSPathModeExperimental::FindPath(const AAPS_NavigationData* NavData,
													 const FNavAgentProperties& AgentProperties,
													 const FPathFindingQuery&   Query)
{
	UE_LOG(LogAPSNav, Verbose,
		TEXT("APSPathModeExperimental::FindPath (delegating to APS_Arc for now)"));

	// For now we use the same algorithm as APS_Arc.
	// A new implementation can be put here later.
	return APSPathModeAPSArc::FindPath(NavData, AgentProperties, Query);
}
