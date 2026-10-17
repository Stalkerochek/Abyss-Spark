/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Visualization/APS_Visualizer.h"

#include "Debug/APS_DebugColorUtils.h"
#include "Debug/APS_DebugOverlaySubsystem.h"

namespace
{
	bool TryVisualizePathWithOverlay(UWorld* World, const TArray<FVector>& Path, float LineThickness, float SphereSize, float Duration, const FLinearColor& PathColor)
	{
		if (!World)
		{
			return false;
		}

		UAPS_DebugOverlaySubsystem* OverlaySubsystem = World->GetSubsystem<UAPS_DebugOverlaySubsystem>();
		if (!OverlaySubsystem)
		{
			return false;
		}

		OverlaySubsystem->EnsureWidget();
		if (!OverlaySubsystem->HasWidget())
		{
			return false;
		}

		const float PointSize = (SphereSize > 0.0f)
			? FMath::Clamp(SphereSize * 0.25f, 4.0f, 12.0f)
			: 0.0f;

		OverlaySubsystem->AddPolyline(
			Path,
			PathColor,
			FMath::Max(LineThickness, 1.0f),
			PointSize,
			Duration);

		return true;
	}

	void DrawPathFallback(UWorld* World, const TArray<FVector>& Path, float LineThickness, float SphereSize, float Duration, const FLinearColor& PathColor)
	{
		if (!World)
		{
			return;
		}

		const FColor DebugPathColor = PathColor.ToFColor(true);

		for (int32 PointIndex = 0; PointIndex < Path.Num() - 1; ++PointIndex)
		{
			DrawDebugLine(
				World,
				Path[PointIndex],
				Path[PointIndex + 1],
				DebugPathColor,
				false,
				Duration,
				0,
				LineThickness);
		}

		if (SphereSize > 0.0f)
		{
			const float PointSize = FMath::Clamp(SphereSize * 0.35f, 6.0f, 18.0f);
			for (const FVector& PointLocation : Path)
			{
				DrawDebugPoint(
					World,
					PointLocation,
					PointSize,
					DebugPathColor,
					false,
					Duration,
					0);
			}
		}
	}
}



/**
 * Visualizes a path using the APS overlay, with DrawDebug fallback when needed.
 * Ensures the path has at least two points before drawing.
 */
void UAPS_Visualizer::VisualizePath(
    const UObject*    WorldContextObject,
    const TArray<FVector>& Path,
    float             LineThickness,
    float             SphereSize,
    float             Duration) const
{
    if (Path.Num() < 2)
    {
        UE_LOG(LogTemp, Warning, TEXT("APS_Visualizer: path must contain at least two points."));
        return;
    }

    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("APS_Visualizer: invalid world context."));
        return;
    }

	// Make a copy for the lambda capture by value (safe for async).
	const TArray<FVector> LocalPath = Path;
	const FAPSDebugPathColorProfile ColorProfile = MakeAPSDebugPathColorProfile(WorldContextObject, LocalPath);
	const FLinearColor PathColor = ColorProfile.FinalPath;

    // Overlay/widget work and DrawDebug fallbacks must happen on the game thread.
    AsyncTask(ENamedThreads::GameThread, [World, LocalPath, LineThickness, SphereSize, Duration, PathColor]()
    {
        if (TryVisualizePathWithOverlay(World, LocalPath, LineThickness, SphereSize, Duration, PathColor))
        {
            return;
        }

        DrawPathFallback(World, LocalPath, LineThickness, SphereSize, Duration, PathColor);
    });
}
