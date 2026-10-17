/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"
#include "NavigationSystem.h"
#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
#include "NavMesh/RecastHelpers.h"

FORCEINLINE FVector Recast2UnrealPoint(const float* RecastPoint)
{
	const FVector RecastVector(
		static_cast<FVector::FReal>(RecastPoint[0]),
		static_cast<FVector::FReal>(RecastPoint[1]),
		static_cast<FVector::FReal>(RecastPoint[2]));
	return Recast2UnrealPoint(RecastVector);
}
#endif

class AAPS_NavigationData;

/**
 * Low-level Detour-based arc path mode.
 * Builds arc-shaped paths by stitching several low-level segments.
 */
namespace APSPathModeLowLevelArc
{
	FPathFindingResult FindPath(const AAPS_NavigationData* NavData,
								const FNavAgentProperties& AgentProperties,
								const FPathFindingQuery&   Query);
}
