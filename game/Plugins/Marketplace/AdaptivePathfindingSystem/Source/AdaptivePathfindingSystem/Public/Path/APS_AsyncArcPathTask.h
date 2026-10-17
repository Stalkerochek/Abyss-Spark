/* Copyright (C) 2024 NordVader Inc. - All Rights Reserved
* This plugin is downloadable from the Unreal Engine Marketplace
 */

#pragma once

#include "CoreMinimal.h"
#include "Async/AsyncWork.h"
#include "Path/APS_ArcPathTypes.h"

// Detour types (dtNavMesh, dtQueryFilter, dtStatus, dtPolyRef, etc.)
#include "Detour/DetourNavMeshQuery.h"

/**
 * Asynchronous task that computes an APS arc path using Detour and
 * APS low-level path wrapper (FAPSNavMeshQuery). It does not touch
 * any UObject, so it is safe to run on worker threads.
 */
class FAPSArcPathTask : public FNonAbandonableTask
{
public:
	/** Construct task from request and externally owned result. */
	FAPSArcPathTask(const FAPSArcPathRequest& InRequest, FAPSArcPathResult& InResult)
		: Request(InRequest)
		, Result(InResult)
	{
	}

	/** Synchronous execution entry point for the async task. */
	void DoWork();

	/** Required by FNonAbandonableTask for stats/tracing. */
	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FAPSArcPathTask, STATGROUP_ThreadPoolAsyncTasks);
	}

private:
	// Local copy of the request (POD / Detour pointers only, no UObjects).
	FAPSArcPathRequest Request;

	// Reference to the result that the caller owns (must outlive the task).
	FAPSArcPathResult& Result;
};
