#include "RmlUiActorObserverService.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "RmlUiWidget.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#endif

namespace
{
FString WriteJson(const TSharedRef<FJsonObject>& Object)
{
    FString Json;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    return Json;
}

FString ActorDisplayName(const AActor* Actor)
{
#if WITH_EDITOR
    return Actor->GetActorLabel();
#else
    return Actor->GetName();
#endif
}

FString CompactVector(const FVector& Value)
{
    return FString::Printf(TEXT("X %.2f  Y %.2f  Z %.2f"), Value.X, Value.Y, Value.Z);
}

FString CompactRotation(const FRotator& Value)
{
    return FString::Printf(TEXT("P %.2f  Y %.2f  R %.2f"), Value.Pitch, Value.Yaw, Value.Roll);
}
}

void URmlUiActorObserverService::Initialize(UObject* InWorldContext)
{
    WorldContext = InWorldContext;
}

bool URmlUiActorObserverService::AttachMaterialShowcase(URmlUiWidget* Widget)
{
    MaterialWidget = Widget;
    bUiMaterialReady = false;
#if WITH_EDITOR
    if (!Widget) return false;
    UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    Material->MaterialDomain = MD_UI;
    Material->BlendMode = BLEND_Translucent;
    UMaterialExpressionVectorParameter* Tint = NewObject<UMaterialExpressionVectorParameter>(Material);
    Tint->ParameterName = TEXT("Tint");
    Tint->DefaultValue = FLinearColor(0.03f, 0.55f, 0.42f, 1.0f);
    Material->GetExpressionCollection().AddExpression(Tint);
    UMaterialEditorOnlyData* EditorOnly = Material->GetEditorOnlyData();
    EditorOnly->EmissiveColor.Expression = Tint;
    EditorOnly->Opacity.UseConstant = true;
    EditorOnly->Opacity.Constant = 1.0f;
    Material->PostEditChange();
    ShowcaseMaterial = Material;
    bUiMaterialReady = Widget->RegisterMaterial(TEXT("showcase.energy"), Material);
    if (bUiMaterialReady) SetUiMaterialIntensity(72);
#endif
    return bUiMaterialReady;
}

FString URmlUiActorObserverService::SetUiMaterialIntensity(int32 Intensity)
{
    const int32 Clamped = FMath::Clamp(Intensity, 0, 100);
    if (!bUiMaterialReady || !MaterialWidget.IsValid()) return TEXT("UE UI Material unavailable");
    const float Alpha = static_cast<float>(Clamped) / 100.0f;
    const FLinearColor Low(0.025f, 0.10f, 0.14f, 1.0f);
    const FLinearColor High(0.04f, 0.82f, 0.55f, 1.0f);
    if (!MaterialWidget->SetMaterialVector(TEXT("showcase.energy"), TEXT("Tint"), FMath::Lerp(Low, High, Alpha)))
        return TEXT("UE MID update failed");
    ++UiMaterialUpdateCount;
    return FString::Printf(TEXT("UE MID / %d%%"), Clamped);
}

UWorld* URmlUiActorObserverService::ResolveWorld() const
{
    if (WorldContext.IsValid())
    {
        return WorldContext->GetWorld();
    }
    return GetWorld();
}

FString URmlUiActorObserverService::GetActorSnapshot()
{
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("sequence"), static_cast<double>(++Sequence));
    UWorld* World = ResolveWorld();
    Root->SetStringField(TEXT("level"), World ? World->GetMapName() : TEXT(""));

    struct FRow
    {
        FString Name;
        TSharedPtr<FJsonValue> Json;
    };
    TArray<FRow> Rows;
    if (World)
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            const AActor* Actor = *It;
            const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
            const FString Name = ActorDisplayName(Actor);
            Item->SetStringField(TEXT("path"), Actor->GetPathName());
            Item->SetStringField(TEXT("name"), Name);
            Item->SetStringField(TEXT("type"), Actor->GetClass()->GetName());
            Item->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetOuter()->GetName() : TEXT(""));
            Item->SetBoolField(TEXT("hidden"), Actor->IsHidden());
            Item->SetBoolField(TEXT("ticking"), Actor->IsActorTickEnabled());
            Rows.Add({Name, MakeShared<FJsonValueObject>(Item)});
        }
    }
    Rows.Sort([](const FRow& Left, const FRow& Right) { return Left.Name < Right.Name; });
    TArray<TSharedPtr<FJsonValue>> Actors;
    Actors.Reserve(Rows.Num());
    for (FRow& Row : Rows) Actors.Add(MoveTemp(Row.Json));
    Root->SetArrayField(TEXT("actors"), Actors);
    return WriteJson(Root);
}

FString URmlUiActorObserverService::GetActorDetails(const FString& ActorPath)
{
    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    AActor* Match = nullptr;
    if (UWorld* World = ResolveWorld())
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->GetPathName() == ActorPath)
            {
                Match = *It;
                break;
            }
        }
    }
    Root->SetBoolField(TEXT("found"), Match != nullptr);
    if (!Match) return WriteJson(Root);

    Root->SetStringField(TEXT("path"), Match->GetPathName());
    Root->SetStringField(TEXT("name"), ActorDisplayName(Match));
    Root->SetStringField(TEXT("type"), Match->GetClass()->GetName());
    Root->SetStringField(TEXT("level"), Match->GetLevel() ? Match->GetLevel()->GetOuter()->GetName() : TEXT(""));
    const TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
    Transform->SetStringField(TEXT("location"), CompactVector(Match->GetActorLocation()));
    Transform->SetStringField(TEXT("rotation"), CompactRotation(Match->GetActorRotation()));
    Transform->SetStringField(TEXT("scale"), CompactVector(Match->GetActorScale3D()));
    Root->SetObjectField(TEXT("transform"), Transform);

    struct FPropertyRow
    {
        FString SortKey;
        TSharedPtr<FJsonValue> Json;
    };
    TArray<FPropertyRow> Rows;
    for (TFieldIterator<FProperty> It(Match->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) ||
            Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
        {
            continue;
        }
        FString Value;
        Property->ExportText_InContainer(0, Value, Match, Match, Match, PPF_None);
        Value.ReplaceInline(TEXT("\r"), TEXT(" "));
        Value.ReplaceInline(TEXT("\n"), TEXT(" "));
        if (Value.Len() > 512) Value = Value.Left(509) + TEXT("...");
        FString Category;
#if WITH_EDITORONLY_DATA
        Category = Property->GetMetaData(TEXT("Category"));
#endif
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("category"), Category.IsEmpty() ? TEXT("Actor") : Category);
        Item->SetStringField(TEXT("name"), Property->GetAuthoredName());
        Item->SetStringField(TEXT("type"), Property->GetCPPType());
        Item->SetStringField(TEXT("value"), Value);
        Rows.Add({Category + TEXT("/") + Property->GetAuthoredName(), MakeShared<FJsonValueObject>(Item)});
    }
    Rows.Sort([](const FPropertyRow& Left, const FPropertyRow& Right) { return Left.SortKey < Right.SortKey; });
    TArray<TSharedPtr<FJsonValue>> Properties;
    Properties.Reserve(Rows.Num());
    for (FPropertyRow& Row : Rows) Properties.Add(MoveTemp(Row.Json));
    Root->SetArrayField(TEXT("properties"), Properties);
    return WriteJson(Root);
}
