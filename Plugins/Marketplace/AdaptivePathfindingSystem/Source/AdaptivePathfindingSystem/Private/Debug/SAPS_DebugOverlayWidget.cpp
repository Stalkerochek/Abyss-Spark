/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Debug/SAPS_DebugOverlayWidget.h"

#include "Debug/APS_DebugOverlaySubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

void SAPS_DebugOverlayWidget::Construct(const FArguments& InArgs, UWorld* InWorld)
{
	World = InWorld;
}

int32 SAPS_DebugOverlayWidget::OnPaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	UWorld* ResolvedWorld = World.Get();
	if (!ResolvedWorld)
	{
		return LayerId;
	}

	UAPS_DebugOverlaySubsystem* OverlaySubsystem = ResolvedWorld->GetSubsystem<UAPS_DebugOverlaySubsystem>();
	APlayerController* PlayerController = ResolvedWorld->GetFirstPlayerController();
	if (!OverlaySubsystem || !PlayerController)
	{
		return LayerId;
	}

	TArray<FAPSDebugOverlayLine> Lines;
	TArray<FAPSDebugOverlayPoint> Points;
	OverlaySubsystem->Snapshot(Lines, Points);

	if (Lines.Num() == 0 && Points.Num() == 0)
	{
		return LayerId;
	}

	int32 MaxLayerId = LayerId;
	TArray<FVector2D> LocalPoints;
	LocalPoints.Reserve(2);

	for (const FAPSDebugOverlayLine& Line : Lines)
	{
		FVector2D LocalA;
		FVector2D LocalB;
		if (!ProjectToLocal(PlayerController, AllottedGeometry, Line.A, LocalA) ||
			!ProjectToLocal(PlayerController, AllottedGeometry, Line.B, LocalB))
		{
			continue;
		}

		LocalPoints.Reset();
		LocalPoints.Add(LocalA);
		LocalPoints.Add(LocalB);

		FSlateDrawElement::MakeLines(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(),
			LocalPoints,
			ESlateDrawEffect::None,
			Line.Color,
			true,
			Line.Thickness);

		MaxLayerId = FMath::Max(MaxLayerId, LayerId);
	}

	if (Points.Num() > 0)
	{
		const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));

		for (const FAPSDebugOverlayPoint& Point : Points)
		{
			FVector2D LocalPoint;
			if (!ProjectToLocal(PlayerController, AllottedGeometry, Point.Location, LocalPoint))
			{
				continue;
			}

			const FVector2D PointSize(Point.Size, Point.Size);
			const FVector2D HalfSize = PointSize * 0.5f;

			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId + 1,
				AllottedGeometry.ToPaintGeometry(PointSize, FSlateLayoutTransform(LocalPoint - HalfSize)),
				WhiteBrush,
				ESlateDrawEffect::None,
				Point.Color);
		}

		MaxLayerId = FMath::Max(MaxLayerId, LayerId + 1);
	}

	return MaxLayerId + 1;
}

FVector2D SAPS_DebugOverlayWidget::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(1.0f, 1.0f);
}

bool SAPS_DebugOverlayWidget::ComputeVolatility() const
{
	return true;
}

bool SAPS_DebugOverlayWidget::ProjectToLocal(
	APlayerController* PlayerController,
	const FGeometry& AllottedGeometry,
	const FVector& WorldLocation,
	FVector2D& OutLocalPoint) const
{
	if (!PlayerController)
	{
		return false;
	}

	int32 ViewX = 0;
	int32 ViewY = 0;
	PlayerController->GetViewportSize(ViewX, ViewY);

	if (ViewX <= 0 || ViewY <= 0)
	{
		return false;
	}

	FVector2D ScreenPoint;
	if (!PlayerController->ProjectWorldLocationToScreen(WorldLocation, ScreenPoint, true))
	{
		return false;
	}

	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	OutLocalPoint.X = (ScreenPoint.X / float(ViewX)) * LocalSize.X;
	OutLocalPoint.Y = (ScreenPoint.Y / float(ViewY)) * LocalSize.Y;
	return true;
}
