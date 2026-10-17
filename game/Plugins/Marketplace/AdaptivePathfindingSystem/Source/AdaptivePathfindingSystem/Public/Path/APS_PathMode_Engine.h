/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"
#include "NavigationSystem.h"

class AAPS_NavigationData;

namespace APSPathModeEngine
{
	/** A completely vanilla engine Recast path (without APS arcs) */
	FPathFindingResult FindPath(const AAPS_NavigationData* NavData,
								const FNavAgentProperties& AgentProperties,
								const FPathFindingQuery&   Query);
}
