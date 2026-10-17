/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"
#include "Async/Async.h"
#include "APS_Visualizer.generated.h"

/**
 * Visualizer for rendering paths in the game world.
 */
UCLASS()
class ADAPTIVEPATHFINDINGSYSTEM_API UAPS_Visualizer : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Visualizes a static path using the APS debug overlay when available.
     * @param WorldContextObject - The context object for the world.
     * @param Path - Array of points representing the path.
     * @param LineThickness - Thickness of the debug lines.
     * @param SphereSize - Size hint for optional debug point markers.
     * @param Duration - Duration the debug visuals will persist.
     */
    void VisualizePath(const UObject* WorldContextObject, const TArray<FVector>& Path, float LineThickness = 2.0f, float SphereSize = 20.0f, float Duration = 5.0f) const;

private:
   
};
