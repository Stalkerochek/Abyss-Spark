/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"
#include "NavigationSystem.h"

class AAPS_NavigationData;

namespace APSPathModeAPSArc
{
	/** Arc-based APS pathfinding on top of standard Recast */
	FPathFindingResult FindPath(const AAPS_NavigationData* NavData,
								const FNavAgentProperties& AgentProperties,
								const FPathFindingQuery&   Query);
}
