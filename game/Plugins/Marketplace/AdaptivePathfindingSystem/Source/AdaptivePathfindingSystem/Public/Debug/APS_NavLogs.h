/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// Log category for all navigation
DECLARE_LOG_CATEGORY_EXTERN(LogAPSNav, Log, All);

// Console flags
extern TAutoConsoleVariable<int32> CVarAPSNavDebug;       // 0/1 — enable detailed logs
extern TAutoConsoleVariable<int32> CVarAPSPathDebugMode;  // 0=off, 1=simple, 2=dynamic

