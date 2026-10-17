/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */
#pragma once

#include "CoreMinimal.h"
#include "NavigationSystem.h"

class AAPS_NavigationData;

/**
 * Low-level Detour-based pathfinding mode scaffold.
 * For now it still uses dtNavMeshQuery::findPath internally,
 * but it exposes a custom query object that owns dtNodePool and dtNodeQueue
 * so we can later replace the inner A* loop.
 */
namespace APSPathModeLowLevelTiles
{
	FPathFindingResult FindPath(const AAPS_NavigationData* NavData,
								const FNavAgentProperties& AgentProperties,
								const FPathFindingQuery&   Query);
}
