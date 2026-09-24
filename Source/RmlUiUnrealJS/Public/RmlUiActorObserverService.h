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

    /** Creates and registers the editor showcase's parameterized User Interface material. */
    bool AttachMaterialShowcase(class URmlUiWidget* Widget);

    UFUNCTION(BlueprintCallable, Category="RmlUi|Actor Observer")
    FString SetUiMaterialIntensity(int32 Intensity);

    bool IsUiMaterialReady() const { return bUiMaterialReady; }
    int32 GetUiMaterialUpdateCount() const { return UiMaterialUpdateCount; }

private:
    UWorld* ResolveWorld() const;
    UPROPERTY(Transient) TWeakObjectPtr<UObject> WorldContext;
    UPROPERTY(Transient) TWeakObjectPtr<URmlUiWidget> MaterialWidget;
    UPROPERTY(Transient) TObjectPtr<class UMaterialInterface> ShowcaseMaterial;
    uint64 Sequence = 0;
    int32 UiMaterialUpdateCount = 0;
    bool bUiMaterialReady = false;
};
