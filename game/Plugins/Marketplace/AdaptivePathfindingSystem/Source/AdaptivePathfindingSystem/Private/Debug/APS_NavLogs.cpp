/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Debug/APS_NavLogs.h"

DEFINE_LOG_CATEGORY(LogAPSNav);

TAutoConsoleVariable<int32> CVarAPSNavDebug(
	TEXT("aps.Nav.Debug"),
	0,
	TEXT("APS navigation debug logs (0=off, 1=on)"),
	ECVF_Cheat
);

TAutoConsoleVariable<int32> CVarAPSPathDebugMode(
	TEXT("aps.Debug.PathMode"),
	0,
	TEXT("APS path debug mode: 0=off, 1=simple, 2=dynamic"),
	ECVF_Default
);

