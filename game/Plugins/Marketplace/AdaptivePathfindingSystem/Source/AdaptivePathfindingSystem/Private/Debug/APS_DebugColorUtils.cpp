/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
 * This plugin is downloadable from the Unreal Engine Marketplace
 */

#include "Debug/APS_DebugColorUtils.h"

#include "Templates/TypeHash.h"
#include "UObject/Object.h"

namespace
{
	static uint32 BuildAPSDebugPathColorHash(const UObject* WorldContextObject, const TArray<FVector>& Path)
	{
		constexpr float PathHashGridScale = 0.05f; // 20-unit buckets.
		uint32 Hash = 0u;

		if (WorldContextObject)
		{
			Hash = GetTypeHash(WorldContextObject->GetFName());
		}

		if (Path.Num() > 0)
		{
			const FVector& Start = Path[0];
			Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Start.X * PathHashGridScale)));
			Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Start.Y * PathHashGridScale)));
			Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Start.Z * PathHashGridScale)));
		}

		if (Path.Num() > 1)
		{
			const FVector& End = Path.Last();
			Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(End.X * PathHashGridScale)));
			Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(End.Y * PathHashGridScale)));
			Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(End.Z * PathHashGridScale)));
		}

		return Hash;
	}
}

FAPSDebugPathColorProfile MakeAPSDebugPathColorProfile(const UObject* WorldContextObject, const TArray<FVector>& Path)
{
	static const FAPSDebugPathColorProfile Palette[] =
	{
		{
			FLinearColor(0.00f, 1.00f, 0.10f, 1.0f),
			FLinearColor(FColor(120, 90, 255)),
			FLinearColor(FColor(200, 110, 255)),
			FLinearColor(FColor(120, 140, 255)),
			FLinearColor(FColor(150, 70, 90)),
			FLinearColor(FColor(255, 30, 30)),
			FLinearColor(FColor(220, 40, 40)),
			FLinearColor(FColor(0, 120, 255)),
			FLinearColor(FColor(0, 200, 255)),
			FLinearColor(FColor(220, 0, 255)),
			FLinearColor(FColor(255, 170, 0)),
			FLinearColor(FColor(0, 220, 255)),
		},
		{
			FLinearColor(0.25f, 1.00f, 0.00f, 1.0f),
			FLinearColor(FColor(102, 82, 255)),
			FLinearColor(FColor(182, 122, 255)),
			FLinearColor(FColor(102, 156, 255)),
			FLinearColor(FColor(165, 72, 92)),
			FLinearColor(FColor(255, 48, 48)),
			FLinearColor(FColor(230, 55, 55)),
			FLinearColor(FColor(20, 128, 255)),
			FLinearColor(FColor(24, 208, 255)),
			FLinearColor(FColor(236, 40, 255)),
			FLinearColor(FColor(255, 178, 24)),
			FLinearColor(FColor(30, 228, 255)),
		},
		{
			FLinearColor(0.00f, 0.85f, 0.25f, 1.0f),
			FLinearColor(FColor(132, 78, 245)),
			FLinearColor(FColor(214, 104, 255)),
			FLinearColor(FColor(118, 128, 250)),
			FLinearColor(FColor(145, 64, 88)),
			FLinearColor(FColor(245, 42, 60)),
			FLinearColor(FColor(215, 38, 64)),
			FLinearColor(FColor(0, 112, 240)),
			FLinearColor(FColor(0, 192, 245)),
			FLinearColor(FColor(206, 12, 242)),
			FLinearColor(FColor(245, 160, 22)),
			FLinearColor(FColor(0, 210, 245)),
		},
		{
			FLinearColor(0.35f, 1.00f, 0.25f, 1.0f),
			FLinearColor(FColor(142, 98, 255)),
			FLinearColor(FColor(220, 132, 255)),
			FLinearColor(FColor(132, 160, 255)),
			FLinearColor(FColor(172, 80, 96)),
			FLinearColor(FColor(255, 60, 70)),
			FLinearColor(FColor(236, 70, 78)),
			FLinearColor(FColor(34, 138, 255)),
			FLinearColor(FColor(48, 214, 255)),
			FLinearColor(FColor(244, 64, 255)),
			FLinearColor(FColor(255, 186, 44)),
			FLinearColor(FColor(54, 232, 255)),
		},
		{
			FLinearColor(0.00f, 0.70f, 0.45f, 1.0f),
			FLinearColor(FColor(98, 112, 245)),
			FLinearColor(FColor(180, 142, 255)),
			FLinearColor(FColor(96, 170, 245)),
			FLinearColor(FColor(156, 66, 86)),
			FLinearColor(FColor(240, 44, 54)),
			FLinearColor(FColor(210, 36, 50)),
			FLinearColor(FColor(0, 102, 228)),
			FLinearColor(FColor(0, 180, 235)),
			FLinearColor(FColor(198, 24, 232)),
			FLinearColor(FColor(238, 154, 18)),
			FLinearColor(FColor(0, 198, 235)),
		},
		{
			FLinearColor(0.55f, 1.00f, 0.15f, 1.0f),
			FLinearColor(FColor(156, 96, 255)),
			FLinearColor(FColor(228, 126, 250)),
			FLinearColor(FColor(150, 150, 255)),
			FLinearColor(FColor(176, 74, 98)),
			FLinearColor(FColor(255, 72, 60)),
			FLinearColor(FColor(242, 82, 64)),
			FLinearColor(FColor(52, 144, 255)),
			FLinearColor(FColor(66, 220, 255)),
			FLinearColor(FColor(252, 78, 244)),
			FLinearColor(FColor(255, 194, 58)),
			FLinearColor(FColor(72, 238, 255)),
		},
		{
			FLinearColor(0.10f, 0.95f, 0.55f, 1.0f),
			FLinearColor(FColor(116, 128, 255)),
			FLinearColor(FColor(194, 146, 255)),
			FLinearColor(FColor(116, 178, 255)),
			FLinearColor(FColor(160, 78, 108)),
			FLinearColor(FColor(250, 58, 82)),
			FLinearColor(FColor(224, 54, 78)),
			FLinearColor(FColor(16, 126, 244)),
			FLinearColor(FColor(24, 202, 246)),
			FLinearColor(FColor(224, 48, 246)),
			FLinearColor(FColor(250, 168, 36)),
			FLinearColor(FColor(28, 222, 248)),
		},
		{
			FLinearColor(0.10f, 0.75f, 0.10f, 1.0f),
			FLinearColor(FColor(136, 104, 240)),
			FLinearColor(FColor(204, 124, 235)),
			FLinearColor(FColor(124, 158, 240)),
			FLinearColor(FColor(148, 60, 84)),
			FLinearColor(FColor(236, 36, 52)),
			FLinearColor(FColor(206, 32, 48)),
			FLinearColor(FColor(0, 108, 220)),
			FLinearColor(FColor(0, 186, 228)),
			FLinearColor(FColor(188, 16, 220)),
			FLinearColor(FColor(230, 148, 20)),
			FLinearColor(FColor(0, 204, 230)),
		},
	};

	const uint32 Hash = BuildAPSDebugPathColorHash(WorldContextObject, Path);
	const uint32 PaletteCount = static_cast<uint32>(sizeof(Palette) / sizeof(Palette[0]));
	return Palette[Hash % PaletteCount];
}
