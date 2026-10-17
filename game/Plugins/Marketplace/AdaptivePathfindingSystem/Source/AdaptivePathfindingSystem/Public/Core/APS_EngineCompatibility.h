/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
*/

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"

namespace APSCompat
{
	/**
	 * Cross-version TArray::RemoveAt wrapper that preserves the old
	 * bAllowShrinking = false behaviour from UE 4.27 through UE 5.8+.
	 *
	 * UE 5.5 introduced the EAllowShrinking overloads and UE 5.8 removed
	 * the legacy bool overload from TArray, so keep the API difference here
	 * instead of scattering engine-version checks throughout the plugin.
	 */
	template <typename ElementType, typename AllocatorType>
	FORCEINLINE void RemoveAtNoShrink(
		TArray<ElementType, AllocatorType>& Array,
		const int32 Index,
		const int32 Count = 1)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		Array.RemoveAt(Index, Count, EAllowShrinking::No);
#else
		Array.RemoveAt(Index, Count, false);
#endif
	}
}
