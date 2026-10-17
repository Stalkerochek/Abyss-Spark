/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"
#include "AI/Navigation/NavigationTypes.h"   // ENavigationQueryResult, FPathFindingQuery

// Detour types: dtNavMesh, dtNavMeshQuery, dtQueryFilter, dtPolyRef, dtStatus, dtReal
#include "Detour/DetourNavMeshQuery.h"

// Forward declaration of UE navigation filter
struct FNavigationQueryFilter;
struct FNavMeshPath;

/**
 * Common base for APS low-level path modes.
 * Provides Detour-related helpers and a shared path query wrapper.
 */
class ADAPTIVEPATHFINDINGSYSTEM_API FAPSPathModeLowLevelBase
{
public:
	/** Extract internal dtQueryFilter from a UE navigation query filter. */
	static const dtQueryFilter* GetDetourFilter(const FNavigationQueryFilter& NavFilter);

	/** Map Detour dtStatus flags to UE ENavigationQueryResult. */
	static ENavigationQueryResult::Type DTStatusToNavResult(dtStatus Status);

	/**
	 * Returns whether low-level APS modes may use a cheap line-of-sight shortcut for this query.
	 * If NavArea costs must be respected, this returns false because LOS shortcuts bypass A* cost evaluation.
	 */
	static bool ShouldUseLineOfSightShortcut(bool bRequestedByMode, bool bRespectNavAreaCosts);

	/**
	 * Fill UE navmesh path corridor from an already-built Detour polygon corridor.
	 * Intended for Crowd Manager / DetourCrowd compatibility.
	 */
	static void FillNavMeshPathCorridorFromRefs(FNavMeshPath* NavMeshPath,
											  const TArray<dtPolyRef>& Corridor);

public:
	/**
	 * Thin wrapper around dtNavMeshQuery used by all APS low-level
	 * path modes (tiles / low-level arc / async tasks).
	 */
	struct FAPSNavMeshQuery
	{
		/** Navmesh instance used for all queries. */
		const dtNavMesh*     NavMesh       = nullptr;

		/** Internal Detour query object (owned by this struct). */
		dtNavMeshQuery*      InternalQuery = nullptr;

		/** Active Detour filter used for pathfinding. */
		const dtQueryFilter* Filter        = nullptr;

		/** Optional per-query node limit for A* (0 = use default). */
		int32                MaxNodes      = 0;

		FAPSNavMeshQuery();
		~FAPSNavMeshQuery();

		/** Initialize the internal Detour query object. */
		bool Init(const dtNavMesh* InNavMesh,
				  const dtQueryFilter* InFilter,
				  int32 InMaxNodes);

		/**
		 * Wrapper around dtNavMeshQuery::findPath.
		 * Produces a polygon corridor in OutCorridor and optional total cost.
		 *
		 * NOTE:
		 *  - The API uses float for positions and cost on the APS side.
		 *    Inside the implementation we convert to dtReal where needed
		 *    to stay compatible with both UE4 (float-based Detour) and
		 *    UE5 (dtReal-based Detour).
		 */
		dtStatus FindPath(dtPolyRef StartRef,
						  dtPolyRef EndRef,
						  const float* StartPos,
						  const float* EndPos,
						  float CostLimit,
						  TArray<dtPolyRef>& OutCorridor,
						  float* OutTotalCost);
	};

	/**
	 * Rebuild UE navmesh path corridor from final path points.
	 * This is heavier than FillNavMeshPathCorridorFromRefs because it runs
	 * Detour path queries for each adjacent point pair. Use only when the final
	 * path points were modified after the original corridor was produced.
	 */
	static bool RebuildNavMeshPathCorridorFromPoints(FNavMeshPath* NavMeshPath,
											   FAPSNavMeshQuery& APSQuery,
											   const dtQueryFilter* DtFilter,
											   const float Extent[3],
											   TArray<FNavPathPoint>& PathPoints,
											   float SegmentCostLimit = BIG_NUMBER);
};
