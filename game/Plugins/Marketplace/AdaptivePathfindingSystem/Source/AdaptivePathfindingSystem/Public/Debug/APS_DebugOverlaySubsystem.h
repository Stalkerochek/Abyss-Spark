/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/SharedPointer.h"

#include "APS_DebugOverlaySubsystem.generated.h"

class SAPS_DebugOverlayWidget;
class SWidget;

struct FAPSDebugOverlayLine
{
	FVector A = FVector::ZeroVector;
	FVector B = FVector::ZeroVector;
	FLinearColor Color = FLinearColor::White;
	float Thickness = 1.0f;
	float ExpireTime = 0.0f;
};

struct FAPSDebugOverlayPoint
{
	FVector Location = FVector::ZeroVector;
	FLinearColor Color = FLinearColor::White;
	float Size = 6.0f;
	float ExpireTime = 0.0f;
};

UCLASS()
class ADAPTIVEPATHFINDINGSYSTEM_API UAPS_DebugOverlaySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	void AddLine(const FVector& A, const FVector& B, const FLinearColor& Color, float Thickness, float LifeTime);
	void AddPoint(const FVector& Location, const FLinearColor& Color, float Size, float LifeTime);
	void AddPolyline(const TArray<FVector>& Points, const FLinearColor& Color, float Thickness, float PointSize, float LifeTime);
	void Snapshot(TArray<FAPSDebugOverlayLine>& OutLines, TArray<FAPSDebugOverlayPoint>& OutPoints) const;
	void EnsureWidget();
	bool HasWidget() const;

private:
	void CleanupExpired();
	void TrimToCaps();
	float GetCurrentTimeSeconds() const;
	float GetExpireTime(float LifeTime) const;

	TArray<FAPSDebugOverlayLine> Lines;
	TArray<FAPSDebugOverlayPoint> Points;
	TSharedPtr<SAPS_DebugOverlayWidget> OverlayWidget;
	TSharedPtr<SWidget> ViewportWidget;
};
