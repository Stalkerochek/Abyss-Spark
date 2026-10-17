/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved 
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#include "Core/AdaptivePathfindingSystem.h"

#define LOCTEXT_NAMESPACE "FAdaptivePathfindingSystemModule"

void FAdaptivePathfindingSystemModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
}

void FAdaptivePathfindingSystemModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FAdaptivePathfindingSystemModule, AdaptivePathfindingSystem)