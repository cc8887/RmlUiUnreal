#pragma once

#include "EntitySystem/MovieSceneEntitySystem.h"

#include "RmlUiMovieSceneECSProbeSystem.generated.h"

UCLASS()
class URmlUiMovieSceneECSProbeSystem final : public UMovieSceneEntitySystem
{
    GENERATED_BODY()

public:
    URmlUiMovieSceneECSProbeSystem(const FObjectInitializer& ObjectInitializer);

private:
    virtual void OnRun(FSystemTaskPrerequisites& Prerequisites, FSystemSubsequentTasks& Subsequents) override;
};

UCLASS()
class URmlUiMovieSceneECSProbeComposeSystem final : public UMovieSceneEntitySystem
{
    GENERATED_BODY()

public:
    URmlUiMovieSceneECSProbeComposeSystem(const FObjectInitializer& ObjectInitializer);

private:
    virtual void OnRun(FSystemTaskPrerequisites& Prerequisites, FSystemSubsequentTasks& Subsequents) override;
};
