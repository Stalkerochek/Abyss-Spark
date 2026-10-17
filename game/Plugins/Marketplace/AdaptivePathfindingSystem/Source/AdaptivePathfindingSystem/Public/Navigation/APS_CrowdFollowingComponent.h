/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
 * This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"
#include "Navigation/CrowdFollowingComponent.h"
#include "APS_CrowdFollowingComponent.generated.h"

/**
 * APS-aware crowd following component that can reduce DetourCrowd shortcutting for curved APS paths.
 */
UCLASS(ClassGroup=AI, BlueprintType, Blueprintable, meta=(BlueprintSpawnableComponent))
class ADAPTIVEPATHFINDINGSYSTEM_API UAPS_CrowdFollowingComponent : public UCrowdFollowingComponent
{
	GENERATED_BODY()

public:
	UAPS_CrowdFollowingComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * Forces APS crowd following to preserve APS path points as intermediate waypoints instead of allowing crowd steering to aggressively shortcut through the navmesh corridor.
	 * Enable for APS Low-Level Arc paths when visual curved paths must be followed more closely.
	 * In UE4.27 this is implemented as a crowd-safe best effort by using tighter corridor parts/switches, so local avoidance is preserved while reducing shortcutting.
	 * This can reduce Crowd's natural smoothing/shortcutting and may add small per-tick steering/path-following overhead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="APS|Crowd Following", meta=(ToolTip="Forces APS crowd following to preserve APS path points as intermediate waypoints instead of allowing crowd steering to aggressively shortcut through the navmesh corridor. Enable for APS Low-Level Arc paths when visual curved paths must be followed more closely. In UE4.27 this uses tighter crowd corridor parts/switches as a crowd-safe best effort, preserving local avoidance while reducing shortcutting. Can reduce Crowd's natural smoothing/shortcutting and may add small per-tick steering/path-following overhead."))
	bool bFollowAPSPathPointsStrictly = true;

	/**
	 * Minimum number of path points required before strict APS behavior is enabled.
	 * Keep this above 2 to avoid extra overhead on very short/simple paths where strict waypoint stepping is not useful.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="APS|Crowd Following", meta=(ClampMin="2", ToolTip="Minimum number of path points required before strict APS behavior is enabled. Keep above 2 to avoid extra overhead on very short/simple paths where strict waypoint stepping is not useful."))
	int32 MinPathPointsForStrictFollowing = 3;

	/**
	 * Max number of corridor polys sent to DetourCrowd in each strict path part.
	 * Lower values force more frequent intermediate retargeting (closer to APS curved points) but increase Crowd/path-following work.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="APS|Crowd Following", meta=(ClampMin="1", ToolTip="Max number of corridor polys sent to DetourCrowd in each strict path part. Lower values force more frequent intermediate retargeting (closer to APS curved points) but increase Crowd/path-following work."))
	int32 StrictPathPartSizeInPolys = 3;

	/**
	 * Corridor size threshold used to switch to the next strict path part.
	 * Higher values switch earlier (closer waypoint adherence, less shortcutting) with extra update cost.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="APS|Crowd Following", meta=(ClampMin="1", ToolTip="Corridor size threshold used to switch to the next strict path part. Higher values switch earlier for closer waypoint adherence (less shortcutting), but may increase update cost and reduce natural crowd smoothing."))
	int32 StrictPartSwitchCorridorThreshold = 4;

	/**
	 * Disables Crowd visibility/topology optimizations while strict APS behavior is active.
	 * This further reduces shortcutting, but may look less smooth and can add CPU overhead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="APS|Crowd Following", meta=(ToolTip="Disables Crowd visibility/topology optimizations while strict APS behavior is active. This further reduces corridor shortcutting and keeps motion closer to APS curved points, but may reduce natural smoothing and add CPU overhead."))
	bool bDisableCrowdShortcutOptimizationsWhenStrict = true;

	/**
	 * Enables verbose APS crowd-following logs (also requires aps.Nav.Debug for most runtime spam).
	 * Useful for validating strict/fallback mode and current segment transitions.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="APS|Crowd Following", meta=(ToolTip="Enables verbose APS crowd-following logs (also gated by aps.Nav.Debug for detailed runtime updates). Useful for validating strict/fallback mode decisions, path-point count, and segment transitions."))
	bool bLogAPSStrictFollowing = false;

protected:
	virtual void OnPathUpdated() override;
	virtual void OnPathFinished(const FPathFollowingResult& Result) override;
	virtual void SetMoveSegment(int32 SegmentStartIndex) override;
	virtual void OnNavNodeChanged(NavNodeRef NewPolyRef, NavNodeRef PrevPolyRef, int32 CorridorSize) override;

private:
	bool IsStrictAPSFollowingActive() const;
	void ApplyStrictCrowdOptimizationPolicy(bool bStrictActive);
	void DebugLogStrictState(const TCHAR* Context, int32 SegmentStartIndex, int32 CorridorSize) const;

	bool bCachedOptimizeVisibility = true;
	bool bCachedOptimizeTopology = true;
	bool bHasCachedCrowdOptimizationState = false;
};
