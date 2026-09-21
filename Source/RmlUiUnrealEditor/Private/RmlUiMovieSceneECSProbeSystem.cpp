#include "RmlUiMovieSceneECSProbeSystem.h"

#include "Async/TaskGraphInterfaces.h"
#include "EntitySystem/BuiltInComponentTypes.h"
#include "EntitySystem/MovieSceneComponentRegistry.h"
#include "EntitySystem/MovieSceneEntityBuilder.h"
#include "EntitySystem/MovieSceneEntityFactoryTemplates.h"
#include "EntitySystem/MovieSceneEntitySystemGraphs.h"
#include "EntitySystem/MovieSceneEntitySystemLinker.h"
#include "EntitySystem/MovieSceneEntitySystemTask.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#endif

namespace RmlUiMovieSceneECSProbe
{
using namespace UE::MovieScene;

struct FTrack
{
    float StartValue = 0.0f;
    float EndValue = 0.0f;
    float Progress = 0.0f;
};

struct FComponentTypes
{
    TComponentTypeID<FTrack> Track;
    TComponentTypeID<float> Result;
    TComponentTypeID<float> ComposedResult;

    static FComponentTypes& Get()
    {
        static FComponentTypes Types;
        return Types;
    }

    void EnsureRegistered()
    {
        if (!Track)
        {
            FComponentRegistry* Registry = UMovieSceneEntitySystemLinker::GetComponents();
            Track = Registry->NewComponentType<FTrack>(TEXT("RmlUi animation probe track"));
            Result = Registry->NewComponentType<float>(TEXT("RmlUi animation probe result"));
            ComposedResult = Registry->NewComponentType<float>(TEXT("RmlUi animation probe composed result"));
        }
    }
};

struct FEvaluateTrack
{
    static void ForEachEntity(const FTrack& Track, float& Result)
    {
        Result = FMath::Lerp(Track.StartValue, Track.EndValue, Track.Progress);
    }
};

struct FComposeTrack
{
    static void ForEachEntity(float Result, float& ComposedResult)
    {
        ComposedResult = Result + 5.0f;
    }
};
}

URmlUiMovieSceneECSProbeSystem::URmlUiMovieSceneECSProbeSystem(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    Phase = UE::MovieScene::ESystemPhase::Evaluation;
}

void URmlUiMovieSceneECSProbeSystem::OnRun(
    FSystemTaskPrerequisites& Prerequisites,
    FSystemSubsequentTasks& Subsequents)
{
    using namespace UE::MovieScene;
    using namespace RmlUiMovieSceneECSProbe;

    const FComponentTypes& Components = FComponentTypes::Get();
    FEntityTaskBuilder()
        .Read(Components.Track)
        .Write(Components.Result)
        .Dispatch_PerEntity<FEvaluateTrack>(&Linker->EntityManager, Prerequisites, &Subsequents);
}

URmlUiMovieSceneECSProbeComposeSystem::URmlUiMovieSceneECSProbeComposeSystem(
    const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    Phase = UE::MovieScene::ESystemPhase::Evaluation;
}

void URmlUiMovieSceneECSProbeComposeSystem::OnRun(
    FSystemTaskPrerequisites& Prerequisites,
    FSystemSubsequentTasks& Subsequents)
{
    using namespace UE::MovieScene;
    using namespace RmlUiMovieSceneECSProbe;

    const FComponentTypes& Components = FComponentTypes::Get();
    FEntityTaskBuilder()
        .Read(Components.Result)
        .Write(Components.ComposedResult)
        .Dispatch_PerEntity<FComposeTrack>(&Linker->EntityManager, Prerequisites, &Subsequents);
}

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FRmlUiMovieSceneECSCompatibilityTest,
    "RmlUi.MovieScene.ECSCompatibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRmlUiMovieSceneECSCompatibilityTest::RunTest(const FString& Parameters)
{
    using namespace UE::MovieScene;
    using namespace RmlUiMovieSceneECSProbe;

    FComponentTypes& Components = FComponentTypes::Get();
    Components.EnsureRegistered();

    static const EEntitySystemLinkerRole LinkerRole = RegisterCustomEntitySystemLinkerRole();
    TStrongObjectPtr<UMovieSceneEntitySystemLinker> Linker(
        UMovieSceneEntitySystemLinker::CreateLinker(GetTransientPackage(), LinkerRole));
    TestNotNull(TEXT("custom linker"), Linker.Get());
    if (!Linker)
    {
        return false;
    }

    TestEqual(TEXT("custom linker role"), Linker->GetLinkerRole(), LinkerRole);
    static const bool bDependenciesDefined = [&Components]()
    {
        UMovieSceneEntitySystem::DefineComponentProducer(
            URmlUiMovieSceneECSProbeSystem::StaticClass(), Components.Result);
        UMovieSceneEntitySystem::DefineComponentConsumer(
            URmlUiMovieSceneECSProbeComposeSystem::StaticClass(), Components.Result);
        return true;
    }();
    TestTrue(TEXT("system dependencies registered"), bDependenciesDefined);

    URmlUiMovieSceneECSProbeComposeSystem* ComposeSystem =
        Linker->LinkSystem<URmlUiMovieSceneECSProbeComposeSystem>();
    URmlUiMovieSceneECSProbeSystem* SampleSystem = Linker->LinkSystem<URmlUiMovieSceneECSProbeSystem>();
    TestNotNull(TEXT("custom sample system"), SampleSystem);
    TestNotNull(TEXT("custom compose system"), ComposeSystem);
    if (!SampleSystem || !ComposeSystem)
    {
        return false;
    }

    constexpr int32 EntityCount = 1024;
    TArray<FMovieSceneEntityID> Entities;
    Entities.Reserve(EntityCount);
    for (int32 Index = 0; Index < EntityCount; ++Index)
    {
        const float Progress = static_cast<float>(Index) / static_cast<float>(EntityCount - 1);
        Entities.Add(FEntityBuilder()
            .Add(Components.Track, FTrack{10.0f, 42.0f, Progress})
            .Add(Components.Result, 0.0f)
            .Add(Components.ComposedResult, 0.0f)
            .CreateEntity(&Linker->EntityManager));
    }

    Linker->EntityManager.UpdateThreadingModel();
    Linker->EntityManager.LockDown();
    FGraphEventArray Tasks;
    Linker->SystemGraph.ExecutePhase(ESystemPhase::Evaluation, Linker.Get(), Tasks);
    if (!Tasks.IsEmpty())
    {
        FTaskGraphInterface::Get().WaitUntilTasksComplete(Tasks, ENamedThreads::GameThread);
    }
    Linker->EntityManager.ReleaseLockDown();

    TestEqual(TEXT("first result"),
        Linker->EntityManager.ReadComponentChecked(Entities[0], Components.Result), 10.0f);
    TestEqual(TEXT("last result"),
        Linker->EntityManager.ReadComponentChecked(Entities.Last(), Components.Result), 42.0f);
    const int32 MiddleIndex = EntityCount / 2;
    const float ExpectedMiddle = FMath::Lerp(
        10.0f, 42.0f, static_cast<float>(MiddleIndex) / static_cast<float>(EntityCount - 1));
    TestTrue(TEXT("middle result"), FMath::IsNearlyEqual(
        Linker->EntityManager.ReadComponentChecked(Entities[MiddleIndex], Components.Result), ExpectedMiddle));
    TestEqual(TEXT("first composed result"),
        Linker->EntityManager.ReadComponentChecked(Entities[0], Components.ComposedResult), 15.0f);
    TestEqual(TEXT("last composed result"),
        Linker->EntityManager.ReadComponentChecked(Entities.Last(), Components.ComposedResult), 47.0f);

    for (FMovieSceneEntityID Entity : Entities)
    {
        Linker->EntityManager.AddComponent(Entity, FBuiltInComponentTypes::Get()->Tags.NeedsUnlink);
        Linker->EntityManager.FreeEntity(Entity);
        TestFalse(TEXT("probe entity released"), Linker->EntityManager.IsAllocated(Entity));
    }
    Linker->Reset();
    return true;
}

#endif
