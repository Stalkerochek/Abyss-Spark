/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SLeafWidget.h"

class APlayerController;
class UWorld;

class ADAPTIVEPATHFINDINGSYSTEM_API SAPS_DebugOverlayWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SAPS_DebugOverlayWidget)
	{}
	SLATE_END_ARGS();

	void Construct(const FArguments& InArgs, UWorld* InWorld);

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	virtual bool ComputeVolatility() const override;

private:
	bool ProjectToLocal(
		APlayerController* PlayerController,
		const FGeometry& AllottedGeometry,
		const FVector& WorldLocation,
		FVector2D& OutLocalPoint) const;

	TWeakObjectPtr<UWorld> World;
};
