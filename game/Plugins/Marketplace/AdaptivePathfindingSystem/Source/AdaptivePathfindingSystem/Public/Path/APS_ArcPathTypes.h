/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */
#pragma once

#include "CoreMinimal.h"
#include "Detour/DetourNavMeshQuery.h"

/**
 * Common APS arc path request.
 * Plain struct, no UObject / reflection.
 */
struct FAPSArcPathRequest
{
	// Detour data (read-only, owned by navmesh)
	const dtNavMesh*     DetourNavMesh = nullptr;
	const dtQueryFilter* QueryFilter   = nullptr;

	// World-space settings
	FVector Start       = FVector::ZeroVector;
	FVector End         = FVector::ZeroVector;
	FVector QueryExtent = FVector::ZeroVector;

	// Pathfinding settings
	float CostLimit      = TNumericLimits<float>::Max();
	bool  bAllowPartial  = true;

	// Arc parameters
	float MinArcDistance = 300.0f; // distance below which arc is disabled
	float ArcStrength    = 0.0f;   // >0 bends to the right, <0 to the left
	int32 NumMidPoints   = 0;      // number of arc anchors between start and end

	FAPSArcPathRequest() = default;
};

/**
 * Result of an APS arc path computation in pure FVector form.
 * Plain struct, can be used in async tasks.
 */
struct FAPSArcPathResult
{
	TArray<FVector, TInlineAllocator<64>> Points;

	bool bSuccess = false;
	bool bPartial = false;

	void Reset()
	{
		Points.Reset();
		bSuccess = false;
		bPartial = false;
	}
};

/** Default min distance for enabling arc shaping. */
static constexpr float APS_DefaultMinArcDistance = 300.0f;

/** Converts UE extent (X,Y,Z) into Detour extent (X,Z,Y). */
inline void APS_UnrealExtentToDetour(const FVector& UEExtent, float* OutDetourExtent)
{
	OutDetourExtent[0] = UEExtent.X;
	OutDetourExtent[1] = UEExtent.Z;
	OutDetourExtent[2] = UEExtent.Y;
}
