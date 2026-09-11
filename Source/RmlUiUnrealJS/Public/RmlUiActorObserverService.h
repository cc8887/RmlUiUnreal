#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RmlUiActorObserverService.generated.h"

UCLASS(BlueprintType)
class RMLUIUNREALJS_API URmlUiActorObserverService : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="RmlUi|Actor Observer")
    void Initialize(UObject* InWorldContext);

    UFUNCTION(BlueprintCallable, Category="RmlUi|Actor Observer")
    FString GetActorSnapshot();

    UFUNCTION(BlueprintCallable, Category="RmlUi|Actor Observer")
    FString GetActorDetails(const FString& ActorPath);

private:
    UWorld* ResolveWorld() const;
    UPROPERTY(Transient) TWeakObjectPtr<UObject> WorldContext;
    uint64 Sequence = 0;
};
