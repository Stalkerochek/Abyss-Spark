/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
 * This plugin is downloadable from the Unreal Engine Marketplace
 */

#include "Navigation/APS_AIController.h"

#include "Navigation/APS_CrowdFollowingComponent.h"

AAPS_AIController::AAPS_AIController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UAPS_CrowdFollowingComponent>(TEXT("PathFollowingComponent")))
{
}
