/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "APS_MoveToInterface.generated.h"

UENUM(BlueprintType)
enum class EAPSArcMidpointMode : uint8
{
	SmoothCurve  UMETA(DisplayName="Smooth Curve"),   // NumMidPoints = насколько плавная одна дуга
	Wavy         UMETA(DisplayName="Wavy / Humps"),   // NumMidPoints = сколько горбов/волн
};


UINTERFACE(Blueprintable)
class UAPS_MoveToInterface : public UInterface
{
	GENERATED_BODY()
};

class IAPS_MoveToInterface
{
	GENERATED_BODY()

public:
	
	// LaneOffset is an additional lateral offset in Unreal units used around corners;
	// positive values make paths wider around turns, 0 disables lane spreading.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="APS|MoveTo")
	void GetMoveToParameters(float& ArcStrength, int32& NumMidPoints, EAPSArcMidpointMode& MidpointMode, float& LaneOffset) const;

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "APS|PathFinding")
	void FindCustomPath(const FVector& StartLocation, const FVector& EndLocation, TArray<FVector>& OutPath, float ArcStrength, UWorld* World, bool& bUseCustomPath, bool& bFound) const; 
};
