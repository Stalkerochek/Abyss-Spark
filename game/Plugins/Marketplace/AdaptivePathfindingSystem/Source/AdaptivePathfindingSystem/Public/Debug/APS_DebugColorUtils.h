/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
 * This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"

struct FAPSDebugPathColorProfile
{
	FLinearColor FinalPath;
	FLinearColor CandidateAnchor;
	FLinearColor CandidatePeak;
	FLinearColor CandidateLine;
	FLinearColor FailedAnchor;
	FLinearColor FailedWaypoint;
	FLinearColor FailedLine;
	FLinearColor InjectedAnchor;
	FLinearColor InjectedPeak;
	FLinearColor SideWaypoint;
	FLinearColor InjectedRoute;
	FLinearColor AnchorLink;
};

FAPSDebugPathColorProfile MakeAPSDebugPathColorProfile(const UObject* WorldContextObject, const TArray<FVector>& Path);
