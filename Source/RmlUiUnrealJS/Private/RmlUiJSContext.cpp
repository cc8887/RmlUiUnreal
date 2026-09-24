#include "RmlUiJSContext.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "JsEnv.h"
#include "JSModuleLoader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "RmlUiBridge.h"
#include "RmlUiAnimationRuntime.h"
#include "RmlUiPerformance.h"
#include "RmlUiUnrealModule.h"
#include "Serialization/JsonSerializer.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace {
bool TryParseOpacity(const FString& Source, float& OutValue)
{
    const FString Text = Source.TrimStartAndEnd();
    if (!Text.IsNumeric()) return false;
    OutValue = FCString::Atof(*Text);
    return FMath::IsFinite(OutValue) && OutValue >= 0.0f && OutValue <= 1.0f;
}

bool TryParseColor(const FString& Source, FRmlUiColor& OutValue)
{
    FString Text = Source.TrimStartAndEnd().ToLower();
    if (Text == TEXT("transparent")) { OutValue = {0.f, 0.f, 0.f, 0.f}; return true; }
    if (Text.StartsWith(TEXT("#")))
    {
        FString Hex = Text.Mid(1);
        if (Hex.Len() == 3 || Hex.Len() == 4)
        {
            FString Expanded;
            for (TCHAR Digit : Hex) { Expanded.AppendChar(Digit); Expanded.AppendChar(Digit); }
            Hex = MoveTemp(Expanded);
        }
        if (Hex.Len() != 6 && Hex.Len() != 8) return false;
        const FColor Color = FColor::FromHex(Hex);
        OutValue = {Color.R / 255.f, Color.G / 255.f, Color.B / 255.f, Color.A / 255.f};
        return true;
    }
    const bool bRgba = Text.StartsWith(TEXT("rgba("));
    if (!bRgba && !Text.StartsWith(TEXT("rgb("))) return false;
    if (!Text.EndsWith(TEXT(")"))) return false;
    TArray<FString> Parts;
    Text.Mid(bRgba ? 5 : 4, Text.Len() - (bRgba ? 6 : 5)).ParseIntoArray(Parts, TEXT(","), true);
    if (Parts.Num() != (bRgba ? 4 : 3)) return false;
    float Components[4] = {0.f, 0.f, 0.f, 1.f};
    for (int32 Index = 0; Index < 3; ++Index)
    {
        Parts[Index].TrimStartAndEndInline();
        if (!Parts[Index].IsNumeric()) return false;
        Components[Index] = FCString::Atof(*Parts[Index]) / 255.f;
    }
    if (bRgba)
    {
        Parts[3].TrimStartAndEndInline();
        if (!Parts[3].IsNumeric()) return false;
        Components[3] = FCString::Atof(*Parts[3]);
    }
    for (float Component : Components)
        if (!FMath::IsFinite(Component) || Component < 0.f || Component > 1.f) return false;
    OutValue = {Components[0], Components[1], Components[2], Components[3]};
    return true;
}

bool TryParseAnimatedProperty(const FString& Source, ERmlUiAnimatedProperty& OutProperty)
{
    if (Source.Equals(TEXT("opacity"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::Opacity;
    else if (Source.Equals(TEXT("transform"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::Transform2D;
    else if (Source.Equals(TEXT("left"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::LeftPx;
    else if (Source.Equals(TEXT("top"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::TopPx;
    else if (Source.Equals(TEXT("right"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::RightPx;
    else if (Source.Equals(TEXT("bottom"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::BottomPx;
    else if (Source.Equals(TEXT("width"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::WidthPx;
    else if (Source.Equals(TEXT("height"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::HeightPx;
    else if (Source.Equals(TEXT("visibility"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::Visibility;
    else if (Source.Equals(TEXT("color"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::Color;
    else if (Source.Equals(TEXT("background-color"), ESearchCase::IgnoreCase) || Source.Equals(TEXT("backgroundColor"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::BackgroundColor;
    else if (Source.Equals(TEXT("border-color"), ESearchCase::IgnoreCase) || Source.Equals(TEXT("borderColor"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::BorderColor;
    else if (Source.Equals(TEXT("image-color"), ESearchCase::IgnoreCase) || Source.Equals(TEXT("imageColor"), ESearchCase::IgnoreCase)) OutProperty = ERmlUiAnimatedProperty::ImageColor;
    else return false;
    return true;
}

bool IsScalarAnimatedProperty(ERmlUiAnimatedProperty Property)
{
    return Property == ERmlUiAnimatedProperty::Opacity ||
        Property == ERmlUiAnimatedProperty::Visibility ||
        (Property >= ERmlUiAnimatedProperty::LeftPx &&
            Property <= ERmlUiAnimatedProperty::HeightPx);
}

bool IsNonNegativeLayoutProperty(ERmlUiAnimatedProperty Property)
{
    return Property == ERmlUiAnimatedProperty::WidthPx ||
        Property == ERmlUiAnimatedProperty::HeightPx;
}

enum ETransformPrimitive : uint8 { TransformNone = 0, TransformTranslate = 1, TransformScale = 2, TransformRotate = 4,
    TransformSkewX = 8, TransformSkewY = 16 };

bool TryParseNumberWithSuffix(const FString& Source, const TCHAR* Suffix, float& OutValue)
{
    FString Text = Source.TrimStartAndEnd();
    const FString Unit(Suffix);
    if (!Unit.IsEmpty())
    {
        if (!Text.EndsWith(Unit, ESearchCase::IgnoreCase)) return false;
        Text.LeftChopInline(Unit.Len());
        Text.TrimStartAndEndInline();
    }
    if (!Text.IsNumeric()) return false;
    OutValue = FCString::Atof(*Text);
    return FMath::IsFinite(OutValue);
}

bool TryParseTransform2D(const FString& Source, FRmlUiTransform2D& OutValue, uint8& OutPrimitives)
{
    const FString Text = Source.TrimStartAndEnd();
    OutValue = {};
    OutPrimitives = TransformNone;
    if (Text.Equals(TEXT("none"), ESearchCase::IgnoreCase))
        return true;
    int32 Cursor = 0;
    while (Cursor < Text.Len())
    {
        while (Cursor < Text.Len() && FChar::IsWhitespace(Text[Cursor])) ++Cursor;
        if (Cursor >= Text.Len()) break;
        const int32 Open = Text.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
        const int32 Close = Open == INDEX_NONE ? INDEX_NONE :
            Text.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
        const int32 NestedOpen = Open == INDEX_NONE ? INDEX_NONE :
            Text.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
        if (Open <= Cursor || Close == INDEX_NONE || (NestedOpen != INDEX_NONE && NestedOpen < Close))
            return false;
        const FString Name = Text.Mid(Cursor, Open - Cursor).TrimStartAndEnd().ToLower();
        const FString Arguments = Text.Mid(Open + 1, Close - Open - 1);
        TArray<FString> Parts;
        Arguments.ParseIntoArray(Parts, TEXT(","), false);
        uint8 Primitive = TransformNone;
        if (Name == TEXT("scale") && (Parts.Num() == 1 || Parts.Num() == 2))
        {
            float X = 0.0f, Y = 0.0f;
            if (!TryParseNumberWithSuffix(Parts[0], TEXT(""), X)) return false;
            if (Parts.Num() == 2) { if (!TryParseNumberWithSuffix(Parts[1], TEXT(""), Y)) return false; }
            else Y = X;
            OutValue.ScaleX = X; OutValue.ScaleY = Y; Primitive = TransformScale;
        }
        else if (Name == TEXT("translate") && Parts.Num() == 2)
        {
            if (!TryParseNumberWithSuffix(Parts[0], TEXT("px"), OutValue.TranslationX) ||
                !TryParseNumberWithSuffix(Parts[1], TEXT("px"), OutValue.TranslationY)) return false;
            Primitive = TransformTranslate;
        }
        else if (Name == TEXT("rotate") && Parts.Num() == 1)
        {
            if (!TryParseNumberWithSuffix(Parts[0], TEXT("deg"), OutValue.RotationDegrees)) return false;
            Primitive = TransformRotate;
        }
        else if ((Name == TEXT("skew") || Name == TEXT("skewx") || Name == TEXT("skewy")) &&
            (Parts.Num() == 1 || (Name == TEXT("skew") && Parts.Num() == 2)))
        {
            if (Name != TEXT("skewy") && !TryParseNumberWithSuffix(Parts[0], TEXT("deg"), OutValue.SkewXDegrees)) return false;
            if (Name == TEXT("skewy")) {
                if (!TryParseNumberWithSuffix(Parts[0], TEXT("deg"), OutValue.SkewYDegrees)) return false;
                Primitive = TransformSkewY;
            }
            else {
                if (Parts.Num() == 2 && !TryParseNumberWithSuffix(Parts[1], TEXT("deg"), OutValue.SkewYDegrees)) return false;
                Primitive = Parts.Num() == 2 ? TransformSkewX | TransformSkewY : TransformSkewX;
            }
        }
        else if (Name == TEXT("matrix") && Parts.Num() == 6)
        {
            float A, B, C, D;
            if (!TryParseNumberWithSuffix(Parts[0], TEXT(""), A) || !TryParseNumberWithSuffix(Parts[1], TEXT(""), B) ||
                !TryParseNumberWithSuffix(Parts[2], TEXT(""), C) || !TryParseNumberWithSuffix(Parts[3], TEXT(""), D) ||
                !TryParseNumberWithSuffix(Parts[4], TEXT(""), OutValue.TranslationX) ||
                !TryParseNumberWithSuffix(Parts[5], TEXT(""), OutValue.TranslationY)) return false;
            const float ScaleX = FMath::Sqrt(A * A + B * B);
            if (ScaleX <= UE_SMALL_NUMBER) return false;
            OutValue.ScaleX = ScaleX;
            OutValue.ScaleY = (A * D - B * C) / ScaleX;
            OutValue.RotationDegrees = FMath::RadiansToDegrees(FMath::Atan2(B, A));
            OutValue.SkewXDegrees = FMath::RadiansToDegrees(FMath::Atan((A * C + B * D) / (ScaleX * ScaleX)));
            Primitive = TransformTranslate | TransformScale | TransformRotate | TransformSkewX | TransformSkewY;
        }
        else return false;
        if (OutPrimitives & Primitive) return false;
        OutPrimitives |= Primitive;
        Cursor = Close + 1;
    }
    return OutPrimitives != TransformNone;
}

bool TryParseTransform2DPair(const FString& From, const FString& To, FRmlUiTransform2D& OutFrom, FRmlUiTransform2D& OutTo)
{
    uint8 FromPrimitive = TransformNone;
    uint8 ToPrimitive = TransformNone;
    if (!TryParseTransform2D(From, OutFrom, FromPrimitive) || !TryParseTransform2D(To, OutTo, ToPrimitive)) return false;
    return FromPrimitive == TransformNone || ToPrimitive == TransformNone || FromPrimitive == ToPrimitive;
}

bool TryParseAnimationEasing(const FString& Source, FRmlUiAnimationEasing& OutEasing)
{
    FString Text = Source.TrimStartAndEnd().ToLower();
    OutEasing = {};
    if (Text.IsEmpty() || Text == TEXT("linear"))
    {
        return true;
    }
    const auto SetSteps = [&OutEasing](int32 Count, ERmlUiAnimationStepPosition Position)
    {
        OutEasing.Type = ERmlUiAnimationEasingType::Steps;
        OutEasing.StepCount = Count;
        OutEasing.StepPosition = Position;
        return true;
    };
    if (Text == TEXT("step-start"))
        return SetSteps(1, ERmlUiAnimationStepPosition::JumpStart);
    if (Text == TEXT("step-end"))
        return SetSteps(1, ERmlUiAnimationStepPosition::JumpEnd);
    constexpr TCHAR StepsPrefix[] = TEXT("steps(");
    if (Text.StartsWith(StepsPrefix) && Text.EndsWith(TEXT(")")))
    {
        const FString Arguments = Text.Mid(UE_ARRAY_COUNT(StepsPrefix) - 1,
            Text.Len() - (UE_ARRAY_COUNT(StepsPrefix) - 1) - 1);
        TArray<FString> Parts;
        Arguments.ParseIntoArray(Parts, TEXT(","), false);
        float CountValue = 0.0f;
        if ((Parts.Num() != 1 && Parts.Num() != 2) ||
            !TryParseNumberWithSuffix(Parts[0], TEXT(""), CountValue) ||
            CountValue < 1.0f || CountValue > MAX_uint16 ||
            !FMath::IsNearlyEqual(CountValue, FMath::RoundToFloat(CountValue))) return false;
        ERmlUiAnimationStepPosition Position = ERmlUiAnimationStepPosition::JumpEnd;
        if (Parts.Num() == 2)
        {
            const FString PositionText = Parts[1].TrimStartAndEnd();
            if (PositionText == TEXT("start") || PositionText == TEXT("jump-start"))
                Position = ERmlUiAnimationStepPosition::JumpStart;
            else if (PositionText == TEXT("end") || PositionText == TEXT("jump-end"))
                Position = ERmlUiAnimationStepPosition::JumpEnd;
            else if (PositionText == TEXT("jump-none"))
                Position = ERmlUiAnimationStepPosition::JumpNone;
            else if (PositionText == TEXT("jump-both"))
                Position = ERmlUiAnimationStepPosition::JumpBoth;
            else return false;
        }
        const int32 Count = FMath::RoundToInt(CountValue);
        return Position != ERmlUiAnimationStepPosition::JumpNone || Count >= 2
            ? SetSteps(Count, Position) : false;
    }
    OutEasing.Type = ERmlUiAnimationEasingType::CubicBezier;
    if (Text == TEXT("ease"))
    {
        OutEasing.X1 = 0.25f; OutEasing.Y1 = 0.1f;
        OutEasing.X2 = 0.25f; OutEasing.Y2 = 1.0f;
        return true;
    }
    if (Text == TEXT("ease-in"))
    {
        OutEasing.X1 = 0.42f; OutEasing.Y1 = 0.0f;
        OutEasing.X2 = 1.0f; OutEasing.Y2 = 1.0f;
        return true;
    }
    if (Text == TEXT("ease-out"))
    {
        OutEasing.X1 = 0.0f; OutEasing.Y1 = 0.0f;
        OutEasing.X2 = 0.58f; OutEasing.Y2 = 1.0f;
        return true;
    }
    if (Text == TEXT("ease-in-out"))
    {
        OutEasing.X1 = 0.42f; OutEasing.Y1 = 0.0f;
        OutEasing.X2 = 0.58f; OutEasing.Y2 = 1.0f;
        return true;
    }
    constexpr TCHAR Prefix[] = TEXT("cubic-bezier(");
    if (!Text.StartsWith(Prefix) || !Text.EndsWith(TEXT(")"))) return false;
    const FString Arguments = Text.Mid(UE_ARRAY_COUNT(Prefix) - 1,
        Text.Len() - (UE_ARRAY_COUNT(Prefix) - 1) - 1);
    TArray<FString> Parts;
    Arguments.ParseIntoArray(Parts, TEXT(","), false);
    if (Parts.Num() != 4 ||
        !TryParseNumberWithSuffix(Parts[0], TEXT(""), OutEasing.X1) ||
        !TryParseNumberWithSuffix(Parts[1], TEXT(""), OutEasing.Y1) ||
        !TryParseNumberWithSuffix(Parts[2], TEXT(""), OutEasing.X2) ||
        !TryParseNumberWithSuffix(Parts[3], TEXT(""), OutEasing.Y2))
    {
        return false;
    }
    return OutEasing.X1 >= 0.0f && OutEasing.X1 <= 1.0f &&
        OutEasing.X2 >= 0.0f && OutEasing.X2 <= 1.0f;
}

class FVueModuleLoader final : public puerts::DefaultJSModuleLoader
{
public:
    explicit FVueModuleLoader(const FString& Root) : DefaultJSModuleLoader(Root)
    {
        Bootstrap = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("Puerts"))->GetContentDir(), TEXT("JavaScript"));
    }
    bool Search(const FString& RequiredDir, const FString& Module, FString& Path, FString& Absolute) override
    {
        return SearchModuleInDir(RequiredDir.IsEmpty() ? ScriptRoot : RequiredDir, Module, Path, Absolute) ||
            SearchModuleInDir(ScriptRoot, Module, Path, Absolute) || SearchModuleInDir(Bootstrap, Module, Path, Absolute);
    }
private:
    FString Bootstrap;
};
class FVueLogger final : public puerts::ILogger
{
public:
    explicit FVueLogger(URmlUiJSContext* Context) : Owner(Context) {}
    void Log(const FString& Message) const override { UE_LOG(LogTemp, Display, TEXT("RmlUiJS: %s"), *Message); }
    void Info(const FString& Message) const override { Log(Message); }
    void Warn(const FString& Message) const override { UE_LOG(LogTemp, Warning, TEXT("RmlUiJS: %s"), *Message); }
    void Error(const FString& Message) const override { if (Owner.IsValid()) Owner->ReportError(Message); }
private:
    TWeakObjectPtr<URmlUiJSContext> Owner;
};
FString JsonString(const TSharedRef<FJsonObject>& Object)
{
    FString Result;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Result));
    return Result;
}
FString AnimationResult(
    bool bAccepted,
    const FString& Route,
    uint64 Handle,
    const FString& State,
    const FString& Error = FString())
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("accepted"), bAccepted);
    Result->SetStringField(TEXT("route"), Route);
    Result->SetStringField(TEXT("handle"), Handle ? LexToString(Handle) : FString());
    Result->SetStringField(TEXT("state"), State);
    if (!Error.IsEmpty()) Result->SetStringField(TEXT("error"), Error);
    return JsonString(Result);
}

FString PropertyBatchResult(
    bool bAccepted,
    int32 Applied,
    const FString& Error = FString(),
    const TArray<int32>* FailedIndices = nullptr)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("accepted"), bAccepted);
    Result->SetNumberField(TEXT("applied"), Applied);
    if (!Error.IsEmpty()) Result->SetStringField(TEXT("error"), Error);
    if (FailedIndices && !FailedIndices->IsEmpty())
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(FailedIndices->Num());
        for (const int32 Index : *FailedIndices)
            Values.Add(MakeShared<FJsonValueNumber>(Index));
        Result->SetArrayField(TEXT("failedIndices"), MoveTemp(Values));
    }
    return JsonString(Result);
}

const TCHAR* CompletionReasonLiteral(ERmlUiAnimationCompletionReason Reason)
{
    switch (Reason)
    {
    case ERmlUiAnimationCompletionReason::Completed: return TEXT("completed");
    case ERmlUiAnimationCompletionReason::Cancelled: return TEXT("cancelled");
    case ERmlUiAnimationCompletionReason::Replaced: return TEXT("replaced");
    }
    return TEXT("cancelled");
}

FString CompletionReasonText(ERmlUiAnimationCompletionReason Reason)
{
    return CompletionReasonLiteral(Reason);
}

bool TryParseAnimationDirection(const FString& Source, ERmlUiAnimationDirection& OutDirection)
{
    const FString Direction = Source.TrimStartAndEnd().ToLower();
    if (Direction.IsEmpty() || Direction == TEXT("normal"))
        OutDirection = ERmlUiAnimationDirection::Normal;
    else if (Direction == TEXT("reverse"))
        OutDirection = ERmlUiAnimationDirection::Reverse;
    else if (Direction == TEXT("alternate"))
        OutDirection = ERmlUiAnimationDirection::Alternate;
    else if (Direction == TEXT("alternate-reverse"))
        OutDirection = ERmlUiAnimationDirection::AlternateReverse;
    else
        return false;
    return true;
}

bool TryParseAnimationFillMode(const FString& Source, ERmlUiAnimationFillMode& OutFill)
{
    const FString Fill = Source.TrimStartAndEnd().ToLower();
    if (Fill.IsEmpty() || Fill == TEXT("both"))
        OutFill = ERmlUiAnimationFillMode::Both;
    else if (Fill == TEXT("none"))
        OutFill = ERmlUiAnimationFillMode::None;
    else if (Fill == TEXT("forwards"))
        OutFill = ERmlUiAnimationFillMode::Forwards;
    else if (Fill == TEXT("backwards"))
        OutFill = ERmlUiAnimationFillMode::Backwards;
    else
        return false;
    return true;
}

struct FPreparedNodeAnimation
{
    FRmlUiAnimationDefinitionHandle Definition;
    FRmlUiAnimationBindingHandle Binding;
    FRmlUiAnimationContributionSpec Contribution;
    bool bLayeredContribution = false;
};

struct FAnimationBatchCommitState
{
    bool bCommitted = false;
};

bool TryHashAnimationDefinition(
    const FString& Property,
    const TArray<TSharedPtr<FJsonValue>>& Input,
    double Duration,
    double Delay,
    int32 Iterations,
    double PlaybackRate,
    ERmlUiAnimationDirection Direction,
    ERmlUiAnimationFillMode Fill,
    FSHAHash& OutHash)
{
    ERmlUiAnimatedProperty ParsedProperty = ERmlUiAnimatedProperty::None;
    if (!TryParseAnimatedProperty(Property, ParsedProperty)) return false;
    const bool bOpacity = ParsedProperty == ERmlUiAnimatedProperty::Opacity;

    FSHA1 Hash;
    const uint8 PropertyKind = static_cast<uint8>(ParsedProperty);
    Hash.Update(PropertyKind);
    Hash.Update(Duration);
    Hash.Update(Delay);
    Hash.Update(Iterations);
    Hash.Update(PlaybackRate);
    Hash.Update(static_cast<uint8>(Direction));
    Hash.Update(static_cast<uint8>(Fill));
    Hash.Update(Input.Num());
    const auto UpdateString = [&Hash](const FString& Value)
    {
        Hash.Update(Value.Len());
        Hash.UpdateWithString(*Value, Value.Len());
    };
    for (const TSharedPtr<FJsonValue>& Value : Input)
    {
        if (!Value || Value->Type != EJson::Object) return false;
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        double Offset = 0.0;
        if (!Object->TryGetNumberField(TEXT("offset"), Offset)) return false;
        Hash.Update(Offset);

        FString TextValue;
        if (Object->TryGetStringField(TEXT("value"), TextValue))
        {
            const uint8 ValueKind = 1;
            Hash.Update(ValueKind);
            UpdateString(TextValue);
        }
        else
        {
            double NumberValue = 0.0;
            if (!bOpacity || !Object->TryGetNumberField(TEXT("value"), NumberValue)) return false;
            const uint8 ValueKind = 2;
            Hash.Update(ValueKind);
            Hash.Update(NumberValue);
        }

        FString Easing;
        const bool bHasEasing = Object->HasField(TEXT("easing"));
        Hash.Update(bHasEasing);
        if (bHasEasing)
        {
            if (!Object->TryGetStringField(TEXT("easing"), Easing)) return false;
            UpdateString(Easing);
        }
    }
    OutHash = Hash.Finalize();
    return true;
}

FString AnimationBatchResult(
    bool bAccepted,
    const TArray<FRmlUiAnimationHandle>& Handles,
    const FString& Error = FString(),
    int32 FailedIndex = INDEX_NONE)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("accepted"), bAccepted);
    Result->SetStringField(TEXT("route"), bAccepted ? TEXT("native") : TEXT("rejected"));
    Result->SetStringField(TEXT("state"), bAccepted ? TEXT("running") : TEXT("rejected"));
    TArray<TSharedPtr<FJsonValue>> HandleValues;
    HandleValues.Reserve(Handles.Num());
    for (const FRmlUiAnimationHandle Handle : Handles)
    {
        HandleValues.Add(MakeShared<FJsonValueString>(LexToString(Handle.Value)));
    }
    Result->SetArrayField(TEXT("handles"), MoveTemp(HandleValues));
    if (!Error.IsEmpty()) Result->SetStringField(TEXT("error"), Error);
    if (FailedIndex != INDEX_NONE) Result->SetNumberField(TEXT("failedIndex"), FailedIndex);
    return JsonString(Result);
}

FString AnimationPlanBatchResult(
    bool bAccepted,
    const TArray<uint64>& Handles,
    const FString& Error = FString(),
    int32 FailedIndex = INDEX_NONE,
    int32 Released = INDEX_NONE,
    const TArray<uint64>* AllocatedBytes = nullptr)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("accepted"), bAccepted);
    TArray<TSharedPtr<FJsonValue>> HandleValues;
    HandleValues.Reserve(Handles.Num());
    for (const uint64 Handle : Handles)
        HandleValues.Add(MakeShared<FJsonValueString>(LexToString(Handle)));
    Result->SetArrayField(TEXT("handles"), MoveTemp(HandleValues));
    if (AllocatedBytes)
    {
        TArray<TSharedPtr<FJsonValue>> ByteValues;
        ByteValues.Reserve(AllocatedBytes->Num());
        for (const uint64 Value : *AllocatedBytes)
            ByteValues.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Value)));
        Result->SetArrayField(TEXT("allocatedBytes"), MoveTemp(ByteValues));
    }
    if (!Error.IsEmpty()) Result->SetStringField(TEXT("error"), Error);
    if (FailedIndex != INDEX_NONE) Result->SetNumberField(TEXT("failedIndex"), FailedIndex);
    if (Released != INDEX_NONE) Result->SetNumberField(TEXT("released"), Released);
    return JsonString(Result);
}

class FPackedAnimationReader
{
public:
    explicit FPackedAnimationReader(const FArrayBuffer& Payload)
        : Data(static_cast<const uint8*>(Payload.Data)), Size(Payload.Length)
    {
    }

    template<typename T>
    bool Read(T& Out)
    {
        if (!Data || Offset > Size || sizeof(T) > Size - Offset) return false;
        FMemory::Memcpy(&Out, Data + Offset, sizeof(T));
        Offset += sizeof(T);
        return true;
    }

    bool Skip(uint64 Bytes)
    {
        if (!Data || Offset > Size || Bytes > Size - Offset) return false;
        Offset += Bytes;
        return true;
    }

    bool AtEnd() const { return Offset == Size; }
    uint64 Remaining() const { return Offset <= Size ? Size - Offset : 0; }

private:
    const uint8* Data = nullptr;
    uint64 Size = 0;
    uint64 Offset = 0;
};

bool ReadPackedHeader(FPackedAnimationReader& Reader, uint32 ExpectedMagic, uint32& OutCount,
    uint16 MaxVersion = 1, uint16* OutVersion = nullptr)
{
    uint32 Magic = 0;
    uint16 Version = 0;
    uint16 Reserved = 0;
    const bool bValid = Reader.Read(Magic) && Reader.Read(Version) && Reader.Read(Reserved) &&
        Reader.Read(OutCount) && Magic == ExpectedMagic && Version >= 1 && Version <= MaxVersion &&
        Reserved == 0 && OutCount > 0 && OutCount <= 4096;
    if (bValid && OutVersion) *OutVersion = Version;
    return bValid;
}

bool ReadPackedEasing(FPackedAnimationReader& Reader, FRmlUiAnimationEasing& OutEasing)
{
    uint8 Type = 0;
    uint8 Reserved[3]{};
    float Payload[4]{};
    if (!Reader.Read(Type) || !Reader.Read(Reserved) ||
        !Reader.Read(Payload[0]) || !Reader.Read(Payload[1]) ||
        !Reader.Read(Payload[2]) || !Reader.Read(Payload[3]) ||
        Type > static_cast<uint8>(ERmlUiAnimationEasingType::Steps) ||
        Reserved[0] != 0 || Reserved[1] != 0 || Reserved[2] != 0)
        return false;
    OutEasing = {};
    OutEasing.Type = static_cast<ERmlUiAnimationEasingType>(Type);
    if (OutEasing.Type == ERmlUiAnimationEasingType::Steps)
    {
        if (!FMath::IsFinite(Payload[0]) || !FMath::IsFinite(Payload[1]) ||
            Payload[0] < 1.0f || Payload[0] > MAX_uint16 ||
            !FMath::IsNearlyEqual(Payload[0], FMath::RoundToFloat(Payload[0])) ||
            Payload[1] < 0.0f || Payload[1] > static_cast<float>(ERmlUiAnimationStepPosition::JumpBoth) ||
            !FMath::IsNearlyEqual(Payload[1], FMath::RoundToFloat(Payload[1])) ||
            Payload[2] != 0.0f || Payload[3] != 0.0f) return false;
        OutEasing.StepCount = FMath::RoundToInt(Payload[0]);
        OutEasing.StepPosition = static_cast<ERmlUiAnimationStepPosition>(FMath::RoundToInt(Payload[1]));
        return OutEasing.StepPosition != ERmlUiAnimationStepPosition::JumpNone || OutEasing.StepCount >= 2;
    }
    OutEasing.X1 = Payload[0]; OutEasing.Y1 = Payload[1];
    OutEasing.X2 = Payload[2]; OutEasing.Y2 = Payload[3];
    return FMath::IsFinite(OutEasing.X1) && FMath::IsFinite(OutEasing.Y1) &&
        FMath::IsFinite(OutEasing.X2) && FMath::IsFinite(OutEasing.Y2) &&
        OutEasing.X1 >= 0.0f && OutEasing.X1 <= 1.0f &&
        OutEasing.X2 >= 0.0f && OutEasing.X2 <= 1.0f;
}

FRmlUiAnimationCompletionCallback MakeCompiledAnimationCompletionCallback(
    FRmlUiAnimationRuntime& Runtime,
    FRmlUiAnimationBindingHandle Binding,
    TWeakObjectPtr<URmlUiJSContext> WeakContext,
    TSharedPtr<FAnimationBatchCommitState> BatchState)
{
    return [&Runtime, Binding, WeakContext, BatchState](
        FRmlUiAnimationHandle CompletedHandle, ERmlUiAnimationCompletionReason Reason)
    {
        Runtime.ReleaseBinding(Binding);
        if (BatchState.IsValid() && !BatchState->bCommitted) return;
        if (URmlUiJSContext* Context = WeakContext.Get())
            Context->QueueAnimationEvent(CompletedHandle.Value, static_cast<uint8>(Reason));
    };
}

void ReleasePreparedAnimation(
    FRmlUiAnimationRuntime& Runtime,
    const FPreparedNodeAnimation& Prepared)
{
    if (Prepared.Binding.IsValid()) Runtime.ReleaseBinding(Prepared.Binding);
    if (Prepared.Definition.IsValid()) Runtime.ReleaseDefinition(Prepared.Definition);
}

FRmlUiAnimationCompletionCallback MakeAnimationCompletionCallback(
    FRmlUiAnimationRuntime& Runtime,
    const FPreparedNodeAnimation& Prepared,
    TWeakObjectPtr<URmlUiJSContext> WeakContext,
    TSharedPtr<FAnimationBatchCommitState> BatchState = {})
{
    return [&Runtime, Prepared, WeakContext, BatchState](
        FRmlUiAnimationHandle CompletedHandle, ERmlUiAnimationCompletionReason Reason)
    {
        ReleasePreparedAnimation(Runtime, Prepared);
        if (BatchState.IsValid() && !BatchState->bCommitted) return;
        if (URmlUiJSContext* Context = WeakContext.Get())
        {
            Context->QueueAnimationEvent(CompletedHandle.Value, static_cast<uint8>(Reason));
        }
    };
}

bool PrepareNodeKeyframeAnimation(
    FRmlUiAnimationRuntime& Runtime,
    RmlUE_View* View,
    int32 Node,
    const FString& Property,
    const TArray<TSharedPtr<FJsonValue>>& Input,
    const FJsonObject& Options,
    FPreparedNodeAnimation& OutPrepared,
    FString& OutError,
    TMap<FSHAHash, FRmlUiAnimationDefinitionHandle>* SharedDefinitions = nullptr)
{
    OutPrepared = {};
    if (!View || Node <= 0 || !RmlUE_IsNodeValid(View, static_cast<RmlUE_Node>(Node)))
    {
        OutError = TEXT("invalid_target_or_payload_size");
        return false;
    }
    double Duration = 0.0;
    double Delay = 0.0;
    double IterationsNumber = 1.0;
    double PlaybackRate = 1.0;
    if (!Options.TryGetNumberField(TEXT("duration"), Duration) ||
        (Options.HasField(TEXT("delay")) && !Options.TryGetNumberField(TEXT("delay"), Delay)) ||
        (Options.HasField(TEXT("iterations")) && !Options.TryGetNumberField(TEXT("iterations"), IterationsNumber)) ||
        (Options.HasField(TEXT("playbackRate")) && !Options.TryGetNumberField(TEXT("playbackRate"), PlaybackRate)) ||
        !FMath::IsFinite(Duration) || Duration < 0.0 ||
        !FMath::IsFinite(Delay) || Delay < 0.0 ||
        !FMath::IsFinite(IterationsNumber) || IterationsNumber < 1.0 ||
        IterationsNumber > MAX_int32 || FMath::FloorToDouble(IterationsNumber) != IterationsNumber ||
        !FMath::IsFinite(PlaybackRate) || PlaybackRate <= 0.0)
    {
        OutError = TEXT("invalid_timing_options");
        return false;
    }
    FString DirectionText;
    if (Options.HasField(TEXT("direction")) &&
        !Options.TryGetStringField(TEXT("direction"), DirectionText))
    {
        OutError = TEXT("invalid_direction_type");
        return false;
    }
    ERmlUiAnimationDirection Direction = ERmlUiAnimationDirection::Normal;
    if (!TryParseAnimationDirection(DirectionText, Direction))
    {
        OutError = TEXT("unsupported_direction");
        return false;
    }
    FString Fill = TEXT("both");
    if (Options.HasField(TEXT("fill")) && !Options.TryGetStringField(TEXT("fill"), Fill))
    {
        OutError = TEXT("invalid_fill_type");
        return false;
    }
    ERmlUiAnimationFillMode FillMode = ERmlUiAnimationFillMode::Both;
    if (!TryParseAnimationFillMode(Fill, FillMode))
    {
        OutError = TEXT("unsupported_fill");
        return false;
    }
    FString Composite = TEXT("replace");
    if (Options.HasField(TEXT("composite")) && !Options.TryGetStringField(TEXT("composite"), Composite))
    {
        OutError = TEXT("invalid_composite_type");
        return false;
    }
    const bool bLayeredContribution = Composite.Equals(
        TEXT("layered-replace"), ESearchCase::IgnoreCase);
    if (!Composite.Equals(TEXT("replace"), ESearchCase::IgnoreCase) && !bLayeredContribution)
    {
        OutError = TEXT("unsupported_composite");
        return false;
    }
    double CompositionOrder = 0.0;
    if (bLayeredContribution &&
        (!Options.TryGetNumberField(TEXT("compositionOrder"), CompositionOrder) ||
            !FMath::IsFinite(CompositionOrder) || CompositionOrder < MIN_int32 ||
            CompositionOrder > MAX_int32 || FMath::FloorToDouble(CompositionOrder) != CompositionOrder))
    {
        OutError = TEXT("invalid_composition_order");
        return false;
    }
    if (!bLayeredContribution && Options.HasField(TEXT("compositionOrder")))
    {
        OutError = TEXT("unexpected_composition_order");
        return false;
    }
    OutPrepared.bLayeredContribution = bLayeredContribution;
    OutPrepared.Contribution.Order = static_cast<int32>(CompositionOrder);
    OutPrepared.Contribution.bSuppressBeforeStart =
        FillMode == ERmlUiAnimationFillMode::None || FillMode == ERmlUiAnimationFillMode::Forwards;
    OutPrepared.Contribution.bLayered = bLayeredContribution;
    if (Input.Num() < 2 || Input.Num() > 4096)
    {
        OutError = TEXT("invalid_keyframe_array");
        return false;
    }

    const int32 Iterations = static_cast<int32>(IterationsNumber);
    ERmlUiAnimatedProperty AnimatedProperty = ERmlUiAnimatedProperty::None;
    if (!TryParseAnimatedProperty(Property, AnimatedProperty))
    {
        OutError = TEXT("unsupported_property");
        return false;
    }
    FSHAHash DefinitionKey;
    const bool bHasDefinitionKey = SharedDefinitions && TryHashAnimationDefinition(
        Property, Input, Duration, Delay, Iterations, PlaybackRate, Direction, FillMode, DefinitionKey);
    if (bHasDefinitionKey)
    {
        if (const FRmlUiAnimationDefinitionHandle* Existing = SharedDefinitions->Find(DefinitionKey))
        {
            OutPrepared.Definition = *Existing;
            FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceWork::AnimationDefinitionsReused);
        }
    }

    const bool bNeedsDefinition = !OutPrepared.Definition.IsValid();
    if (bNeedsDefinition && IsScalarAnimatedProperty(AnimatedProperty))
    {
        FRmlUiFloatAnimationDefinition Parsed;
        Parsed.DurationSeconds = Duration;
        Parsed.DelaySeconds = Delay;
        Parsed.Iterations = Iterations;
        Parsed.PlaybackRate = PlaybackRate;
        Parsed.Direction = Direction;
        Parsed.Fill = FillMode;
        Parsed.Keyframes.Reserve(Input.Num());
        for (const TSharedPtr<FJsonValue>& Value : Input)
        {
            if (!Value || Value->Type != EJson::Object)
            {
                OutError = TEXT("keyframes_must_be_objects");
                return false;
            }
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            double Offset = 0.0;
            if (!Object->TryGetNumberField(TEXT("offset"), Offset))
            {
                OutError = TEXT("keyframe_offset_must_be_numeric");
                return false;
            }
            float ScalarValue = 0.0f;
            FString TextValue;
            if (AnimatedProperty == ERmlUiAnimatedProperty::Opacity &&
                !Object->TryGetStringField(TEXT("value"), TextValue))
            {
                double NumberValue = 0.0;
                if (!Object->TryGetNumberField(TEXT("value"), NumberValue))
                {
                    OutError = TEXT("opacity_value_must_be_numeric");
                    return false;
                }
                TextValue = FString::SanitizeFloat(NumberValue);
            }
            else if (AnimatedProperty != ERmlUiAnimatedProperty::Opacity &&
                !Object->TryGetStringField(TEXT("value"), TextValue))
            {
                OutError = TEXT("layout_value_must_use_px");
                return false;
            }
            bool bValidScalar = false;
            if (AnimatedProperty == ERmlUiAnimatedProperty::Opacity)
                bValidScalar = TryParseOpacity(TextValue, ScalarValue);
            else if (AnimatedProperty == ERmlUiAnimatedProperty::Visibility)
            {
                if (TextValue.Equals(TEXT("visible"), ESearchCase::IgnoreCase)) { ScalarValue = 1.f; bValidScalar = true; }
                else if (TextValue.Equals(TEXT("hidden"), ESearchCase::IgnoreCase)) { ScalarValue = 0.f; bValidScalar = true; }
            }
            else bValidScalar = TryParseNumberWithSuffix(TextValue, TEXT("px"), ScalarValue);
            if (!bValidScalar ||
                (IsNonNegativeLayoutProperty(AnimatedProperty) && ScalarValue < 0.0f))
            {
                OutError = AnimatedProperty == ERmlUiAnimatedProperty::Opacity
                    ? TEXT("opacity_value_out_of_range") : AnimatedProperty == ERmlUiAnimatedProperty::Visibility
                        ? TEXT("invalid_visibility_value") : TEXT("invalid_layout_px_value");
                return false;
            }
            FString EasingText;
            if (Object->HasField(TEXT("easing")) &&
                !Object->TryGetStringField(TEXT("easing"), EasingText))
            {
                OutError = TEXT("invalid_easing_type");
                return false;
            }
            FRmlUiAnimationEasing Easing;
            if (!TryParseAnimationEasing(EasingText, Easing))
            {
                OutError = TEXT("unsupported_easing");
                return false;
            }
            Parsed.Keyframes.Add({static_cast<float>(Offset), ScalarValue, Easing});
        }
        OutPrepared.Definition = Runtime.RegisterFloatDefinition(AnimatedProperty, Parsed);
    }
    else if (bNeedsDefinition && AnimatedProperty == ERmlUiAnimatedProperty::Transform2D)
    {
        FRmlUiTransform2DAnimationDefinition Parsed;
        Parsed.DurationSeconds = Duration;
        Parsed.DelaySeconds = Delay;
        Parsed.Iterations = Iterations;
        Parsed.PlaybackRate = PlaybackRate;
        Parsed.Direction = Direction;
        Parsed.Fill = FillMode;
        Parsed.Keyframes.Reserve(Input.Num());
        uint8 CommonPrimitive = TransformNone;
        for (const TSharedPtr<FJsonValue>& Value : Input)
        {
            if (!Value || Value->Type != EJson::Object)
            {
                OutError = TEXT("keyframes_must_be_objects");
                return false;
            }
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            double Offset = 0.0;
            FString TextValue;
            if (!Object->TryGetNumberField(TEXT("offset"), Offset) ||
                !Object->TryGetStringField(TEXT("value"), TextValue))
            {
                OutError = TEXT("transform_keyframe_shape_invalid");
                return false;
            }
            FRmlUiTransform2D Transform;
            uint8 Primitive = TransformNone;
            if (!TryParseTransform2D(TextValue, Transform, Primitive))
            {
                OutError = TEXT("unsupported_transform_value");
                return false;
            }
            if (Primitive != TransformNone)
            {
                if (CommonPrimitive != TransformNone && CommonPrimitive != Primitive)
                {
                    OutError = TEXT("mixed_transform_primitives");
                    return false;
                }
                CommonPrimitive = Primitive;
            }
            FString EasingText;
            if (Object->HasField(TEXT("easing")) &&
                !Object->TryGetStringField(TEXT("easing"), EasingText))
            {
                OutError = TEXT("invalid_easing_type");
                return false;
            }
            FRmlUiAnimationEasing Easing;
            if (!TryParseAnimationEasing(EasingText, Easing))
            {
                OutError = TEXT("unsupported_easing");
                return false;
            }
            Parsed.Keyframes.Add({static_cast<float>(Offset), Transform, Easing});
        }
        OutPrepared.Definition = Runtime.RegisterTransform2DDefinition(Parsed);
    }
    else if (bNeedsDefinition && AnimatedProperty >= ERmlUiAnimatedProperty::Color &&
        AnimatedProperty <= ERmlUiAnimatedProperty::ImageColor)
    {
        FRmlUiColorAnimationDefinition Parsed;
        Parsed.DurationSeconds = Duration;
        Parsed.DelaySeconds = Delay;
        Parsed.Iterations = Iterations;
        Parsed.PlaybackRate = PlaybackRate;
        Parsed.Direction = Direction;
        Parsed.Fill = FillMode;
        Parsed.Keyframes.Reserve(Input.Num());
        for (const TSharedPtr<FJsonValue>& Value : Input)
        {
            if (!Value || Value->Type != EJson::Object) { OutError = TEXT("keyframes_must_be_objects"); return false; }
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            double Offset = 0.0;
            FString TextValue, EasingText;
            FRmlUiColor Color;
            if (!Object->TryGetNumberField(TEXT("offset"), Offset) ||
                !Object->TryGetStringField(TEXT("value"), TextValue) || !TryParseColor(TextValue, Color))
            { OutError = TEXT("invalid_color_value"); return false; }
            if (Object->HasField(TEXT("easing")) && !Object->TryGetStringField(TEXT("easing"), EasingText))
            { OutError = TEXT("invalid_easing_type"); return false; }
            FRmlUiAnimationEasing Easing;
            if (!TryParseAnimationEasing(EasingText, Easing)) { OutError = TEXT("unsupported_easing"); return false; }
            Parsed.Keyframes.Add({static_cast<float>(Offset), Color, Easing});
        }
        OutPrepared.Definition = Runtime.RegisterColorDefinition(AnimatedProperty, Parsed);
    }
    else if (bNeedsDefinition)
    {
        OutError = TEXT("unsupported_property");
        return false;
    }

    if (!OutPrepared.Definition.IsValid())
    {
        OutError = TEXT("invalid_keyframe_offsets_or_values");
        return false;
    }
    if (bNeedsDefinition)
    {
        FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationDefinitionsRegistered);
    }
    OutPrepared.Binding = Runtime.BindNode(
        OutPrepared.Definition, View, static_cast<uint32>(Node));
    if (!OutPrepared.Binding.IsValid())
    {
        if (bNeedsDefinition) Runtime.ReleaseDefinition(OutPrepared.Definition);
        OutPrepared.Definition = {};
        OutError = TEXT("target_binding_failed");
        return false;
    }
    if (bHasDefinitionKey && bNeedsDefinition)
    {
        SharedDefinitions->Add(DefinitionKey, OutPrepared.Definition);
    }
    return true;
}

void CountNodeCall()
{
    FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed,
        ERmlUiPerformanceWork::JsNodeCalls);
}
}

URmlUiJSContext::URmlUiJSContext() = default;
URmlUiJSContext::~URmlUiJSContext() = default;

bool URmlUiJSContext::Initialize(RmlUE_View* InView, const FString& Directory, const FString& Entry,
    const FString& InVersion, const FString& State, int32 DebugPort, const TArray<TPair<FString, UObject*>>& Services)
{
    check(IsInGameThread());
    View = InView;
    StateJson = State;
    Version = InVersion;
    AnimationRuntime = &FRmlUiUnrealModule::Get().GetAnimationRuntime();
    AnimationPostAdvanceHandle = AnimationRuntime->AddPostAdvanceCallback(
        FSimpleDelegate::CreateUObject(this, &URmlUiJSContext::FlushAnimationEvents));
    RmlUE_SetNodeEventCallback(View, &URmlUiJSContext::NativeEvent, this);
    RmlUE_SetLayoutCallback(View, &URmlUiJSContext::LayoutCompleted, this);
    Environment = MakeShared<puerts::FJsEnv>(std::make_shared<FVueModuleLoader>(Directory), std::make_shared<FVueLogger>(this), DebugPort);
    TArray<TPair<FString, UObject*>> Arguments;
    Arguments.Reserve(Services.Num() + 1);
    Arguments.Emplace(TEXT("bridge"), this);
    Arguments.Append(Services);
    Environment->Start(Entry, Arguments);
    if (!bReady && LastError.IsEmpty()) LastError = TEXT("The JS entry did not call ReportReady synchronously.");
    return bReady && LastError.IsEmpty();
}

bool URmlUiJSContext::Result(int Value)
{
    if (!Value) ReportError(UTF8_TO_TCHAR(RmlUE_GetLastError()));
    return Value != 0;
}
int32 URmlUiJSContext::RootNode() { CountNodeCall(); return View ? RmlUE_GetRootNode(View) : 0; }
int32 URmlUiJSContext::FindNode(const FString& Id) { CountNodeCall(); return View ? RmlUE_FindNode(View, TCHAR_TO_UTF8(*Id)) : 0; }
int32 URmlUiJSContext::CreateNode(int32 Kind, const FString& Text) { CountNodeCall(); return View ? RmlUE_CreateNode(View, Kind, TCHAR_TO_UTF8(*Text)) : 0; }
bool URmlUiJSContext::IsNodeValid(int32 Node) { return View && RmlUE_IsNodeValid(View, Node); }
bool URmlUiJSContext::InsertNode(int32 Node, int32 Parent, int32 Before)
{
    CountNodeCall();
    return View && Result(RmlUE_InsertNode(View, Node, Parent, Before));
}
bool URmlUiJSContext::RemoveNode(int32 Node)
{
    CountNodeCall();
    return View && Result(RmlUE_RemoveNode(View, Node));
}
int32 URmlUiJSContext::ParentNode(int32 Node) { return View ? RmlUE_ParentNode(View, Node) : 0; }
int32 URmlUiJSContext::NextNode(int32 Node) { return View ? RmlUE_NextNode(View, Node) : 0; }
bool URmlUiJSContext::SetText(int32 Node, const FString& Text) { CountNodeCall(); return View && Result(RmlUE_SetNodeText(View, Node, TCHAR_TO_UTF8(*Text))); }
FString URmlUiJSContext::GetText(int32 Node)
{
    TArray<char> Buffer; Buffer.SetNumZeroed(1024 * 1024);
    return View && Result(RmlUE_GetNodeText(View, Node, Buffer.GetData(), Buffer.Num())) ? UTF8_TO_TCHAR(Buffer.GetData()) : FString();
}
bool URmlUiJSContext::SetAttribute(int32 Node, const FString& Name, const FString& Value, bool bRemove)
{
    CountNodeCall();
    return View && Result(RmlUE_SetNodeAttribute(View, Node, TCHAR_TO_UTF8(*Name), bRemove ? nullptr : TCHAR_TO_UTF8(*Value)));
}
FString URmlUiJSContext::GetAttribute(int32 Node, const FString& Name)
{
    TArray<char> Buffer; Buffer.SetNumZeroed(1024 * 1024);
    return View && Result(RmlUE_GetNodeAttribute(View, Node, TCHAR_TO_UTF8(*Name), Buffer.GetData(), Buffer.Num())) ? UTF8_TO_TCHAR(Buffer.GetData()) : FString();
}
bool URmlUiJSContext::SetProperty(int32 Node, const FString& Name, const FString& Value, bool bRemove)
{
    CountNodeCall();
    return View && Result(RmlUE_SetNodeProperty(View, Node, TCHAR_TO_UTF8(*Name), bRemove ? nullptr : TCHAR_TO_UTF8(*Value)));
}
bool URmlUiJSContext::SetInnerRml(int32 Node, const FString& Rml)
{
    CountNodeCall();
    return View && Result(RmlUE_SetNodeInnerRml(View, Node, TCHAR_TO_UTF8(*Rml)));
}
bool URmlUiJSContext::Listen(int32 Node, const FString& Type, int32 Listener, bool bCapture)
{
    return View && Result(RmlUE_ListenNode(View, Node, TCHAR_TO_UTF8(*Type), Listener, bCapture ? 1 : 0));
}
void URmlUiJSContext::Unlisten(int32 Listener) { if (View) RmlUE_UnlistenNode(View, Listener); }
void URmlUiJSContext::SetEventResult(int32 Value) { if (!EventResults.IsEmpty()) EventResults.Last() |= Value; }
void URmlUiJSContext::SaveState(const FString& Json) { StateJson = Json; }
void URmlUiJSContext::ReportReady() { bReady = true; }
void URmlUiJSContext::ReportError(const FString& Error)
{
    LastError = Error;
    UE_LOG(LogTemp, Error, TEXT("RmlUiJS: %s"), *Error);
}
void URmlUiJSContext::ReportDebugState(const FString& Json) { DebugStateJson = Json; }
void URmlUiJSContext::SetWakeSchedule(float TimerDelayMilliseconds, bool bHasAnimationFrame)
{
    if (bDisposed) return;
    bAnimationFramePending = bHasAnimationFrame;
    NextTimerWakeTime = FMath::IsFinite(TimerDelayMilliseconds) && TimerDelayMilliseconds >= 0.0f
        ? FPlatformTime::Seconds() + static_cast<double>(TimerDelayMilliseconds) / 1000.0
        : TNumericLimits<double>::Max();
    NotifyWake();
}
void URmlUiJSContext::RequestHost(int32 RequestId, const FString& Method, const FString& Json)
{
    FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceWork::JsonHostRequests);
    FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed, ERmlUiPerformanceWork::JsonHostBytes,
        FTCHARToUTF8(*Method).Length() + FTCHARToUTF8(*Json).Length());
    if (bDisposed) return;
    if (!bHostActive) { PendingHostRequests.Add({RequestId, Method, Json}); return; }
    if (!OnHostRequest.IsBound()) { ResolveHostRequest(RequestId, TEXT("No UE host handler is registered"), false); return; }
    OnHostRequest.Broadcast(RequestId, Method, Json);
}
void URmlUiJSContext::ActivateHostRequests()
{
    bHostActive = true;
    auto Queued = MoveTemp(PendingHostRequests);
    PendingHostRequests.Reset();
    for (const auto& Request : Queued) RequestHost(Request.Id, Request.Method, Request.Json);
}
void URmlUiJSContext::ResolveHostRequest(int32 RequestId, const FString& Json, bool bSuccess)
{
    if (bDisposed) return;
    auto Response = MakeShared<FJsonObject>();
    Response->SetNumberField(TEXT("id"), RequestId);
    Response->SetBoolField(TEXT("success"), bSuccess);
    Response->SetStringField(TEXT("payload"), Json);
    PendingResponses.Add(JsonString(Response));
    NotifyWake();
}
int URmlUiJSContext::NativeEvent(void* User, uint32 Listener, const RmlUE_NodeEvent* Event)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_JS_NativeEvent);
    FScopedRmlUiPerformanceTimer Timer(ERmlUiPerformanceBackend::Unattributed,
        ERmlUiPerformanceStage::JsNativeEvent);
    auto* Self = static_cast<URmlUiJSContext*>(User);
    if (Self->bDisposed) return 0;
    auto Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("listener"), Listener);
    Json->SetNumberField(TEXT("target"), Event->Target);
    Json->SetNumberField(TEXT("currentTarget"), Event->CurrentTarget);
    Json->SetStringField(TEXT("type"), UTF8_TO_TCHAR(Event->Type));
    Json->SetStringField(TEXT("value"), UTF8_TO_TCHAR(Event->Value));
    Json->SetBoolField(TEXT("checked"), Event->Checked != 0);
    Json->SetNumberField(TEXT("phase"), Event->Phase);
    Json->SetNumberField(TEXT("key"), Event->Key);
    Json->SetStringField(TEXT("keyName"), UTF8_TO_TCHAR(Event->KeyName ? Event->KeyName : ""));
    Json->SetNumberField(TEXT("button"), Event->Button);
    Json->SetNumberField(TEXT("modifiers"), Event->Modifiers);
    Json->SetNumberField(TEXT("x"), Event->X);
    Json->SetNumberField(TEXT("y"), Event->Y);
    Json->SetStringField(TEXT("code"), UTF8_TO_TCHAR(Event->Code ? Event->Code : ""));
    Json->SetStringField(TEXT("pointerType"), UTF8_TO_TCHAR(Event->PointerType ? Event->PointerType : "mouse"));
    Json->SetStringField(TEXT("data"), UTF8_TO_TCHAR(Event->Data ? Event->Data : ""));
    Json->SetNumberField(TEXT("relatedTarget"), Event->RelatedTarget);
    Json->SetNumberField(TEXT("buttons"), Event->Buttons);
    Json->SetNumberField(TEXT("pointerId"), Event->PointerId);
    Json->SetNumberField(TEXT("localX"), Event->LocalX);
    Json->SetNumberField(TEXT("localY"), Event->LocalY);
    Json->SetNumberField(TEXT("wheelX"), Event->WheelX);
    Json->SetNumberField(TEXT("wheelY"), Event->WheelY);
    Json->SetNumberField(TEXT("timestamp"), Event->Timestamp);
    Json->SetBoolField(TEXT("repeat"), Event->Repeat != 0);
    Json->SetBoolField(TEXT("isComposing"), Event->IsComposing != 0);
    Json->SetBoolField(TEXT("cancelable"), Event->Cancelable != 0);
    Json->SetBoolField(TEXT("defaultPrevented"), Event->DefaultPrevented != 0);
    Self->EventResults.Add(0);
    Self->OnNativeEvent.Broadcast(JsonString(Json));
    return Self->EventResults.Pop();
}
void URmlUiJSContext::Advance(float DeltaSeconds)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RmlUi_JS_ContextAdvance);
    FScopedRmlUiPerformanceTimer Timer(ERmlUiPerformanceBackend::Unattributed,
        ERmlUiPerformanceStage::JsFrameCallbacks);
    if (bDisposed) return;
    TArray<FString> Responses = MoveTemp(PendingResponses);
    for (const FString& Json : Responses) OnHostResponse.Broadcast(Json);
    auto Events = MoveTemp(PendingHostEvents); PendingHostEvents.Reset();
    for (const FString& Json : Events) OnHostEvent.Broadcast(Json);
    OnFrame.Broadcast(DeltaSeconds);
}
bool URmlUiJSContext::HasDispatchWake() const
{
    return !PendingResponses.IsEmpty() || !PendingHostEvents.IsEmpty();
}
bool URmlUiJSContext::HasTimerWake(double NowSeconds) const
{
    return FMath::IsFinite(NextTimerWakeTime) && NowSeconds >= NextTimerWakeTime;
}
bool URmlUiJSContext::NeedsAdvance(double NowSeconds) const
{
    return !bDisposed && (HasDispatchWake() || bAnimationFramePending || HasTimerWake(NowSeconds));
}
double URmlUiJSContext::GetNextWakeTimeSeconds(double NowSeconds) const
{
    if (bDisposed) return TNumericLimits<double>::Max();
    if (HasDispatchWake() || bAnimationFramePending) return NowSeconds;
    return NextTimerWakeTime;
}
void URmlUiJSContext::NotifyWake()
{
    if (!bDisposed && WakeCallback) WakeCallback();
}
bool URmlUiJSContext::ScrollNode(int32 Node, float Top) { return View && Result(RmlUE_ScrollNode(View, Node, Top)); }
float URmlUiJSContext::ScrollRemaining(int32 Node) { return View ? RmlUE_NodeScrollRemaining(View, Node) : 0; }
bool URmlUiJSContext::FocusNode(int32 Node) { return View && RmlUE_FocusNode(View, Node); }
void URmlUiJSContext::LayoutCompleted(void* User, uint64 Revision)
{
    auto* Self = static_cast<URmlUiJSContext*>(User);
    if (!Self->bDisposed) Self->OnAfterLayout.Broadcast(static_cast<int64>(Revision));
}
int32 URmlUiJSContext::QueryNode(int32 Root, const FString& Selector) { CountNodeCall(); return View ? RmlUE_QueryNode(View, Root, TCHAR_TO_UTF8(*Selector)) : 0; }
FString URmlUiJSContext::QueryNodes(int32 Root, const FString& Selector)
{
    CountNodeCall();
    if (!View) return TEXT("[]");
    const int Count = RmlUE_QueryNodes(View, Root, TCHAR_TO_UTF8(*Selector), nullptr, 0);
    if (Count < 0 || Count > 65536) { ReportError(TEXT("Invalid or oversized node query.")); return TEXT("[]"); }
    TArray<RmlUE_Node> Nodes; Nodes.SetNumUninitialized(Count);
    RmlUE_QueryNodes(View, Root, TCHAR_TO_UTF8(*Selector), Nodes.GetData(), Count);
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const auto Node : Nodes) Values.Add(MakeShared<FJsonValueNumber>(Node));
    FString Json; FJsonSerializer::Serialize(Values, TJsonWriterFactory<>::Create(&Json)); return Json;
}
FString URmlUiJSContext::ChildNodes(int32 Node)
{
    CountNodeCall(); if (!View) return TEXT("[]");
    const int Count = RmlUE_ChildNodes(View, Node, nullptr, 0);
    if (Count < 0 || Count > 65536) { ReportError(TEXT("Invalid child node query.")); return TEXT("[]"); }
    TArray<RmlUE_Node> Nodes; Nodes.SetNumUninitialized(Count);
    RmlUE_ChildNodes(View, Node, Nodes.GetData(), Count);
    TArray<TSharedPtr<FJsonValue>> Values; for (const auto Handle : Nodes) Values.Add(MakeShared<FJsonValueNumber>(Handle));
    FString Json; FJsonSerializer::Serialize(Values, TJsonWriterFactory<>::Create(&Json)); return Json;
}
bool URmlUiJSContext::ContainsNode(int32 Parent, int32 Child) { return View && RmlUE_ContainsNode(View, Parent, Child); }
int32 URmlUiJSContext::ActiveNode() { return View ? RmlUE_ActiveNode(View) : 0; }
bool URmlUiJSContext::BlurNode(int32 Node) { return View && RmlUE_BlurNode(View, Node); }
bool URmlUiJSContext::SetNodeClass(int32 Node, const FString& Name, bool bEnabled)
{
    CountNodeCall();
    return View && Result(RmlUE_SetNodeClass(View, Node, TCHAR_TO_UTF8(*Name), bEnabled));
}
bool URmlUiJSContext::RestartCssAnimation(int32 Node)
{
    CountNodeCall();
    if (View && CssAnimationRestartCallback && CssAnimationRestartCallback(Node)) return true;
    ReportError(TEXT("Cannot restart CSS animation: node has no active native CSS rule."));
    return false;
}
FString URmlUiJSContext::GetComputedProperty(int32 Node, const FString& Name)
{
    TArray<char> Buffer; Buffer.SetNumZeroed(16384);
    return View && Result(RmlUE_GetComputedProperty(View, Node, TCHAR_TO_UTF8(*Name), Buffer.GetData(), Buffer.Num())) ? UTF8_TO_TCHAR(Buffer.GetData()) : FString();
}
FString URmlUiJSContext::MeasureNodes(const FString& HandlesJson)
{
    CountNodeCall();
    TArray<TSharedPtr<FJsonValue>> Input;
    if (!View || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(HandlesJson), Input) || Input.Num() > 16384) {
        ReportError(TEXT("MeasureNodes requires at most 16384 integer handles.")); return TEXT("{}");
    }
    TArray<RmlUE_Node> Handles;
    for (const auto& Value : Input) {
        double Number = 0;
        if (!Value || !Value->TryGetNumber(Number) || Number < 1 || Number > MAX_int32 || FMath::FloorToDouble(Number) != Number) {
            ReportError(TEXT("MeasureNodes received an invalid handle.")); return TEXT("{}");
        }
        Handles.Add(static_cast<RmlUE_Node>(Number));
    }
    TArray<RmlUE_NodeMetrics> Metrics; Metrics.SetNumZeroed(Handles.Num());
    RmlUE_LayoutInfo Info{};
    if (!Result(RmlUE_MeasureNodes(View, Handles.GetData(), Handles.Num(), Metrics.GetData(), &Info))) return TEXT("{}");
    auto Json = MakeShared<FJsonObject>(); Json->SetNumberField(TEXT("revision"), static_cast<double>(Info.Revision));
    auto Viewport = MakeShared<FJsonObject>(); Viewport->SetNumberField(TEXT("width"), Info.Width);
    Viewport->SetNumberField(TEXT("height"), Info.Height); Viewport->SetNumberField(TEXT("dpi"), Info.Dpi);
    Json->SetObjectField(TEXT("viewport"), Viewport);
    TArray<TSharedPtr<FJsonValue>> Nodes;
    for (const auto& M : Metrics) {
        if (!M.Valid) continue;
        auto Item = MakeShared<FJsonObject>(); Item->SetNumberField(TEXT("handle"), M.Node);
        Item->SetBoolField(TEXT("visible"), M.Visible != 0);
        Item->SetNumberField(TEXT("x"), M.X); Item->SetNumberField(TEXT("y"), M.Y);
        Item->SetNumberField(TEXT("width"), M.Width); Item->SetNumberField(TEXT("height"), M.Height);
        Item->SetNumberField(TEXT("layoutX"), M.LayoutX); Item->SetNumberField(TEXT("layoutY"), M.LayoutY);
        Item->SetNumberField(TEXT("layoutWidth"), M.LayoutWidth); Item->SetNumberField(TEXT("layoutHeight"), M.LayoutHeight);
        Item->SetNumberField(TEXT("scrollTop"), M.ScrollTop); Item->SetNumberField(TEXT("scrollLeft"), M.ScrollLeft);
        Item->SetNumberField(TEXT("scrollWidth"), M.ScrollWidth); Item->SetNumberField(TEXT("scrollHeight"), M.ScrollHeight);
        Item->SetNumberField(TEXT("clientWidth"), M.ClientWidth); Item->SetNumberField(TEXT("clientHeight"), M.ClientHeight);
        Item->SetNumberField(TEXT("clipX"), M.ClipX); Item->SetNumberField(TEXT("clipY"), M.ClipY);
        Item->SetNumberField(TEXT("clipWidth"), M.ClipWidth); Item->SetNumberField(TEXT("clipHeight"), M.ClipHeight);
        Nodes.Add(MakeShared<FJsonValueObject>(Item));
    }
    Json->SetArrayField(TEXT("nodes"), Nodes); return JsonString(Json);
}
FString URmlUiJSContext::ResolveAnimationHostSnapshot(const FString& RequestJson)
{
    CountNodeCall();
    const auto Fail = [](const FString& Error)
    {
        auto Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("accepted"), false);
        Result->SetStringField(TEXT("error"), Error);
        return JsonString(Result);
    };
    if (!View || RequestJson.Len() > 1024 * 1024)
        return Fail(TEXT("invalid_snapshot_target_or_payload_size"));

    TSharedPtr<FJsonObject> Request;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RequestJson), Request) || !Request.IsValid())
        return Fail(TEXT("invalid_snapshot_json"));
    const TArray<TSharedPtr<FJsonValue>>* TargetValues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* PropertyValues = nullptr;
    if (!Request->TryGetArrayField(TEXT("targets"), TargetValues) || !TargetValues ||
        TargetValues->IsEmpty() || TargetValues->Num() > 4096)
        return Fail(TEXT("invalid_snapshot_targets"));
    if (!Request->TryGetArrayField(TEXT("properties"), PropertyValues) || !PropertyValues ||
        PropertyValues->Num() > 128)
        return Fail(TEXT("invalid_snapshot_properties"));

    TArray<FString> Properties;
    TSet<FString> SeenProperties;
    Properties.Reserve(PropertyValues->Num());
    for (const TSharedPtr<FJsonValue>& Value : *PropertyValues)
    {
        FString Property;
        if (!Value.IsValid() || !Value->TryGetString(Property))
            return Fail(TEXT("invalid_snapshot_property"));
        Property.TrimStartAndEndInline();
        Property.ToLowerInline();
        if (Property.IsEmpty() || Property.Len() > 256)
            return Fail(TEXT("invalid_snapshot_property"));
        if (!SeenProperties.Contains(Property))
        {
            SeenProperties.Add(Property);
            Properties.Add(MoveTemp(Property));
        }
    }

    TArray<RmlUE_Node> Handles;
    TArray<TArray<RmlUE_Node>> TargetGroups;
    TargetGroups.Reserve(TargetValues->Num());
    int32 TargetGroupEntryCount = 0;
    TSet<RmlUE_Node> SeenHandles;
    const auto AddHandle = [&](RmlUE_Node Handle) -> bool
    {
        if (Handle <= 0 || !RmlUE_IsNodeValid(View, Handle)) return false;
        if (!SeenHandles.Contains(Handle))
        {
            if (Handles.Num() >= 16384) return false;
            SeenHandles.Add(Handle);
            Handles.Add(Handle);
        }
        return true;
    };
    for (const TSharedPtr<FJsonValue>& Value : *TargetValues)
    {
        TArray<RmlUE_Node> Group;
        double Number = 0.0;
        FString Selector;
        if (Value.IsValid() && Value->TryGetNumber(Number))
        {
            if (Number < 1 || Number > MAX_int32 || FMath::FloorToDouble(Number) != Number)
                return Fail(TEXT("invalid_snapshot_node"));
            const RmlUE_Node Handle = static_cast<RmlUE_Node>(Number);
            if (!AddHandle(Handle)) return Fail(TEXT("invalid_snapshot_node"));
            Group.Add(Handle);
        }
        else if (Value.IsValid() && Value->TryGetString(Selector))
        {
            Selector.TrimStartAndEndInline();
            if (Selector.IsEmpty() || Selector.Len() > 1024)
                return Fail(TEXT("invalid_snapshot_selector"));
            const int Count = RmlUE_QueryNodes(
                View, RmlUE_GetRootNode(View), TCHAR_TO_UTF8(*Selector), nullptr, 0);
            if (Count <= 0 || Count > 16384 || TargetGroupEntryCount + Count > 65536)
                return Fail(Count == 0 ? TEXT("snapshot_target_not_found") : TEXT("invalid_snapshot_selector_result"));
            TArray<RmlUE_Node> Matches;
            Matches.SetNumUninitialized(Count);
            if (RmlUE_QueryNodes(
                    View, RmlUE_GetRootNode(View), TCHAR_TO_UTF8(*Selector), Matches.GetData(), Count) != Count)
                return Fail(TEXT("invalid_snapshot_selector_result"));
            for (const RmlUE_Node Handle : Matches)
            {
                if (!AddHandle(Handle)) return Fail(TEXT("invalid_snapshot_node"));
                Group.Add(Handle);
            }
        }
        else
        {
            return Fail(TEXT("invalid_snapshot_target"));
        }
        TargetGroupEntryCount += Group.Num();
        if (TargetGroupEntryCount > 65536)
            return Fail(TEXT("invalid_snapshot_selector_result"));
        TargetGroups.Add(MoveTemp(Group));
    }
    if (Handles.IsEmpty()) return Fail(TEXT("snapshot_target_not_found"));

    TArray<RmlUE_NodeMetrics> Metrics;
    Metrics.SetNumZeroed(Handles.Num());
    RmlUE_LayoutInfo Layout{};
    if (!RmlUE_MeasureNodes(View, Handles.GetData(), Handles.Num(), Metrics.GetData(), &Layout))
        return Fail(TEXT("snapshot_measure_failed"));
    bool bIncludeMetrics = false;
    Request->TryGetBoolField(TEXT("includeMetrics"), bIncludeMetrics);

    TArray<TSharedPtr<FJsonValue>> Nodes;
    Nodes.Reserve(Handles.Num());
    TArray<char> Buffer;
    Buffer.SetNumZeroed(16384);
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        const RmlUE_Node Handle = Handles[Index];
        if (!Metrics[Index].Valid) return Fail(TEXT("snapshot_stale_node"));
        auto Node = MakeShared<FJsonObject>();
        Node->SetNumberField(TEXT("node"), Handle);
        auto Computed = MakeShared<FJsonObject>();
        for (const FString& Property : Properties)
        {
            FMemory::Memzero(Buffer.GetData(), Buffer.Num());
            if (!RmlUE_GetComputedProperty(
                    View, Handle, TCHAR_TO_UTF8(*Property), Buffer.GetData(), Buffer.Num()))
                return Fail(TEXT("snapshot_property_read_failed"));
            Computed->SetStringField(Property, UTF8_TO_TCHAR(Buffer.GetData()));
        }
        Node->SetObjectField(TEXT("properties"), Computed);
        if (bIncludeMetrics)
        {
            const RmlUE_NodeMetrics& M = Metrics[Index];
            auto Box = MakeShared<FJsonObject>();
            Box->SetBoolField(TEXT("visible"), M.Visible != 0);
            Box->SetNumberField(TEXT("x"), M.X); Box->SetNumberField(TEXT("y"), M.Y);
            Box->SetNumberField(TEXT("width"), M.Width); Box->SetNumberField(TEXT("height"), M.Height);
            Box->SetNumberField(TEXT("layoutX"), M.LayoutX); Box->SetNumberField(TEXT("layoutY"), M.LayoutY);
            Box->SetNumberField(TEXT("layoutWidth"), M.LayoutWidth); Box->SetNumberField(TEXT("layoutHeight"), M.LayoutHeight);
            Box->SetNumberField(TEXT("scrollTop"), M.ScrollTop); Box->SetNumberField(TEXT("scrollLeft"), M.ScrollLeft);
            Box->SetNumberField(TEXT("scrollWidth"), M.ScrollWidth); Box->SetNumberField(TEXT("scrollHeight"), M.ScrollHeight);
            Box->SetNumberField(TEXT("clientWidth"), M.ClientWidth); Box->SetNumberField(TEXT("clientHeight"), M.ClientHeight);
            Box->SetNumberField(TEXT("clipX"), M.ClipX); Box->SetNumberField(TEXT("clipY"), M.ClipY);
            Box->SetNumberField(TEXT("clipWidth"), M.ClipWidth); Box->SetNumberField(TEXT("clipHeight"), M.ClipHeight);
            Node->SetObjectField(TEXT("metrics"), Box);
        }
        Nodes.Add(MakeShared<FJsonValueObject>(Node));
    }
    auto ResultObject = MakeShared<FJsonObject>();
    ResultObject->SetBoolField(TEXT("accepted"), true);
    ResultObject->SetStringField(TEXT("revision"), FString::Printf(TEXT("%llu"), static_cast<unsigned long long>(Layout.Revision)));
    auto Viewport = MakeShared<FJsonObject>();
    Viewport->SetNumberField(TEXT("width"), Layout.Width);
    Viewport->SetNumberField(TEXT("height"), Layout.Height);
    Viewport->SetNumberField(TEXT("dpi"), Layout.Dpi);
    ResultObject->SetObjectField(TEXT("viewport"), Viewport);
    ResultObject->SetArrayField(TEXT("nodes"), Nodes);
    TArray<TSharedPtr<FJsonValue>> GroupValues;
    GroupValues.Reserve(TargetGroups.Num());
    for (const TArray<RmlUE_Node>& Group : TargetGroups)
    {
        TArray<TSharedPtr<FJsonValue>> HandlesInGroup;
        HandlesInGroup.Reserve(Group.Num());
        for (const RmlUE_Node Handle : Group)
            HandlesInGroup.Add(MakeShared<FJsonValueNumber>(Handle));
        GroupValues.Add(MakeShared<FJsonValueArray>(MoveTemp(HandlesInGroup)));
    }
    ResultObject->SetArrayField(TEXT("targetGroups"), MoveTemp(GroupValues));
    return JsonString(ResultObject);
}
bool URmlUiJSContext::SetModalRoot(int32 Root, int32 InitialFocus) { return View && Result(RmlUE_SetModalRoot(View, Root, InitialFocus)); }
bool URmlUiJSContext::CaptureNode(int32 Node, int32 PointerId) { return View && Result(RmlUE_CaptureNode(View, Node, PointerId)); }
bool URmlUiJSContext::ReleaseCaptureNode(int32 Node, int32 PointerId) { return View && Result(RmlUE_ReleaseCaptureNode(View, Node, PointerId)); }
bool URmlUiJSContext::AnimateNode(int32 Node, const FString& Property, const FString& From, const FString& To, float Duration, int32 Iterations)
{
    if (!View) return false;
    FRmlUiAnimationRuntime& Runtime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
    if (Property.Equals(TEXT("opacity"), ESearchCase::IgnoreCase))
    {
        float Start = 0.0f;
        float End = 0.0f;
        if (Duration > 0.0f && Iterations > 0 && TryParseOpacity(From, Start) && TryParseOpacity(To, End))
        {
            FRmlUiFloatAnimationDesc Desc;
            Desc.From = Start;
            Desc.To = End;
            Desc.DurationSeconds = Duration;
            Desc.Iterations = Iterations;
            return Runtime.PlayNodeFloat(View, static_cast<uint32>(Node), ERmlUiAnimatedProperty::Opacity, Desc).IsValid();
        }
        Runtime.CancelNodeAnimation(View, static_cast<uint32>(Node), ERmlUiAnimatedProperty::Opacity);
    }
    else if (Property.Equals(TEXT("transform"), ESearchCase::IgnoreCase))
    {
        FRmlUiTransform2D Start;
        FRmlUiTransform2D End;
        if (Duration > 0.0f && Iterations > 0 && TryParseTransform2DPair(From, To, Start, End))
        {
            FRmlUiTransform2DAnimationDesc Desc;
            Desc.From = Start;
            Desc.To = End;
            Desc.DurationSeconds = Duration;
            Desc.Iterations = Iterations;
            return Runtime.PlayNodeTransform2D(View, static_cast<uint32>(Node), Desc).IsValid();
        }
        Runtime.CancelNodeAnimation(View, static_cast<uint32>(Node), ERmlUiAnimatedProperty::Transform2D);
    }
    return Result(RmlUE_AnimateNode(View, Node, TCHAR_TO_UTF8(*Property), TCHAR_TO_UTF8(*From), TCHAR_TO_UTF8(*To), Duration, Iterations));
}
bool URmlUiJSContext::AnimateNodeKeyframes(
    int32 Node, const FString& Property, const FString& KeyframesJson,
    float Duration, int32 Iterations)
{
    TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
    Options->SetNumberField(TEXT("duration"), Duration);
    Options->SetNumberField(TEXT("iterations"), Iterations);
    const FString ResultJson = StartNodeKeyframeAnimation(
        Node, Property, KeyframesJson, JsonString(Options));
    TSharedPtr<FJsonObject> ResultObject;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResultJson), ResultObject) ||
        !ResultObject.IsValid())
    {
        LastError = TEXT("AnimateNodeKeyframes received an invalid adapter result.");
        return false;
    }
    bool bAccepted = false;
    ResultObject->TryGetBoolField(TEXT("accepted"), bAccepted);
    if (!bAccepted)
    {
        ResultObject->TryGetStringField(TEXT("error"), LastError);
    }
    return bAccepted;
}

FString URmlUiJSContext::StartNodeKeyframeAnimation(
    int32 Node, const FString& Property, const FString& KeyframesJson,
    const FString& OptionsJson)
{
    const auto Fail = [](const FString& Message)
    {
        return AnimationResult(false, TEXT("rejected"), 0, TEXT("rejected"), Message);
    };
    if (!View || Node <= 0 || KeyframesJson.Len() > 1024 * 1024 ||
        OptionsJson.Len() > 64 * 1024)
    {
        return Fail(TEXT("invalid_target_or_payload_size"));
    }

    TSharedPtr<FJsonObject> Options;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OptionsJson), Options) ||
        !Options.IsValid())
    {
        return Fail(TEXT("invalid_options_json"));
    }
    TArray<TSharedPtr<FJsonValue>> Input;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(KeyframesJson), Input) ||
        Input.Num() < 2 || Input.Num() > 4096)
    {
        return Fail(TEXT("invalid_keyframe_array"));
    }

    FRmlUiAnimationRuntime& Runtime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
    FPreparedNodeAnimation Prepared;
    FString Error;
    if (!PrepareNodeKeyframeAnimation(
        Runtime, View, Node, Property, Input, *Options, Prepared, Error))
        return Fail(Error);
    if (Prepared.bLayeredContribution)
    {
        ReleasePreparedAnimation(Runtime, Prepared);
        return Fail(TEXT("layered_composite_requires_batch"));
    }
    const TWeakObjectPtr<URmlUiJSContext> WeakThis(this);
    const FRmlUiAnimationHandle Animation = Runtime.PlayBinding(
        Prepared.Binding, {}, MakeAnimationCompletionCallback(Runtime, Prepared, WeakThis));
    if (!Animation.IsValid())
    {
        ReleasePreparedAnimation(Runtime, Prepared);
        return Fail(TEXT("playback_start_failed"));
    }
    return AnimationResult(true, TEXT("native"), Animation.Value, TEXT("running"));
}

FString URmlUiJSContext::StartNodeKeyframeAnimationBatch(const FString& RequestsJson)
{
    const auto Fail = [](const FString& Message, int32 FailedIndex = INDEX_NONE)
    {
        return AnimationBatchResult(false, {}, Message, FailedIndex);
    };
    if (!View || RequestsJson.Len() > 4 * 1024 * 1024)
        return Fail(TEXT("invalid_batch_payload_size"));

    TArray<TSharedPtr<FJsonValue>> Requests;
    {
        FScopedRmlUiPerformanceTimer ParseTimer(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceStage::AnimationStartBatchParse);
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RequestsJson), Requests) ||
            Requests.IsEmpty() || Requests.Num() > 4096)
            return Fail(TEXT("invalid_batch_array"));
    }

    struct FBatchRequest
    {
        int32 Node = 0;
        FString Property;
        TArray<TSharedPtr<FJsonValue>> Keyframes;
        TSharedPtr<FJsonObject> Options;
        bool bLayeredContribution = false;
        int32 CompositionOrder = 0;
    };
    TArray<FBatchRequest> ParsedRequests;
    ParsedRequests.Reserve(Requests.Num());
    TMap<FString, TSet<int32>> LayeredOrdersByTarget;
    TSet<FString> NormalTargets;
    for (int32 Index = 0; Index < Requests.Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Value = Requests[Index];
        if (!Value || Value->Type != EJson::Object)
            return Fail(TEXT("batch_entries_must_be_objects"), Index);
        const TSharedPtr<FJsonObject> Request = Value->AsObject();
        double NodeNumber = 0.0;
        FString Property;
        if (!Request->TryGetNumberField(TEXT("node"), NodeNumber) ||
            !FMath::IsFinite(NodeNumber) || NodeNumber <= 0.0 || NodeNumber > MAX_int32 ||
            FMath::FloorToDouble(NodeNumber) != NodeNumber ||
            !Request->TryGetStringField(TEXT("property"), Property) ||
            Property.TrimStartAndEnd().IsEmpty() ||
            !Request->HasTypedField<EJson::Array>(TEXT("keyframes")) ||
            !Request->HasTypedField<EJson::Object>(TEXT("options")))
            return Fail(TEXT("invalid_batch_entry_shape"), Index);
        const int32 Node = static_cast<int32>(NodeNumber);
        if (!RmlUE_IsNodeValid(View, static_cast<RmlUE_Node>(Node)))
            return Fail(TEXT("invalid_target_or_payload_size"), Index);
        Property = Property.TrimStartAndEnd();
        const FString TargetProperty = FString::Printf(
            TEXT("%d:%s"), Node, *Property.ToLower());
        const TSharedPtr<FJsonObject> Options = Request->GetObjectField(TEXT("options"));
        FString Composite = TEXT("replace");
        if (Options->HasField(TEXT("composite")) &&
            !Options->TryGetStringField(TEXT("composite"), Composite))
            return Fail(TEXT("invalid_composite_type"), Index);
        const bool bLayeredContribution = Composite.Equals(
            TEXT("layered-replace"), ESearchCase::IgnoreCase);
        double CompositionOrderNumber = 0.0;
        if (bLayeredContribution &&
            (!Options->TryGetNumberField(TEXT("compositionOrder"), CompositionOrderNumber) ||
                !FMath::IsFinite(CompositionOrderNumber) ||
                CompositionOrderNumber < MIN_int32 || CompositionOrderNumber > MAX_int32 ||
                FMath::FloorToDouble(CompositionOrderNumber) != CompositionOrderNumber))
            return Fail(TEXT("invalid_composition_order"), Index);
        const int32 CompositionOrder = static_cast<int32>(CompositionOrderNumber);
        if (bLayeredContribution)
        {
            if (NormalTargets.Contains(TargetProperty) ||
                LayeredOrdersByTarget.FindOrAdd(TargetProperty).Contains(CompositionOrder))
                return Fail(TEXT("duplicate_batch_contribution_order"), Index);
            LayeredOrdersByTarget.FindChecked(TargetProperty).Add(CompositionOrder);
        }
        else
        {
            if (NormalTargets.Contains(TargetProperty) || LayeredOrdersByTarget.Contains(TargetProperty))
                return Fail(TEXT("duplicate_batch_target_property"), Index);
            NormalTargets.Add(TargetProperty);
        }

        FBatchRequest& Parsed = ParsedRequests.AddDefaulted_GetRef();
        Parsed.Node = Node;
        Parsed.Property = MoveTemp(Property);
        Parsed.Keyframes = Request->GetArrayField(TEXT("keyframes"));
        Parsed.Options = Options;
        Parsed.bLayeredContribution = bLayeredContribution;
        Parsed.CompositionOrder = CompositionOrder;
    }

    FRmlUiAnimationRuntime& Runtime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
    TArray<FPreparedNodeAnimation> Prepared;
    Prepared.Reserve(ParsedRequests.Num());
    {
        FScopedRmlUiPerformanceTimer PrepareTimer(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceStage::AnimationStartBatchPrepare);
        TMap<FSHAHash, FRmlUiAnimationDefinitionHandle> SharedDefinitions;
        TMap<FSHAHash, FRmlUiAnimationDefinitionHandle>* SharedDefinitionTable =
            bShareAnimationDefinitionsInBatch ? &SharedDefinitions : nullptr;
        for (int32 Index = 0; Index < ParsedRequests.Num(); ++Index)
        {
            const FBatchRequest& Request = ParsedRequests[Index];
            FPreparedNodeAnimation& Item = Prepared.AddDefaulted_GetRef();
            FString Error;
            if (!PrepareNodeKeyframeAnimation(
                Runtime, View, Request.Node, Request.Property,
                Request.Keyframes, *Request.Options, Item, Error, SharedDefinitionTable))
            {
                Prepared.Pop();
                for (const FPreparedNodeAnimation& Existing : Prepared)
                    ReleasePreparedAnimation(Runtime, Existing);
                return Fail(Error, Index);
            }
        }
    }

    TArray<FRmlUiAnimationBindingHandle> Bindings;
    TArray<FRmlUiAnimationContributionSpec> Contributions;
    TArray<FRmlUiAnimationCompletionCallback> OnCompletes;
    Bindings.Reserve(Prepared.Num());
    Contributions.Reserve(Prepared.Num());
    OnCompletes.Reserve(Prepared.Num());
    const TWeakObjectPtr<URmlUiJSContext> WeakThis(this);
    const TSharedRef<FAnimationBatchCommitState> CommitState =
        MakeShared<FAnimationBatchCommitState>();
    for (const FPreparedNodeAnimation& Item : Prepared)
    {
        Bindings.Add(Item.Binding);
        Contributions.Add(Item.Contribution);
        OnCompletes.Add(MakeAnimationCompletionCallback(Runtime, Item, WeakThis, CommitState));
    }

    TArray<FRmlUiAnimationHandle> Handles;
    const bool bHasLayeredContributions = Prepared.ContainsByPredicate(
        [](const FPreparedNodeAnimation& Item) { return Item.bLayeredContribution; });
    int32 Played = 0;
    {
        FScopedRmlUiPerformanceTimer PlayTimer(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceStage::AnimationStartBatchPlay);
        Played = bHasLayeredContributions
            ? Runtime.PlayContributionBindings(Bindings, Contributions, Handles, MoveTemp(OnCompletes))
            : Runtime.PlayBindings(Bindings, Handles, MoveTemp(OnCompletes));
    }
    if (Played != Prepared.Num())
    {
        for (const FPreparedNodeAnimation& Item : Prepared)
            ReleasePreparedAnimation(Runtime, Item);
        return Fail(TEXT("playback_start_failed"));
    }
    CommitState->bCommitted = true;
    return AnimationBatchResult(true, Handles);
}

FString URmlUiJSContext::RegisterAnimationPlansPacked(const FArrayBuffer& Payload)
{
    const auto Fail = [](const FString& Message, int32 FailedIndex = INDEX_NONE)
    {
        return AnimationPlanBatchResult(false, {}, Message, FailedIndex);
    };
    if (!AnimationRuntime || !Payload.Data || Payload.Length > 4 * 1024 * 1024)
        return Fail(TEXT("invalid_plan_payload_size"));

    FScopedRmlUiPerformanceTimer Timer(
        ERmlUiPerformanceBackend::Unattributed,
        ERmlUiPerformanceStage::AnimationPlanRegisterPacked);
    FPackedAnimationReader Reader(Payload);
    uint32 Count = 0;
    uint16 PlanPayloadVersion = 0;
    constexpr uint32 Magic = 0x31504152; // RAP1
    if (!ReadPackedHeader(Reader, Magic, Count, 3, &PlanPayloadVersion)) return Fail(TEXT("invalid_plan_header"));
    int32 ActivePlanCount = 0;
    for (const FCompiledAnimationPlan& Plan : CompiledAnimationPlans)
        ActivePlanCount += Plan.bActive ? 1 : 0;
    if (ActivePlanCount + static_cast<int32>(Count) > 4096)
        return Fail(TEXT("plan_capacity_exceeded"));

    struct FRegisteredPlan
    {
        FRmlUiAnimationDefinitionHandle Definition;
        uint64 AllocatedBytes = 0;
        ERmlUiAnimatedProperty Property = ERmlUiAnimatedProperty::None;
        ERmlUiAnimationFillMode Fill = ERmlUiAnimationFillMode::Both;
    };
    TArray<FRegisteredPlan> Registered;
    Registered.Reserve(Count);
    uint64 RegisteredBytes = 0;
    const auto Rollback = [this, &Registered]()
    {
        for (const FRegisteredPlan& Plan : Registered)
            AnimationRuntime->ReleaseDefinition(Plan.Definition);
    };

    for (uint32 Index = 0; Index < Count; ++Index)
    {
        uint8 PropertyByte = 0;
        uint8 DirectionByte = 0;
        uint16 KeyframeCount = 0;
        uint32 Iterations = 0;
        double Duration = 0.0;
        double Delay = 0.0;
        double PlaybackRate = 0.0;
        uint8 FillByte = static_cast<uint8>(ERmlUiAnimationFillMode::Both);
        uint8 Reserved[7]{};
        if (!Reader.Read(PropertyByte) || !Reader.Read(DirectionByte) ||
            !Reader.Read(KeyframeCount) || !Reader.Read(Iterations) ||
            !Reader.Read(Duration) || !Reader.Read(Delay) || !Reader.Read(PlaybackRate) ||
            (PlanPayloadVersion >= 2 && (!Reader.Read(FillByte) || !Reader.Read(Reserved))) ||
            (PropertyByte < static_cast<uint8>(ERmlUiAnimatedProperty::Opacity) ||
                PropertyByte > static_cast<uint8>(ERmlUiAnimatedProperty::ImageColor)) ||
            DirectionByte > static_cast<uint8>(ERmlUiAnimationDirection::AlternateReverse) ||
            FillByte > static_cast<uint8>(ERmlUiAnimationFillMode::Both) ||
            (PlanPayloadVersion >= 2 && (Reserved[0] || Reserved[1] || Reserved[2] || Reserved[3] ||
                Reserved[4] || Reserved[5] || Reserved[6])) ||
            KeyframeCount < 2 || KeyframeCount > 4096 || Iterations < 1 ||
            Iterations > static_cast<uint32>(MAX_int32) ||
            !FMath::IsFinite(Duration) || Duration < 0.0 ||
            !FMath::IsFinite(Delay) || Delay < 0.0 ||
            !FMath::IsFinite(PlaybackRate) || PlaybackRate <= 0.0)
        {
            Rollback();
            return Fail(TEXT("invalid_plan_definition"), static_cast<int32>(Index));
        }

        FRegisteredPlan Parsed;
        Parsed.Property = static_cast<ERmlUiAnimatedProperty>(PropertyByte);
        Parsed.Fill = static_cast<ERmlUiAnimationFillMode>(FillByte);
        if (IsScalarAnimatedProperty(Parsed.Property))
        {
            FRmlUiFloatAnimationDefinition Definition;
            Definition.DurationSeconds = Duration;
            Definition.DelaySeconds = Delay;
            Definition.Iterations = static_cast<int32>(Iterations);
            Definition.PlaybackRate = PlaybackRate;
            Definition.Direction = static_cast<ERmlUiAnimationDirection>(DirectionByte);
            Definition.Fill = Parsed.Fill;
            Definition.Keyframes.Reserve(KeyframeCount);
            for (uint16 KeyframeIndex = 0; KeyframeIndex < KeyframeCount; ++KeyframeIndex)
            {
                FRmlUiFloatAnimationKeyframe Keyframe;
                if (!Reader.Read(Keyframe.Offset) || !Reader.Read(Keyframe.Value) ||
                    !ReadPackedEasing(Reader, Keyframe.EasingToNext) ||
                    !FMath::IsFinite(Keyframe.Offset) || !FMath::IsFinite(Keyframe.Value) ||
                    (Parsed.Property == ERmlUiAnimatedProperty::Opacity &&
                        (Keyframe.Value < 0.0f || Keyframe.Value > 1.0f)) ||
                    (Parsed.Property == ERmlUiAnimatedProperty::Visibility &&
                        Keyframe.Value != 0.0f && Keyframe.Value != 1.0f) ||
                    (IsNonNegativeLayoutProperty(Parsed.Property) && Keyframe.Value < 0.0f))
                {
                    Rollback();
                    return Fail(TEXT("invalid_plan_keyframe"), static_cast<int32>(Index));
                }
                Definition.Keyframes.Add(Keyframe);
            }
            Parsed.Definition = AnimationRuntime->RegisterFloatDefinition(Parsed.Property, Definition);
        }
        else if (Parsed.Property == ERmlUiAnimatedProperty::Transform2D)
        {
            FRmlUiTransform2DAnimationDefinition Definition;
            Definition.DurationSeconds = Duration;
            Definition.DelaySeconds = Delay;
            Definition.Iterations = static_cast<int32>(Iterations);
            Definition.PlaybackRate = PlaybackRate;
            Definition.Direction = static_cast<ERmlUiAnimationDirection>(DirectionByte);
            Definition.Fill = Parsed.Fill;
            Definition.Keyframes.Reserve(KeyframeCount);
            for (uint16 KeyframeIndex = 0; KeyframeIndex < KeyframeCount; ++KeyframeIndex)
            {
                FRmlUiTransform2DAnimationKeyframe Keyframe;
                if (!Reader.Read(Keyframe.Offset) ||
                    !Reader.Read(Keyframe.Value.TranslationX) ||
                    !Reader.Read(Keyframe.Value.TranslationY) ||
                    !Reader.Read(Keyframe.Value.ScaleX) ||
                    !Reader.Read(Keyframe.Value.ScaleY) ||
                    !Reader.Read(Keyframe.Value.RotationDegrees) ||
                    !Reader.Read(Keyframe.Value.SkewXDegrees) ||
                    !Reader.Read(Keyframe.Value.SkewYDegrees) ||
                    !ReadPackedEasing(Reader, Keyframe.EasingToNext) ||
                    !FMath::IsFinite(Keyframe.Offset) ||
                    !FMath::IsFinite(Keyframe.Value.TranslationX) ||
                    !FMath::IsFinite(Keyframe.Value.TranslationY) ||
                    !FMath::IsFinite(Keyframe.Value.ScaleX) ||
                    !FMath::IsFinite(Keyframe.Value.ScaleY) ||
                    !FMath::IsFinite(Keyframe.Value.RotationDegrees) ||
                    !FMath::IsFinite(Keyframe.Value.SkewXDegrees) ||
                    !FMath::IsFinite(Keyframe.Value.SkewYDegrees))
                {
                    Rollback();
                    return Fail(TEXT("invalid_plan_keyframe"), static_cast<int32>(Index));
                }
                Definition.Keyframes.Add(Keyframe);
            }
            Parsed.Definition = AnimationRuntime->RegisterTransform2DDefinition(Definition);
        }
        else
        {
            FRmlUiColorAnimationDefinition Definition;
            Definition.DurationSeconds = Duration;
            Definition.DelaySeconds = Delay;
            Definition.Iterations = static_cast<int32>(Iterations);
            Definition.PlaybackRate = PlaybackRate;
            Definition.Direction = static_cast<ERmlUiAnimationDirection>(DirectionByte);
            Definition.Fill = Parsed.Fill;
            Definition.Keyframes.Reserve(KeyframeCount);
            for (uint16 KeyframeIndex = 0; KeyframeIndex < KeyframeCount; ++KeyframeIndex)
            {
                FRmlUiColorAnimationKeyframe Keyframe;
                if (!Reader.Read(Keyframe.Offset) || !Reader.Read(Keyframe.Value.Red) ||
                    !Reader.Read(Keyframe.Value.Green) || !Reader.Read(Keyframe.Value.Blue) ||
                    !Reader.Read(Keyframe.Value.Alpha) || !ReadPackedEasing(Reader, Keyframe.EasingToNext) ||
                    !FMath::IsFinite(Keyframe.Offset) || Keyframe.Value.Red < 0.f || Keyframe.Value.Red > 1.f ||
                    Keyframe.Value.Green < 0.f || Keyframe.Value.Green > 1.f || Keyframe.Value.Blue < 0.f ||
                    Keyframe.Value.Blue > 1.f || Keyframe.Value.Alpha < 0.f || Keyframe.Value.Alpha > 1.f)
                { Rollback(); return Fail(TEXT("invalid_plan_keyframe"), static_cast<int32>(Index)); }
                Definition.Keyframes.Add(Keyframe);
            }
            Parsed.Definition = AnimationRuntime->RegisterColorDefinition(Parsed.Property, Definition);
        }
        if (!Parsed.Definition.IsValid())
        {
            Rollback();
            return Fail(TEXT("invalid_keyframe_offsets_or_values"), static_cast<int32>(Index));
        }
        Parsed.AllocatedBytes = AnimationRuntime->GetDefinitionAllocatedBytes(Parsed.Definition);
        const uint64 ResidentLimit = static_cast<uint64>(
            FMath::Max<int64>(CompiledAnimationPlanResidentLimitBytes, 1024));
        if (CompiledAnimationPlanAllocatedBytes + RegisteredBytes + Parsed.AllocatedBytes > ResidentLimit)
        {
            AnimationRuntime->ReleaseDefinition(Parsed.Definition);
            Rollback();
            return Fail(TEXT("plan_memory_capacity_exceeded"), static_cast<int32>(Index));
        }
        RegisteredBytes += Parsed.AllocatedBytes;
        Registered.Add(Parsed);
    }
    if (!Reader.AtEnd())
    {
        Rollback();
        return Fail(TEXT("invalid_plan_payload_length"));
    }

    TArray<uint64> Handles;
    TArray<uint64> AllocatedBytes;
    Handles.Reserve(Registered.Num());
    AllocatedBytes.Reserve(Registered.Num());
    for (const FRegisteredPlan& RegisteredPlan : Registered)
    {
        uint32 Slot = 0;
        if (!FreeCompiledAnimationPlanSlots.IsEmpty())
        {
            Slot = FreeCompiledAnimationPlanSlots.Pop(EAllowShrinking::No);
        }
        else
        {
            Slot = static_cast<uint32>(CompiledAnimationPlans.AddDefaulted());
        }
        FCompiledAnimationPlan& Plan = CompiledAnimationPlans[Slot];
        if (Plan.Generation == 0) Plan.Generation = 1;
        Plan.Definition = RegisteredPlan.Definition.Value;
        Plan.UseCount = 0;
        Plan.AllocatedBytes = RegisteredPlan.AllocatedBytes;
        Plan.Property = static_cast<uint8>(RegisteredPlan.Property);
        Plan.CostClass = static_cast<uint8>(GetRmlUiAnimationCostClass(RegisteredPlan.Property));
        Plan.Fill = static_cast<uint8>(RegisteredPlan.Fill);
        Plan.bActive = true;
        CompiledAnimationPlanAllocatedBytes += Plan.AllocatedBytes;
        Handles.Add((static_cast<uint64>(Plan.Generation) << 32) | (static_cast<uint64>(Slot) + 1));
        AllocatedBytes.Add(Plan.AllocatedBytes);
        FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationDefinitionsRegistered);
    }
    return AnimationPlanBatchResult(true, Handles, FString(), INDEX_NONE, INDEX_NONE, &AllocatedBytes);
}

FString URmlUiJSContext::StartCompiledAnimationBatchPacked(const FArrayBuffer& Payload)
{
    const auto Fail = [](const FString& Message, int32 FailedIndex = INDEX_NONE)
    {
        return AnimationBatchResult(false, {}, Message, FailedIndex);
    };
    if (!View || !AnimationRuntime || !Payload.Data || Payload.Length > 256 * 1024)
        return Fail(TEXT("invalid_compiled_batch_payload_size"));

    FScopedRmlUiPerformanceTimer PrepareTimer(
        ERmlUiPerformanceBackend::Unattributed,
        ERmlUiPerformanceStage::AnimationCompiledBatchPrepare);
    FPackedAnimationReader Reader(Payload);
    uint32 Count = 0;
    constexpr uint32 Magic = 0x31494152; // RAI1
    if (!ReadPackedHeader(Reader, Magic, Count)) return Fail(TEXT("invalid_compiled_batch_header"));
    if (Reader.Remaining() != static_cast<uint64>(Count) * 24)
        return Fail(TEXT("invalid_compiled_batch_length"));

    struct FCompiledRequest
    {
        FRmlUiAnimationBindingHandle Binding;
        FRmlUiAnimationContributionSpec Contribution;
        uint32 PlanSlot = 0;
        bool bLayered = false;
    };
    TArray<FCompiledRequest> Prepared;
    Prepared.Reserve(Count);
    TMap<uint64, TSet<int32>> LayeredOrdersByTarget;
    TSet<uint64> NormalTargets;
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        uint32 PlanLow = 0, PlanGeneration = 0, Node = 0;
        int32 CompositionOrder = 0;
        uint8 Composite = 0;
        uint8 Reserved[7]{};
        if (!Reader.Read(PlanLow) || !Reader.Read(PlanGeneration) || !Reader.Read(Node) ||
            !Reader.Read(CompositionOrder) || !Reader.Read(Composite) || !Reader.Read(Reserved) ||
            PlanLow == 0 || Composite > 1 ||
            Reserved[0] || Reserved[1] || Reserved[2] || Reserved[3] ||
            Reserved[4] || Reserved[5] || Reserved[6] ||
            Node == 0 || !RmlUE_IsNodeValid(View, static_cast<RmlUE_Node>(Node)))
        {
            for (const FCompiledRequest& Item : Prepared) AnimationRuntime->ReleaseBinding(Item.Binding);
            return Fail(TEXT("invalid_compiled_batch_entry"), static_cast<int32>(Index));
        }
        const uint32 Slot = PlanLow - 1;
        if (!CompiledAnimationPlans.IsValidIndex(static_cast<int32>(Slot)))
        {
            for (const FCompiledRequest& Item : Prepared) AnimationRuntime->ReleaseBinding(Item.Binding);
            return Fail(TEXT("stale_plan_handle"), static_cast<int32>(Index));
        }
        const FCompiledAnimationPlan& Plan = CompiledAnimationPlans[Slot];
        if (!Plan.bActive || Plan.Generation != PlanGeneration || Plan.Definition == 0)
        {
            for (const FCompiledRequest& Item : Prepared) AnimationRuntime->ReleaseBinding(Item.Binding);
            return Fail(TEXT("stale_plan_handle"), static_cast<int32>(Index));
        }
        const uint64 Target = (static_cast<uint64>(Node) << 8) | Plan.Property;
        if (Composite == 1)
        {
            if (NormalTargets.Contains(Target) ||
                LayeredOrdersByTarget.FindOrAdd(Target).Contains(CompositionOrder))
            {
                for (const FCompiledRequest& Item : Prepared) AnimationRuntime->ReleaseBinding(Item.Binding);
                return Fail(TEXT("duplicate_batch_contribution_order"), static_cast<int32>(Index));
            }
            LayeredOrdersByTarget.FindChecked(Target).Add(CompositionOrder);
        }
        else
        {
            if (NormalTargets.Contains(Target) || LayeredOrdersByTarget.Contains(Target))
            {
                for (const FCompiledRequest& Item : Prepared) AnimationRuntime->ReleaseBinding(Item.Binding);
                return Fail(TEXT("duplicate_batch_target_property"), static_cast<int32>(Index));
            }
            NormalTargets.Add(Target);
        }
        FCompiledRequest& Item = Prepared.AddDefaulted_GetRef();
        Item.PlanSlot = Slot;
        Item.bLayered = Composite == 1;
        Item.Contribution.Order = CompositionOrder;
        Item.Contribution.bSuppressBeforeStart =
            Plan.Fill == static_cast<uint8>(ERmlUiAnimationFillMode::None) ||
            Plan.Fill == static_cast<uint8>(ERmlUiAnimationFillMode::Forwards);
        Item.Contribution.bLayered = Item.bLayered;
        Item.Binding = AnimationRuntime->BindNode(
            FRmlUiAnimationDefinitionHandle{Plan.Definition}, View, Node);
        if (!Item.Binding.IsValid())
        {
            Prepared.Pop();
            for (const FCompiledRequest& Existing : Prepared) AnimationRuntime->ReleaseBinding(Existing.Binding);
            return Fail(TEXT("target_binding_failed"), static_cast<int32>(Index));
        }
    }

    TArray<FRmlUiAnimationBindingHandle> Bindings;
    TArray<FRmlUiAnimationContributionSpec> Contributions;
    TArray<FRmlUiAnimationCompletionCallback> OnCompletes;
    Bindings.Reserve(Prepared.Num());
    Contributions.Reserve(Prepared.Num());
    OnCompletes.Reserve(Prepared.Num());
    const TWeakObjectPtr<URmlUiJSContext> WeakThis(this);
    const TSharedRef<FAnimationBatchCommitState> CommitState = MakeShared<FAnimationBatchCommitState>();
    bool bHasLayered = false;
    for (const FCompiledRequest& Item : Prepared)
    {
        Bindings.Add(Item.Binding);
        Contributions.Add(Item.Contribution);
        OnCompletes.Add(MakeCompiledAnimationCompletionCallback(
            *AnimationRuntime, Item.Binding, WeakThis, CommitState));
        bHasLayered |= Item.bLayered;
    }

    TArray<FRmlUiAnimationHandle> Handles;
    int32 Played = 0;
    {
        FScopedRmlUiPerformanceTimer PlayTimer(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceStage::AnimationCompiledBatchPlay);
        Played = bHasLayered
            ? AnimationRuntime->PlayContributionBindings(Bindings, Contributions, Handles, MoveTemp(OnCompletes))
            : AnimationRuntime->PlayBindings(Bindings, Handles, MoveTemp(OnCompletes));
    }
    if (Played != Prepared.Num())
    {
        for (const FCompiledRequest& Item : Prepared) AnimationRuntime->ReleaseBinding(Item.Binding);
        return Fail(TEXT("playback_start_failed"));
    }
    CommitState->bCommitted = true;
    for (const FCompiledRequest& Item : Prepared)
    {
        FCompiledAnimationPlan& Plan = CompiledAnimationPlans[Item.PlanSlot];
        if (Plan.UseCount > 0)
            FRmlUiPerformance::AddWork(ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceWork::AnimationDefinitionsReused);
        ++Plan.UseCount;
    }
    return AnimationBatchResult(true, Handles);
}

FString URmlUiJSContext::ReleaseAnimationPlansPacked(const FArrayBuffer& Payload)
{
    const auto Fail = [](const FString& Message, int32 FailedIndex = INDEX_NONE, int32 Released = 0)
    {
        return AnimationPlanBatchResult(false, {}, Message, FailedIndex, Released);
    };
    if (!AnimationRuntime || !Payload.Data || Payload.Length > 64 * 1024)
        return Fail(TEXT("invalid_plan_release_payload_size"));
    FPackedAnimationReader Reader(Payload);
    uint32 Count = 0;
    constexpr uint32 Magic = 0x31524152; // RAR1
    if (!ReadPackedHeader(Reader, Magic, Count) || Reader.Remaining() != static_cast<uint64>(Count) * 8)
        return Fail(TEXT("invalid_plan_release_header"));

    TArray<uint64> Handles;
    Handles.Reserve(Count);
    TSet<uint64> UniqueHandles;
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        uint32 Low = 0, Generation = 0;
        if (!Reader.Read(Low) || !Reader.Read(Generation) || Low == 0)
            return Fail(TEXT("invalid_plan_handle"), static_cast<int32>(Index));
        const uint64 Handle = (static_cast<uint64>(Generation) << 32) | Low;
        if (UniqueHandles.Contains(Handle))
            return Fail(TEXT("duplicate_plan_handle"), static_cast<int32>(Index));
        const uint32 Slot = Low - 1;
        if (!CompiledAnimationPlans.IsValidIndex(static_cast<int32>(Slot)) ||
            !CompiledAnimationPlans[Slot].bActive ||
            CompiledAnimationPlans[Slot].Generation != Generation)
            return Fail(TEXT("stale_plan_handle"), static_cast<int32>(Index));
        UniqueHandles.Add(Handle);
        Handles.Add(Handle);
    }

    int32 Released = 0;
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        const uint32 Slot = static_cast<uint32>(Handles[Index]) - 1;
        FCompiledAnimationPlan& Plan = CompiledAnimationPlans[Slot];
        if (!AnimationRuntime->ReleaseDefinition(FRmlUiAnimationDefinitionHandle{Plan.Definition}))
            return Fail(TEXT("plan_is_in_use"), Index, Released);
        CompiledAnimationPlanAllocatedBytes -= FMath::Min(
            CompiledAnimationPlanAllocatedBytes, Plan.AllocatedBytes);
        Plan.Definition = 0;
        Plan.UseCount = 0;
        Plan.AllocatedBytes = 0;
        Plan.Property = 0;
        Plan.CostClass = 0;
        Plan.bActive = false;
        ++Plan.Generation;
        if (Plan.Generation == 0) Plan.Generation = 1;
        FreeCompiledAnimationPlanSlots.Add(Slot);
        ++Released;
    }
    return AnimationPlanBatchResult(true, {}, FString(), INDEX_NONE, Released);
}

FString URmlUiJSContext::GetAnimationPlanCacheStats() const
{
    int32 ActivePlans = 0;
    int32 VisualPlans = 0;
    int32 LayoutPositionPlans = 0;
    int32 LayoutSizePlans = 0;
    int32 VisualDiscretePlans = 0;
    int32 PaintPlans = 0;
    for (const FCompiledAnimationPlan& Plan : CompiledAnimationPlans)
    {
        if (!Plan.bActive) continue;
        ++ActivePlans;
        switch (static_cast<ERmlUiAnimationCostClass>(Plan.CostClass))
        {
        case ERmlUiAnimationCostClass::Visual: ++VisualPlans; break;
        case ERmlUiAnimationCostClass::LayoutPosition: ++LayoutPositionPlans; break;
        case ERmlUiAnimationCostClass::LayoutSize: ++LayoutSizePlans; break;
        case ERmlUiAnimationCostClass::VisualDiscrete: ++VisualDiscretePlans; break;
        case ERmlUiAnimationCostClass::Paint: ++PaintPlans; break;
        default: break;
        }
    }
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("activePlans"), ActivePlans);
    Result->SetNumberField(TEXT("allocatedBytes"), static_cast<double>(CompiledAnimationPlanAllocatedBytes));
    Result->SetNumberField(TEXT("residentLimitBytes"), static_cast<double>(
        FMath::Max<int64>(CompiledAnimationPlanResidentLimitBytes, 1024)));
    Result->SetNumberField(TEXT("slotCapacity"), CompiledAnimationPlans.Max());
    TSharedRef<FJsonObject> PlansByCost = MakeShared<FJsonObject>();
    PlansByCost->SetNumberField(TEXT("visual"), VisualPlans);
    PlansByCost->SetNumberField(TEXT("layoutPosition"), LayoutPositionPlans);
    PlansByCost->SetNumberField(TEXT("layoutSize"), LayoutSizePlans);
    PlansByCost->SetNumberField(TEXT("visualDiscrete"), VisualDiscretePlans);
    PlansByCost->SetNumberField(TEXT("paint"), PaintPlans);
    Result->SetObjectField(TEXT("plansByCost"), PlansByCost);
    return JsonString(Result);
}

FString URmlUiJSContext::GetAnimationRuntimeStats() const
{
    const FRmlUiAnimationViewActivity Activity = AnimationRuntime && View
        ? AnimationRuntime->GetViewActivity(View)
        : FRmlUiAnimationViewActivity{};
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("activeTracks"), Activity.Total);
    Result->SetNumberField(TEXT("visual"), Activity.Visual);
    Result->SetNumberField(TEXT("layoutPosition"), Activity.LayoutPosition);
    Result->SetNumberField(TEXT("layoutSize"), Activity.LayoutSize);
    Result->SetNumberField(TEXT("visualDiscrete"), Activity.VisualDiscrete);
    Result->SetNumberField(TEXT("paint"), Activity.Paint);
    return JsonString(Result);
}

FString URmlUiJSContext::ControlAnimation(
    const FString& HandleText, const FString& CommandText, double Value)
{
    uint64 HandleValue = 0;
    const FString TrimmedHandle = HandleText.TrimStartAndEnd();
    if (!LexTryParseString(HandleValue, *TrimmedHandle) || HandleValue == 0)
    {
        return AnimationResult(false, TEXT("rejected"), 0, TEXT("rejected"),
            TEXT("invalid_handle"));
    }
    FRmlUiAnimationRuntime& Runtime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
    const FRmlUiAnimationHandle Handle{HandleValue};
    if (!Runtime.IsActive(Handle))
    {
        return AnimationResult(false, TEXT("rejected"), HandleValue, TEXT("stale"),
            TEXT("stale_handle"));
    }
    const FString Command = CommandText.TrimStartAndEnd().ToLower();
    bool bAccepted = false;
    FString State;
    if (Command == TEXT("pause"))
    {
        bAccepted = Runtime.Pause(Handle);
        State = TEXT("paused");
    }
    else if (Command == TEXT("play") || Command == TEXT("resume"))
    {
        bAccepted = Runtime.Resume(Handle);
        State = TEXT("running");
    }
    else if (Command == TEXT("seek"))
    {
        bAccepted = Runtime.Seek(Handle, Value);
        State = Runtime.IsPaused(Handle) ? TEXT("paused") : TEXT("running");
    }
    else if (Command == TEXT("setplaybackrate"))
    {
        bAccepted = Runtime.SetPlaybackRate(Handle, Value);
        State = Runtime.IsPaused(Handle) ? TEXT("paused") : TEXT("running");
    }
    else if (Command == TEXT("cancel"))
    {
        bAccepted = Runtime.Cancel(Handle);
        State = TEXT("cancelled");
    }
    else if (Command == TEXT("status"))
    {
        bAccepted = true;
        State = Runtime.IsPaused(Handle) ? TEXT("paused") : TEXT("running");
    }
    else
    {
        return AnimationResult(false, TEXT("rejected"), HandleValue, TEXT("rejected"),
            TEXT("unsupported_command"));
    }
    return bAccepted
        ? AnimationResult(true, TEXT("native"), HandleValue, State)
        : AnimationResult(false, TEXT("rejected"), HandleValue, TEXT("rejected"),
            TEXT("control_failed"));
}

FString URmlUiJSContext::ApplyNodePropertyBatch(const FString& UpdatesJson)
{
    const auto Fail = [](int32 Applied, const FString& Error)
    {
        return PropertyBatchResult(false, Applied, Error);
    };
    if (!View || UpdatesJson.Len() > 1024 * 1024)
        return Fail(0, TEXT("invalid_target_or_payload_size"));

    TArray<TSharedPtr<FJsonValue>> Input;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(UpdatesJson), Input) ||
        Input.IsEmpty() || Input.Num() > 65536)
        return Fail(0, TEXT("invalid_property_batch"));

    struct FUpdate
    {
        int32 Node = 0;
        FString Property;
        FString Value;
        bool bRemove = false;
    };
    TArray<FUpdate> Updates;
    Updates.Reserve(Input.Num());
    TArray<int32> StaleIndices;
    for (int32 Index = 0; Index < Input.Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Value = Input[Index];
        if (!Value || Value->Type != EJson::Object)
            return Fail(0, TEXT("property_updates_must_be_objects"));
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        double NodeNumber = 0.0;
        FUpdate Update;
        if (!Object->TryGetNumberField(TEXT("node"), NodeNumber) ||
            NodeNumber < 1.0 || NodeNumber > MAX_int32 ||
            FMath::FloorToDouble(NodeNumber) != NodeNumber ||
            !Object->TryGetStringField(TEXT("property"), Update.Property) ||
            Update.Property.IsEmpty() || Update.Property.Len() > 256 ||
            (Object->HasField(TEXT("remove")) &&
                !Object->TryGetBoolField(TEXT("remove"), Update.bRemove)) ||
            (!Update.bRemove && !Object->TryGetStringField(TEXT("value"), Update.Value)) ||
            Update.Value.Len() > 4096)
        {
            return Fail(0, TEXT("invalid_property_update_shape"));
        }
        Update.Node = static_cast<int32>(NodeNumber);
        if (!RmlUE_IsNodeValid(View, Update.Node))
        {
            StaleIndices.Add(Index);
            continue;
        }
        Updates.Add(MoveTemp(Update));
    }
    if (!StaleIndices.IsEmpty())
        return PropertyBatchResult(false, 0, TEXT("stale_property_target"), &StaleIndices);

    CountNodeCall();
    int32 Applied = 0;
    for (const FUpdate& Update : Updates)
    {
        if (!RmlUE_SetNodeProperty(View, Update.Node, TCHAR_TO_UTF8(*Update.Property),
            Update.bRemove ? nullptr : TCHAR_TO_UTF8(*Update.Value)))
        {
            return Fail(Applied, TEXT("property_apply_failed"));
        }
        ++Applied;
    }
    return PropertyBatchResult(true, Applied);
}
bool URmlUiJSContext::CancelAnimation(int32 Node, const FString& Property)
{
    if (!View) return false;
    bool bCancelledEcs = false;
    if (Property.Equals(TEXT("opacity"), ESearchCase::IgnoreCase))
        bCancelledEcs = FRmlUiUnrealModule::Get().GetAnimationRuntime().CancelNodeAnimation(
            View, static_cast<uint32>(Node), ERmlUiAnimatedProperty::Opacity);
    else if (Property.Equals(TEXT("transform"), ESearchCase::IgnoreCase))
        bCancelledEcs = FRmlUiUnrealModule::Get().GetAnimationRuntime().CancelNodeAnimation(
            View, static_cast<uint32>(Node), ERmlUiAnimatedProperty::Transform2D);
    const bool bCancelledNative = RmlUE_CancelAnimation(View, Node, TCHAR_TO_UTF8(*Property)) != 0;
    return bCancelledEcs || Result(bCancelledNative);
}
void URmlUiJSContext::QueueHostEvent(const FString& Json)
{
    if (bDisposed) return;
    PendingHostEvents.Add(Json);
    NotifyWake();
}
void URmlUiJSContext::QueueAnimationEvent(uint64 Handle, uint8 ReasonValue)
{
    if (bDisposed) return;
    const ERmlUiAnimationCompletionReason Reason =
        static_cast<ERmlUiAnimationCompletionReason>(ReasonValue);
    if (!OnAnimationEventBatchPacked.IsBound() && !OnAnimationEventBatch.IsBound())
    {
        TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
        Event->SetStringField(TEXT("handle"), LexToString(Handle));
        Event->SetStringField(TEXT("reason"), CompletionReasonText(Reason));
        Event->SetStringField(TEXT("state"), Reason == ERmlUiAnimationCompletionReason::Completed
            ? TEXT("finished") : CompletionReasonText(Reason));
        OnAnimationEvent.Broadcast(JsonString(Event));
        return;
    }

    PendingAnimationEvents.Add({Handle, ReasonValue});
    if (!AnimationRuntime || !AnimationRuntime->IsAdvancing())
    {
        FlushAnimationEvents();
    }
}
void URmlUiJSContext::FlushAnimationEvents()
{
    if (bDisposed ||
        (!OnAnimationEventBatchPacked.IsBound() && !OnAnimationEventBatch.IsBound()))
    {
        PendingAnimationEvents.Reset();
        return;
    }
    while (!PendingAnimationEvents.IsEmpty())
    {
        TArray<FPendingAnimationEvent> Events = MoveTemp(PendingAnimationEvents);
        PendingAnimationEvents.Reset();
        FString Json;
        TArray<uint8> PackedPayload;
        if (OnAnimationEventBatchPacked.IsBound())
        {
            const uint64 PackStart =
                FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
            constexpr uint32 PackedEventMagic = 0x31454152; // "RAE1" in little-endian byte order.
            constexpr int32 PackedEventHeaderSize = 8;
            constexpr int32 PackedEventStride = 12;
            PackedPayload.SetNumZeroed(
                PackedEventHeaderSize + Events.Num() * PackedEventStride);
            const uint32 PackedEventCount = static_cast<uint32>(Events.Num());
            FMemory::Memcpy(PackedPayload.GetData(), &PackedEventMagic, sizeof(PackedEventMagic));
            FMemory::Memcpy(PackedPayload.GetData() + sizeof(PackedEventMagic),
                &PackedEventCount, sizeof(PackedEventCount));
            for (int32 Index = 0; Index < Events.Num(); ++Index)
            {
                const uint32 HandleLow = static_cast<uint32>(Events[Index].Handle);
                const uint32 HandleHigh = static_cast<uint32>(Events[Index].Handle >> 32);
                uint8* Destination = PackedPayload.GetData() + PackedEventHeaderSize +
                    Index * PackedEventStride;
                FMemory::Memcpy(Destination, &HandleLow, sizeof(HandleLow));
                FMemory::Memcpy(Destination + sizeof(HandleLow), &HandleHigh, sizeof(HandleHigh));
                Destination[8] = Events[Index].Reason;
            }
            if (PackStart)
            {
                FRmlUiPerformance::AddCycles(
                    ERmlUiPerformanceBackend::Unattributed,
                    ERmlUiPerformanceStage::AnimationCompletionEventPack,
                    FPlatformTime::Cycles64() - PackStart);
            }
        }
        else
        {
            const uint64 SerializeStart =
                FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
            Json.Reserve(Events.Num() * 64 + 2);
            Json.AppendChar(TEXT('['));
            for (int32 Index = 0; Index < Events.Num(); ++Index)
            {
                if (Index != 0) Json.AppendChar(TEXT(','));
                const ERmlUiAnimationCompletionReason Reason =
                    static_cast<ERmlUiAnimationCompletionReason>(Events[Index].Reason);
                const TCHAR* ReasonText = CompletionReasonLiteral(Reason);
                Json.Append(TEXT("{\"handle\":\""));
                Json.Appendf(TEXT("%llu"),
                    static_cast<unsigned long long>(Events[Index].Handle));
                Json.Append(TEXT("\",\"reason\":\""));
                Json.Append(ReasonText);
                Json.Append(TEXT("\",\"state\":\""));
                Json.Append(Reason == ERmlUiAnimationCompletionReason::Completed
                    ? TEXT("finished") : ReasonText);
                Json.Append(TEXT("\"}"));
            }
            Json.AppendChar(TEXT(']'));
            if (SerializeStart)
            {
                FRmlUiPerformance::AddCycles(
                    ERmlUiPerformanceBackend::Unattributed,
                    ERmlUiPerformanceStage::AnimationCompletionEventSerialize,
                    FPlatformTime::Cycles64() - SerializeStart);
            }
        }
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCompletionEventBatches);
        FRmlUiPerformance::AddWork(
            ERmlUiPerformanceBackend::Unattributed,
            ERmlUiPerformanceWork::AnimationCompletionEvents,
            Events.Num());
        const uint64 DispatchStart =
            FRmlUiPerformance::IsEnabled() ? FPlatformTime::Cycles64() : 0;
        if (OnAnimationEventBatchPacked.IsBound())
        {
            FArrayBuffer Payload;
            Payload.Data = PackedPayload.GetData();
            Payload.Length = PackedPayload.Num();
            Payload.bCopy = true;
            OnAnimationEventBatchPacked.Broadcast(Payload);
        }
        else
            OnAnimationEventBatch.Broadcast(Json);
        if (DispatchStart)
        {
            FRmlUiPerformance::AddCycles(
                ERmlUiPerformanceBackend::Unattributed,
                ERmlUiPerformanceStage::AnimationCompletionEventDispatch,
                FPlatformTime::Cycles64() - DispatchStart);
        }
    }
}
FString URmlUiJSContext::CaptureState()
{
    if (!bDisposed) OnLifecycle.Broadcast(TEXT("serialize"));
    return StateJson;
}
void URmlUiJSContext::ReleaseAllAnimationPlans()
{
    if (!AnimationRuntime) return;
    for (FCompiledAnimationPlan& Plan : CompiledAnimationPlans)
    {
        if (Plan.bActive && Plan.Definition != 0)
            AnimationRuntime->ReleaseDefinition(FRmlUiAnimationDefinitionHandle{Plan.Definition});
        Plan.Definition = 0;
        Plan.UseCount = 0;
        Plan.AllocatedBytes = 0;
        Plan.Property = 0;
        Plan.CostClass = 0;
        Plan.bActive = false;
    }
    CompiledAnimationPlans.Empty();
    FreeCompiledAnimationPlanSlots.Empty();
    CompiledAnimationPlanAllocatedBytes = 0;
}
void URmlUiJSContext::Dispose()
{
    if (bDisposed) return;
    OnLifecycle.Broadcast(TEXT("dispose"));
    bDisposed = true;
    if (View)
    {
        FRmlUiUnrealModule::Get().GetAnimationRuntime().CancelViewAnimations(View);
        RmlUE_SetNodeEventCallback(View, nullptr, nullptr);
        RmlUE_SetLayoutCallback(View, nullptr, nullptr);
    }
    ReleaseAllAnimationPlans();
    OnAfterLayout.Clear();
    OnNativeEvent.Clear(); OnFrame.Clear(); OnLifecycle.Clear(); OnHostResponse.Clear(); OnHostRequest.Clear();
    OnAnimationEvent.Clear();
    OnAnimationEventBatch.Clear();
    OnAnimationEventBatchPacked.Clear();
    PendingAnimationEvents.Empty();
    if (AnimationRuntime && AnimationPostAdvanceHandle.IsValid())
    {
        AnimationRuntime->RemovePostAdvanceCallback(AnimationPostAdvanceHandle);
    }
    AnimationPostAdvanceHandle.Reset();
    AnimationRuntime = nullptr;
    PendingResponses.Empty();
    PendingHostRequests.Empty();
    OnHostEvent.Clear(); PendingHostEvents.Empty();
    WakeCallback = {};
    CssAnimationRestartCallback = {};
    bAnimationFramePending = false;
    NextTimerWakeTime = TNumericLimits<double>::Max();
    Environment.Reset();
    View = nullptr;
}
void URmlUiJSContext::BeginDestroy() { Dispose(); Super::BeginDestroy(); }
