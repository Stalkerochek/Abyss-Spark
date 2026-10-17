/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"
#include "NavMesh/RecastNavMesh.h"
#include "APS_NavigationData.generated.h"

DECLARE_STATS_GROUP(TEXT("APS"), STATGROUP_APS, STATCAT_Advanced);

DECLARE_CYCLE_STAT(TEXT("APSFindPath"), STAT_APSFindPath, STATGROUP_APS);

UENUM(BlueprintType)
enum class EAPSPathAlgorithm : uint8
{
    EngineOnly         UMETA(DisplayName = "Engine Recast (no APS)"),
    APS_Arc            UMETA(DisplayName = "APS Arc Path"),
    APS_LowLevelTiles  UMETA(DisplayName = "APS Low-Level Tiles"),
    APS_LowLevelArc    UMETA(DisplayName = "APS Low-Level Tiles + Arc"),
    APS_Experimental   UMETA(DisplayName = "APS Experimental"),
};

UCLASS()
class ADAPTIVEPATHFINDINGSYSTEM_API AAPS_NavigationData : public ARecastNavMesh
{
    GENERATED_BODY()

public:
    AAPS_NavigationData();

    virtual void BeginPlay() override;

    /** Delegate that the navigation system calls to perform pathfinding */
    static FPathFindingResult FindPath(const FNavAgentProperties& AgentProperties,
                                       const FPathFindingQuery&   Query);

    /** Which pathfinding algorithm to use */
    UPROPERTY(EditAnywhere, Category="APS|Pathfinding")
    EAPSPathAlgorithm PathAlgorithm = EAPSPathAlgorithm::APS_Arc;

    /** Controls whether low-level APS should prioritize NavArea cost correctness over cheap LOS shortcuts. */
    UPROPERTY(EditAnywhere, Category="APS|Pathfinding|Areas", meta=(ToolTip="When enabled, forces full A* pathfinding to respect NavArea costs by disabling LOS shortcuts (higher CPU cost). When disabled, allows LOS shortcuts for better performance but NavArea costs may have less influence on route choice."))
    bool bRespectNavAreaCosts = true;

    /** Enables building FNavMeshPath polygon corridor data for Crowd Manager / DetourCrowd compatibility. */
    UPROPERTY(EditAnywhere, Category="APS|Crowd", meta=(ToolTip="Builds NavMesh polygon corridor data inside FNavMeshPath for Crowd Manager / DetourCrowd compatibility. Enable this when using APS low-level path modes with crowd following. Adds small extra CPU work and memory per path because APS copies/stores corridor refs. Disable for maximum performance when not using Crowd Manager."))
    bool bBuildCrowdCompatiblePathCorridor = false;

    /** Rebuilds the crowd corridor from final path points after APS arc warping. */
    UPROPERTY(EditAnywhere, Category="APS|Crowd", meta=(EditCondition="bBuildCrowdCompatiblePathCorridor", ToolTip="Rebuilds the Crowd Manager corridor from final path points after APS arc warping. Only used by APS Low-Level Arc mode (ignored by APS Low-Level Tiles). More accurate for curved Low-Level Arc paths, but significantly more expensive because it runs extra Detour queries between path points. Enable only if crowd following needs the final curved corridor; otherwise leave disabled to use the cheaper base corridor."))
    bool bRebuildCrowdCorridorAfterArc = false;

    /** Optional default NavData (if needed) */
    static ANavigationData* DefaultNavData;
    
    /** Public wrapper around the protected GetRecastNavMeshImpl() */
    const FPImplRecastNavMesh* GetRecastImpl() const;

private:
    UFUNCTION(meta = (WorldContext = "WorldContextObject"))
    void InitializeDefaultNavData(const UObject* WorldContextObject);
};
