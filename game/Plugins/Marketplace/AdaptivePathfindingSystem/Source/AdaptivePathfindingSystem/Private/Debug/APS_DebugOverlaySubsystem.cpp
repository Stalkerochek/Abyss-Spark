/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Debug/APS_DebugOverlaySubsystem.h"

#include "Core/APS_EngineCompatibility.h"

#include "Debug/APS_NavLogs.h"
#include "Debug/SAPS_DebugOverlayWidget.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SWeakWidget.h"

namespace
{
	constexpr int32 GAPSMaxOverlayLines = 4096;
	constexpr int32 GAPSMaxOverlayPoints = 4096;
	constexpr int32 GAPSOverlayZOrder = 10000;
}

void UAPS_DebugOverlaySubsystem::Deinitialize()
{
	if (IsInGameThread() && GEngine && GEngine->GameViewport && ViewportWidget.IsValid())
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(ViewportWidget.ToSharedRef());
	}

	ViewportWidget.Reset();
	OverlayWidget.Reset();
	Lines.Reset();
	Points.Reset();

	Super::Deinitialize();
}

bool UAPS_DebugOverlaySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UAPS_DebugOverlaySubsystem::AddLine(const FVector& A, const FVector& B, const FLinearColor& Color, float Thickness, float LifeTime)
{
	if (!IsInGameThread())
	{
		return;
	}

	CleanupExpired();

	Lines.Emplace(FAPSDebugOverlayLine{
		A,
		B,
		Color,
		FMath::Max(Thickness, 1.0f),
		GetExpireTime(LifeTime)
	});

	TrimToCaps();

	if (CVarAPSPathDebugMode.GetValueOnAnyThread() > 0)
	{
		EnsureWidget();
	}
}

void UAPS_DebugOverlaySubsystem::AddPoint(const FVector& Location, const FLinearColor& Color, float Size, float LifeTime)
{
	if (!IsInGameThread())
	{
		return;
	}

	CleanupExpired();

	Points.Emplace(FAPSDebugOverlayPoint{
		Location,
		Color,
		FMath::Max(Size, 1.0f),
		GetExpireTime(LifeTime)
	});

	TrimToCaps();

	if (CVarAPSPathDebugMode.GetValueOnAnyThread() > 0)
	{
		EnsureWidget();
	}
}

void UAPS_DebugOverlaySubsystem::AddPolyline(const TArray<FVector>& InPoints, const FLinearColor& Color, float Thickness, float PointSize, float LifeTime)
{
	if (!IsInGameThread() || InPoints.Num() == 0)
	{
		return;
	}

	CleanupExpired();

	const float ExpireTime = GetExpireTime(LifeTime);

	if (InPoints.Num() > 1)
	{
		for (int32 PointIndex = 0; PointIndex < InPoints.Num() - 1; ++PointIndex)
		{
			Lines.Emplace(FAPSDebugOverlayLine{
				InPoints[PointIndex],
				InPoints[PointIndex + 1],
				Color,
				FMath::Max(Thickness, 1.0f),
				ExpireTime
			});
		}
	}

	if (PointSize > 0.0f)
	{
		for (const FVector& PointLocation : InPoints)
		{
			Points.Emplace(FAPSDebugOverlayPoint{
				PointLocation,
				Color,
				FMath::Max(PointSize, 1.0f),
				ExpireTime
			});
		}
	}

	TrimToCaps();

	if (CVarAPSPathDebugMode.GetValueOnAnyThread() > 0)
	{
		EnsureWidget();
	}
}

void UAPS_DebugOverlaySubsystem::Snapshot(TArray<FAPSDebugOverlayLine>& OutLines, TArray<FAPSDebugOverlayPoint>& OutPoints) const
{
	OutLines.Reset();
	OutPoints.Reset();

	if (!IsInGameThread() || CVarAPSPathDebugMode.GetValueOnAnyThread() <= 0)
	{
		return;
	}

	const_cast<UAPS_DebugOverlaySubsystem*>(this)->CleanupExpired();
	OutLines = Lines;
	OutPoints = Points;
}

void UAPS_DebugOverlaySubsystem::EnsureWidget()
{
	if (!IsInGameThread() || CVarAPSPathDebugMode.GetValueOnAnyThread() <= 0)
	{
		return;
	}

	if (OverlayWidget.IsValid())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !GEngine || !GEngine->GameViewport)
	{
		return;
	}

	SAssignNew(OverlayWidget, SAPS_DebugOverlayWidget, World);
	ViewportWidget = SNew(SWeakWidget).PossiblyNullContent(OverlayWidget.ToSharedRef());
	GEngine->GameViewport->AddViewportWidgetContent(ViewportWidget.ToSharedRef(), GAPSOverlayZOrder);
}

bool UAPS_DebugOverlaySubsystem::HasWidget() const
{
	return OverlayWidget.IsValid() && ViewportWidget.IsValid();
}

void UAPS_DebugOverlaySubsystem::CleanupExpired()
{
	const float CurrentTime = GetCurrentTimeSeconds();

	Lines.RemoveAllSwap([CurrentTime](const FAPSDebugOverlayLine& Line)
	{
		return Line.ExpireTime <= CurrentTime;
	});

	Points.RemoveAllSwap([CurrentTime](const FAPSDebugOverlayPoint& Point)
	{
		return Point.ExpireTime <= CurrentTime;
	});
}

void UAPS_DebugOverlaySubsystem::TrimToCaps()
{
	if (Lines.Num() > GAPSMaxOverlayLines)
	{
		APSCompat::RemoveAtNoShrink(Lines, 0, Lines.Num() - GAPSMaxOverlayLines);
	}

	if (Points.Num() > GAPSMaxOverlayPoints)
	{
		APSCompat::RemoveAtNoShrink(Points, 0, Points.Num() - GAPSMaxOverlayPoints);
	}
}

float UAPS_DebugOverlaySubsystem::GetCurrentTimeSeconds() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0f;
}

float UAPS_DebugOverlaySubsystem::GetExpireTime(float LifeTime) const
{
	return GetCurrentTimeSeconds() + FMath::Max(LifeTime, 0.0f);
}
