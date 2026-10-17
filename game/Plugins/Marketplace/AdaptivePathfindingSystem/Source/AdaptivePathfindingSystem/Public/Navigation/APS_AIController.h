/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
 * This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "APS_AIController.generated.h"

/**
 * APS AI controller using UAPS_CrowdFollowingComponent as default PathFollowingComponent.
 */
UCLASS(Blueprintable)
class ADAPTIVEPATHFINDINGSYSTEM_API AAPS_AIController : public AAIController
{
	GENERATED_BODY()

public:
	AAPS_AIController(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
