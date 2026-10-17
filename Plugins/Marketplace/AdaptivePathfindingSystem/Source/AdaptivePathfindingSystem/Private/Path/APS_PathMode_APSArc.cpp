/* Основной дуговый APS-алгоритм. Это по сути твой текущий
 * AAPS_NavigationData::FindPath, вынесенный в отдельный файл/namespace.
 */

#include "Path/APS_PathMode_APSArc.h"

#include "Navigation/APS_NavigationData.h"
#include "Navigation/APS_MoveToInterface.h"

#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"

#include "Debug/APS_NavLogs.h"
#include "Path/APS_PathMode_Engine.h"

// ------------------------ Вспомогательные структуры/функции ------------------------

namespace
{
    struct FAPSArcSegmentResult
    {
        TArray<FNavPathPoint> AB;
        TArray<FNavPathPoint> BE;
        float Score = -1e9f;
    };

    static float PathLength2D(const TArray<FNavPathPoint>& Pts)
    {
        if (Pts.Num() < 2)
        {
            return 0.f;
        }

        float L = 0.f;
        for (int32 i = 1; i < Pts.Num(); ++i)
        {
            L += FVector::Dist2D(Pts[i - 1].Location, Pts[i].Location);
        }
        return L;
    }

    static bool BuildBestArcSegment(
        const FNavAgentProperties& AgentProps,
        const FPathFindingQuery&   BaseQuery,
        const FVector&             P0,
        const FVector&             P2,
        float                      ArcStrength,
        const ARecastNavMesh*      RecastSelf,
        UNavigationSystemV1*       NavSys,
        bool                       bDbg,
        FAPSArcSegmentResult&      OutSegment)
    {
        if (!RecastSelf || !NavSys)
        {
            return false;
        }

        const FVector Fwd2D = (P2 - P0).GetSafeNormal2D();
        const FVector Right = FVector::CrossProduct(Fwd2D, FVector::UpVector).GetSafeNormal();
        const float   Dist2D= FVector::Dist2D(P0, P2);

        const float AmpFrac     = 0.35f;
        const float DesiredSide = (ArcStrength >= 0.f) ? 1.f : -1.f;    
        const float BaseAmp     = AmpFrac * Dist2D * FMath::Clamp(FMath::Abs(ArcStrength), 0.f, 3.f);

        const FVector QueryExtent = RecastSelf->GetDefaultQueryExtent();

        auto Project = [&](const FVector& In, FVector& Out)->bool
        {
            FNavLocation NL;
            const bool bOk = NavSys->ProjectPointToNavigation(
                In,
                NL,
                QueryExtent,
                RecastSelf,
                BaseQuery.QueryFilter);

            if (bOk)
            {
                Out = NL.Location;
            }
            return bOk;
        };

        auto BuildHalf = [&](const FVector& A, const FVector& B, TArray<FNavPathPoint>& OutPts)->bool
        {
            FPathFindingQuery Q = BaseQuery;
            Q.StartLocation = A;
            Q.EndLocation   = B;

            FPathFindingResult R =
                const_cast<ARecastNavMesh*>(RecastSelf)->ARecastNavMesh::FindPath(AgentProps, Q);

            if (!R.IsSuccessful() || !R.Path.IsValid())
            {
                return false;
            }

            OutPts = R.Path->GetPathPoints();
            return OutPts.Num() > 0;
        };

        auto PseudoClearance = [&](const FVector& C)->float
        {
            const float r = FMath::Max(AgentProps.AgentRadius * 1.5f, 40.f);
            int32 Ok = 0;
            const int32 Total = 8;

            for (int32 i = 0; i < Total; ++i)
            {
                const float a = (float)i / (float)Total * 2.f * PI;
                FVector Pt = C + FVector(FMath::Cos(a) * r, FMath::Sin(a) * r, 0.f);
                FVector Proj;
                if (Project(Pt, Proj))
                {
                    ++Ok;
                }
            }

            return (float)Ok / (float)Total;
        };

        struct FCandidate
        {
            FVector               Mid;
            FVector               MidProjected;
            float                 Score = -1e9f;
            TArray<FNavPathPoint> AB;
            TArray<FNavPathPoint> BE;
        };

        FCandidate Best;
        Best.Score = -1e9f;

        const float tVals[] = { 0.40f, 0.50f, 0.60f };
        const float sVals[] = { 1.00f, 0.70f, 0.45f };

        for (float t : tVals)
        {
            const FVector LineAtT = FMath::Lerp(P0, P2, t);

            for (float s : sVals)
            {
                FCandidate C;

                C.Mid = LineAtT + Right * (DesiredSide * BaseAmp * s);

                FVector MidProj;
                bool bMidOk = Project(C.Mid, MidProj);
                if (!bMidOk) bMidOk = Project(LineAtT + Right * (DesiredSide * BaseAmp * s * 0.6f), MidProj);
                if (!bMidOk) bMidOk = Project(LineAtT + Right * (DesiredSide * BaseAmp * s * 0.3f), MidProj);
                if (!bMidOk)
                {
                    continue;
                }

                C.MidProjected = MidProj;

                if (!BuildHalf(P0, MidProj, C.AB))
                {
                    continue;
                }
                if (!BuildHalf(MidProj, P2, C.BE))
                {
                    continue;
                }

                const FVector Prev  = (C.AB.Num() >= 2) ? C.AB[C.AB.Num() - 2].Location : P0;
                const FVector Joint =  C.AB.Last().Location;
                const FVector Next  = (C.BE.Num() >= 2) ? C.BE[1].Location : P2;

                const FVector v1 = (Joint - Prev).GetSafeNormal2D();
                const FVector v2 = (Next  - Joint).GetSafeNormal2D();
                const float   Dot = FVector::DotProduct(v1, v2); 

                if (Dot < -0.8f)
                {
                    continue;
                }

                const FVector BaseDir   = (P2 - P0).GetSafeNormal2D();
                const float   DotAhead1 = FVector::DotProduct(BaseDir, v1);
                const float   DotAhead2 = FVector::DotProduct(BaseDir, v2);

                const float BackPenalty =
                    FMath::Max(0.f, -DotAhead1) +
                    FMath::Max(0.f, -DotAhead2);

                const float SideDot  = FVector::DotProduct(Right, (Joint - LineAtT));
                const float SideSign = (SideDot >= 0.f ? 1.f : -1.f);
                const float WrongSidePenalty = (SideSign == DesiredSide) ? 0.f : 5.f;

                const float Clearance = PseudoClearance(Joint);

                const float AnglePenalty =
                    (float)PI - FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f));

                const float Score =
                    2.0f * Clearance
                    - 1.0f * AnglePenalty
                    - 3.0f * BackPenalty
                    - WrongSidePenalty;

                if (bDbg)
                {
                    UE_LOG(LogAPSNav, Log,
                        TEXT("Mid t=%.2f s=%.2f  clr=%.2f  dot=%.3f  side=%+.0f  Score=%.3f  joint=%s"),
                        t, s, Clearance, Dot, SideSign, Score, *Joint.ToCompactString());
                }

                if (Score > Best.Score)
                {
                    Best = C;
                    Best.Score = Score;
                }
            }
        }

        if (Best.Score < -1e8f)
        {
            return false;
        }

        OutSegment.AB    = MoveTemp(Best.AB);
        OutSegment.BE    = MoveTemp(Best.BE);
        OutSegment.Score = Best.Score;
        return true;
    }
}

// --------------------------- Основной APS дуговый поиск ---------------------------

FPathFindingResult APSPathModeAPSArc::FindPath(const AAPS_NavigationData* NavData,
                                               const FNavAgentProperties& AgentProperties,
                                               const FPathFindingQuery&   Query)
{
    const bool bDbg = CVarAPSNavDebug.GetValueOnAnyThread() != 0;

    if (!NavData)
    {
        UE_LOG(LogAPSNav, Error, TEXT("APSPathModeAPSArc::FindPath: NavData is null"));
        return ENavigationQueryResult::Error;
    }

    const ARecastNavMesh* RecastSelf = Cast<const ARecastNavMesh>(NavData);
    if (!RecastSelf)
    {
        UE_LOG(LogAPSNav, Error, TEXT("APSPathModeAPSArc::FindPath: NavData is not ARecastNavMesh"));
        return ENavigationQueryResult::Error;
    }

    // Prepare path holder we may fill manually
    FPathFindingResult Result(ENavigationQueryResult::Error);
    FNavigationPath*   NavPath     = Query.PathInstanceToFill.Get();
    FNavMeshPath*      NavMeshPath = NavPath ? NavPath->CastPath<FNavMeshPath>() : nullptr;
    if (!NavMeshPath)
    {
        Result.Path  = NavData->CreatePathInstance<FNavMeshPath>(Query);
        NavPath      = Result.Path.Get();
        NavMeshPath  = NavPath ? NavPath->CastPath<FNavMeshPath>() : nullptr;
    }

    const FNavigationQueryFilter* NavFilter = Query.QueryFilter.Get();
    if (!NavMeshPath || !NavFilter)
    {
        UE_LOG(LogAPSNav, Error, TEXT("APSPathModeAPSArc::FindPath: NavMeshPath or NavFilter is null"));
        return ENavigationQueryResult::Error;
    }

    NavMeshPath->ApplyFlags(Query.NavDataFlags);
    const FVector AdjustedEnd = NavFilter->GetAdjustedEndLocation(Query.EndLocation);

    // Actor and debug visualization 
    AActor* Actor = nullptr;
    if (const AController* C = Cast<AController>(Query.Owner.Get()))
    {
        Actor = C->GetPawn();
    }
    else
    {
        Actor = const_cast<AActor*>(Cast<const AActor>(Query.Owner.Get()));
    }

    auto VisualizePts = [&](const TArray<FNavPathPoint>& Pts)
    {
        if (!Actor || Pts.Num() < 2)
            return;
    };

    auto Finish = [&](FPathFindingResult In)->FPathFindingResult
    {
        const TArray<FNavPathPoint>* Src = nullptr;
        if (In.Path.IsValid())
            Src = &In.Path->GetPathPoints();
        else if (NavMeshPath)
            Src = &NavMeshPath->GetPathPoints();

        if (Src)
        {
            VisualizePts(*Src);
        }
        return In;
    };

    // Trivial path
    if ((Query.StartLocation - AdjustedEnd).IsNearlyZero())
    {
        if (bDbg)
        {
            UE_LOG(LogAPSNav, Log, TEXT("Trivial path (Start ~ End)"));
        }

        NavMeshPath->GetPathPoints().Reset();
        NavMeshPath->GetPathPoints().Add(FNavPathPoint(AdjustedEnd));
        NavPath->MarkReady();
        Result.Result = ENavigationQueryResult::Success;
        return Finish(Result);
    }

    // Interface-controlled parameters
    float ArcStrength  = 0.f;
    int32 NumMidPoints = -1;

    if (Actor && Actor->GetClass()->ImplementsInterface(UAPS_MoveToInterface::StaticClass()))
    {
        // If user provides a full path — just use it.
        TArray<FVector> Tmp;
        bool bUseCustom = false;
        bool bGen       = false;

        IAPS_MoveToInterface::Execute_FindCustomPath(
            Actor,
            Query.StartLocation,
            AdjustedEnd,
            Tmp,
            ArcStrength,
            Query.Owner->GetWorld(),
            bUseCustom,
            bGen);

        if (bDbg)
        {
            UE_LOG(LogAPSNav, Log, TEXT("Iface: bUseCustom=%d bGen=%d Arc=%.3f"),
                bUseCustom ? 1 : 0,
                bGen ? 1 : 0,
                ArcStrength);
        }

        if (bUseCustom && bGen)
        {
            NavMeshPath->GetPathPoints().Reset();
            for (const FVector& P : Tmp)
            {
                NavMeshPath->GetPathPoints().Add(FNavPathPoint(P));
            }
            NavPath->MarkReady();
            Result.Result = ENavigationQueryResult::Success;
            return Finish(Result);
        }

        if (!bUseCustom)
        {
            EAPSArcMidpointMode DummyMode = EAPSArcMidpointMode::SmoothCurve;
            float DummyLaneOffset = 0.0f;
            
            IAPS_MoveToInterface::Execute_GetMoveToParameters(
                Actor,
                ArcStrength,
                NumMidPoints,
                DummyMode,
                DummyLaneOffset);

            if (bDbg)
            {
                UE_LOG(LogAPSNav, Log, TEXT("Interface params -> Arc=%.3f MidPoints=%d Mode=%s LaneOffset=%.3f"),
                    ArcStrength,
                    NumMidPoints,
                    (DummyMode == EAPSArcMidpointMode::SmoothCurve ? TEXT("Smooth") : TEXT("Wavy")),
                    DummyLaneOffset);
            }
        }
    }

    // No arc => fallback на чистый движковый путь
    if (FMath::IsNearlyZero(ArcStrength, 1e-3f))
    {
        return Finish(APSPathModeEngine::FindPath(NavData, AgentProperties, Query));
    }

    // Нормализуем количество внутренних точек:
    if (NumMidPoints < 1)
    {
        NumMidPoints = 1;
    }
    NumMidPoints = FMath::Clamp(NumMidPoints, 1, 8);

    const FVector P0 = Query.StartLocation;
    const FVector P2 = AdjustedEnd;

    UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(Query.Owner->GetWorld());
    if (!NavSys)
    {
        UE_LOG(LogAPSNav, Error, TEXT("APSPathModeAPSArc::FindPath: NavSys not found"));
        return Finish(FPathFindingResult(ENavigationQueryResult::Error));
    }

    // =========================
    //  MULTI-SEGMENT MODE
    // =========================
    if (NumMidPoints > 1)
    {
        TArray<FNavPathPoint> Merged;
        Merged.Add(FNavPathPoint(P0));
        Merged.Add(FNavPathPoint(P2));

        const int32 Steps = NumMidPoints;

        for (int32 Step = 0; Step < Steps; ++Step)
        {
            if (Merged.Num() < 2)
                break;

            int32 BestEdgeStart = INDEX_NONE;
            float BestDistSq    = -1.f;

            for (int32 i = 0; i < Merged.Num() - 1; ++i)
            {
                const float D = FVector::DistSquared2D(Merged[i].Location, Merged[i + 1].Location);
                if (D > BestDistSq)
                {
                    BestDistSq    = D;
                    BestEdgeStart = i;
                }
            }

            if (BestEdgeStart == INDEX_NONE || BestDistSq <= KINDA_SMALL_NUMBER)
                break;

            const FVector A = Merged[BestEdgeStart    ].Location;
            const FVector B = Merged[BestEdgeStart + 1].Location;

            const float Falloff = 0.4f;
            const float LocalArcStrength =
                (Step == 0) ? ArcStrength
                            : ArcStrength * FMath::Pow(Falloff, (float)Step);

            FAPSArcSegmentResult Segment;
            if (!BuildBestArcSegment(
                    AgentProperties,
                    Query,
                    A, B,
                    LocalArcStrength,
                    RecastSelf,
                    NavSys,
                    bDbg,
                    Segment))
            {
                if (bDbg)
                {
                    UE_LOG(LogAPSNav, Log,
                        TEXT("Multi-seg step %d: BuildBestArcSegment failed"), Step);
                }
                break;
            }

            TArray<FNavPathPoint> SubMerged;
            SubMerged.Reserve(Segment.AB.Num() + Segment.BE.Num());
            SubMerged.Append(Segment.AB);

            if (SubMerged.Num() > 0 && Segment.BE.Num() > 0 &&
                SubMerged.Last().Location.Equals(Segment.BE[0].Location, 1.0f))
            {
                Segment.BE.RemoveAt(0);
            }
            SubMerged.Append(Segment.BE);

            if (SubMerged.Num() < 2)
                break;

            const float BaseLen    = FVector::Dist2D(A, B);
            const float NewLen     = PathLength2D(SubMerged);
            const float MaxStretch = (Step == 0) ? 0.6f : 0.25f;

            if (BaseLen > KINDA_SMALL_NUMBER &&
                NewLen > BaseLen * (1.f + MaxStretch))
            {
                if (bDbg)
                {
                    UE_LOG(LogAPSNav, Log,
                        TEXT("Multi-seg step %d: rejected, stretch %.2f > %.2f"),
                        Step, NewLen / BaseLen - 1.f, MaxStretch);
                }
                continue;
            }

            TArray<FNavPathPoint> NewMerged;
            NewMerged.Reserve(Merged.Num() - 2 + SubMerged.Num());

            for (int32 i = 0; i <= BestEdgeStart; ++i)
            {
                NewMerged.Add(Merged[i]);
            }

            for (int32 i = 1; i < SubMerged.Num(); ++i)
            {
                NewMerged.Add(SubMerged[i]);
            }

            for (int32 i = BestEdgeStart + 2; i < Merged.Num(); ++i)
            {
                NewMerged.Add(Merged[i]);
            }

            Merged = MoveTemp(NewMerged);
        }

        for (int32 i = 1; i < Merged.Num() - 1; )
        {
            const float D1 = FVector::Dist2D(Merged[i - 1].Location, Merged[i].Location);
            const float D2 = FVector::Dist2D(Merged[i].Location,     Merged[i + 1].Location);
            if (D1 < 5.f && D2 < 5.f)
            {
                Merged.RemoveAt(i);
            }
            else
            {
                ++i;
            }
        }

        NavMeshPath->GetPathPoints() = MoveTemp(Merged);
        NavPath->MarkReady();
        Result.Result = ENavigationQueryResult::Success;
        return Finish(Result);
    }

    // =========================
    //  SINGLE-ARC MODE (NumMidPoints == 1)
    // =========================

    FAPSArcSegmentResult Segment;
    if (!BuildBestArcSegment(
            AgentProperties,
            Query,
            P0,
            P2,
            ArcStrength,
            RecastSelf,
            NavSys,
            bDbg,
            Segment))
    {
        if (bDbg)
        {
            UE_LOG(LogAPSNav, Warning, TEXT("No valid Mid candidate -> base path"));
        }

        return Finish(APSPathModeEngine::FindPath(NavData, AgentProperties, Query));
    }

    // Raycast helper
    AController* Querier = nullptr;
    if (const AController* AsCtrl = Cast<AController>(Query.Owner.Get()))
    {
        Querier = const_cast<AController*>(AsCtrl);
    }
    else if (const AActor* AsActor = Cast<AActor>(Query.Owner.Get()))
    {
        Querier = AsActor->GetInstigatorController();
    }

    TSubclassOf<UNavigationQueryFilter> RaycastFilter = nullptr;

    auto CanGoStraight = [&](const FVector& A, const FVector& C)->bool
    {
        FVector Hit;
        const bool bHit = NavSys->NavigationRaycast(
            Query.Owner->GetWorld(),
            A,
            C,
            Hit,
            RaycastFilter,
            Querier);
        return !bHit;
    };

    TArray<FNavPathPoint> Merged;
    Merged.Reserve(Segment.AB.Num() + Segment.BE.Num());
    Merged.Append(Segment.AB);

    int32 JoinIdx = Merged.Num() - 1;
    if (Merged.Num() > 0 && Segment.BE.Num() > 0 &&
        Merged.Last().Location.Equals(Segment.BE[0].Location, 1.0f))
    {
        Segment.BE.RemoveAt(0);
    }
    Merged.Append(Segment.BE);

    for (int32 i = 0; i < Merged.Num() - 2; ++i)
    {
        int32 BestJ = i + 1;
        for (int32 j = i + 2; j < Merged.Num(); ++j)
        {
            if (i < JoinIdx && j > JoinIdx)
                break;

            const FVector A = Merged[i].Location;
            const FVector C = Merged[j].Location;

            if (CanGoStraight(A, C))
            {
                BestJ = j;
            }
            else
            {
                break;
            }
        }

        if (BestJ > i + 1)
        {
            const int32 RemoveCount = BestJ - (i + 1);
            Merged.RemoveAt(i + 1, RemoveCount);

            if (i < JoinIdx)
            {
                const int32 Removed = RemoveCount;
                JoinIdx -= FMath::Min(Removed, JoinIdx - (i + 1));
            }
        }
    }

    for (int32 i = 1; i < Merged.Num() - 1; )
    {
        if (i == JoinIdx)
        {
            ++i;
            continue;
        }

        const FVector A = Merged[i - 1].Location;
        const FVector B = Merged[i    ].Location;
        const FVector C = Merged[i + 1].Location;

        const FVector V1 = (B - A).GetSafeNormal2D();
        const FVector V2 = (C - B).GetSafeNormal2D();
        const float   Dot = FVector::DotProduct(V1, V2);

        if (Dot > 0.99f)
        {
            Merged.RemoveAt(i);
            if (i < JoinIdx)
            {
                --JoinIdx;
            }
        }
        else
        {
            ++i;
        }
    }

    NavMeshPath->GetPathPoints() = MoveTemp(Merged);
    NavPath->MarkReady();
    Result.Result = ENavigationQueryResult::Success;
    return Finish(Result);
}
