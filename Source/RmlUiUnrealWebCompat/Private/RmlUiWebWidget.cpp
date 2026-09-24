#include "RmlUiWebWidget.h"

#include "RmlUiCssAnimationSession.h"
#include "RmlUiWebCompatModule.h"
#include "RmlUiAnimationRuntime.h"
#include "RmlUiBridge.h"
#include "RmlUiUnrealModule.h"
#include "SRmlUiWidget.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool ReadEasing(const TArray<TSharedPtr<FJsonValue>>& Values, FRmlUiAnimationEasing& Out)
{
    if (Values.Num() != 5) return false;
    double Number[5]{};
    for (int32 Index = 0; Index < 5; ++Index)
        if (!Values[Index].IsValid() || !Values[Index]->TryGetNumber(Number[Index]) || !FMath::IsFinite(Number[Index])) return false;
    const int32 Type = static_cast<int32>(Number[0]);
    if (Type < 0 || Type > 2 || Number[0] != Type) return false;
    Out.Type = static_cast<ERmlUiAnimationEasingType>(Type);
    if (Out.Type == ERmlUiAnimationEasingType::CubicBezier)
    {
        Out.X1 = Number[1]; Out.Y1 = Number[2]; Out.X2 = Number[3]; Out.Y2 = Number[4];
        return Out.X1 >= 0.f && Out.X1 <= 1.f && Out.X2 >= 0.f && Out.X2 <= 1.f;
    }
    if (Out.Type == ERmlUiAnimationEasingType::Steps)
    {
        Out.StepCount = static_cast<int32>(Number[1]);
        Out.StepPosition = static_cast<ERmlUiAnimationStepPosition>(static_cast<int32>(Number[2]));
        return Number[1] == Out.StepCount && Out.StepCount >= 1 && Number[2] == static_cast<int32>(Out.StepPosition) &&
            static_cast<int32>(Out.StepPosition) >= 0 && static_cast<int32>(Out.StepPosition) <= 3 &&
            (Out.StepPosition != ERmlUiAnimationStepPosition::JumpNone || Out.StepCount >= 2);
    }
    return true;
}

bool ReadNumbers(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 Count, TArray<double>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values || Values->Num() != Count) return false;
    Out.SetNumUninitialized(Count);
    for (int32 Index = 0; Index < Count; ++Index)
        if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetNumber(Out[Index]) || !FMath::IsFinite(Out[Index])) return false;
    return true;
}

bool ReadFrameHeader(const TSharedPtr<FJsonObject>& Frame, float& Offset, FRmlUiAnimationEasing& Easing)
{
    double Number = 0.0;
    const TArray<TSharedPtr<FJsonValue>>* EasingValues = nullptr;
    if (!Frame.IsValid() || !Frame->TryGetNumberField(TEXT("offset"), Number) || !FMath::IsFinite(Number) ||
        Number < 0.0 || Number > 1.0 || !Frame->TryGetArrayField(TEXT("easing"), EasingValues) || !EasingValues ||
        !ReadEasing(*EasingValues, Easing)) return false;
    Offset = static_cast<float>(Number);
    return true;
}
}

class FRmlUiCssAnimationSession final : public IRmlUiCssAnimationSession
{
    struct FNodeActivation
    {
        TArray<FRmlUiAnimationBindingHandle> Bindings;
        TArray<FRmlUiAnimationHandle> Animations;
    };

    struct FLifecycleEvent
    {
        RmlUE_Node Node = 0;
        FString Name;
        FString Type;
        int64 Iteration = 0;
    };

    struct FLifecycleGroup
    {
        int32 RemainingTracks = 0;
        bool bTerminalQueued = false;
    };

    struct FRuleState
    {
        FString Selector;
        FString Name;
        bool bPaused = false;
        bool bRequiredOnLoad = false;
        bool bDependsOnAncestor = false;
        uint64 Matched = 0;
        uint64 Bound = 0;
        uint64 Played = 0;
        uint64 Started = 0;
        uint64 LastMutationRevision = 0;
        TArray<FRmlUiAnimationDefinitionHandle> Definitions;
        TArray<int32> PropertyIds;
        TMap<RmlUE_Node, FNodeActivation> ActiveNodes;
    };

    struct FPlayStateRule
    {
        FString Selector;
        bool bPaused = false;
    };

public:
    ~FRmlUiCssAnimationSession() { Reset(); }

    virtual void Reset() override
    {
        if (View) RmlUE_SetNodeMutationCallback(View, nullptr, nullptr);
        if (FModuleManager::Get().IsModuleLoaded(TEXT("RmlUiUnreal")))
        {
            FRmlUiAnimationRuntime& Runtime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
            if (PostAdvanceHandle.IsValid()) Runtime.RemovePostAdvanceCallback(PostAdvanceHandle);
            for (FRuleState& Rule : RuleStates)
            {
                for (TPair<RmlUE_Node, FNodeActivation>& Pair : Rule.ActiveNodes)
                    ReleaseActivation(Runtime, Pair.Value);
                for (const FRmlUiAnimationDefinitionHandle Handle : Rule.Definitions)
                    Runtime.ReleaseDefinition(Handle);
            }
        }
        PostAdvanceHandle.Reset(); PendingLifecycleEvents.Reset(); RuleStates.Reset(); PlayStateRules.Reset();
        DirtyNodes.Reset(); DirtySubtrees.Reset(); DirtySelectorContexts.Reset(); RestartNodes.Reset();
        bHasStructuralMutation = false; bHasRelationalPlayStateSelector = false; bNeedsInitialScan = false; View = nullptr;
        MutationRevision = 0;
    }

    virtual void SetWakeCallback(TFunction<void()> Callback) override
    {
        WakeCallback = MoveTemp(Callback);
    }

    virtual void SetLifecycleEventBatchCallback(TFunction<void(const FString&)> Callback) override
    {
        LifecycleEventBatchCallback = MoveTemp(Callback);
    }

    virtual bool Install(const FString& Json, RmlUE_View* InView, FString& OutError) override
    {
        Reset();
        if (Json.IsEmpty()) return true;
        TSharedPtr<FJsonObject> Root;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
        { OutError = TEXT("Motion Manifest is not valid JSON."); return false; }
        double SchemaVersion = 0.0;
        const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
        if (!Root->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion) || SchemaVersion != 1.0 ||
            !Root->TryGetArrayField(TEXT("rules"), Rules) || !Rules)
        { OutError = TEXT("Motion Manifest schemaVersion must be 1 and rules must be an array."); return false; }
        View = InView;
        const TArray<TSharedPtr<FJsonValue>>* PlayStates = nullptr;
        if (Root->TryGetArrayField(TEXT("playStates"), PlayStates) && PlayStates)
        {
            for (const TSharedPtr<FJsonValue>& Value : *PlayStates)
            {
                const TSharedPtr<FJsonObject>* Object = nullptr;
                FPlayStateRule State;
                if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid() ||
                    !(*Object)->TryGetStringField(TEXT("selector"), State.Selector) || State.Selector.IsEmpty() ||
                    !(*Object)->TryGetBoolField(TEXT("paused"), State.bPaused))
                { OutError = TEXT("Motion Manifest play-state rule is invalid."); Reset(); return false; }
                PlayStateRules.Add(MoveTemp(State));
                bHasRelationalPlayStateSelector |= SelectorDependsOnAncestor(PlayStateRules.Last().Selector);
            }
        }
        FRmlUiAnimationRuntime& Runtime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
        for (const TSharedPtr<FJsonValue>& RuleValue : *Rules)
        {
            const TSharedPtr<FJsonObject>* RulePointer = nullptr;
            if (!RuleValue.IsValid() || !RuleValue->TryGetObject(RulePointer) || !RulePointer || !RulePointer->IsValid())
            { OutError = TEXT("Motion Manifest rule must be an object."); Reset(); return false; }
            const TSharedPtr<FJsonObject>& Rule = *RulePointer;
            FString Selector, Name;
            double Duration = 0.0, Delay = 0.0, IterationsNumber = 0.0, DirectionNumber = 0.0, FillNumber = 0.0;
            bool bPaused = false;
            const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
            if (!Rule->TryGetStringField(TEXT("selector"), Selector) || Selector.IsEmpty() ||
                !Rule->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty() ||
                !Rule->TryGetNumberField(TEXT("duration"), Duration) || Duration < 0.0 ||
                !Rule->TryGetNumberField(TEXT("delay"), Delay) ||
                !Rule->TryGetNumberField(TEXT("iterations"), IterationsNumber) ||
                !Rule->TryGetNumberField(TEXT("direction"), DirectionNumber) ||
                !Rule->TryGetNumberField(TEXT("fill"), FillNumber) ||
                !Rule->TryGetBoolField(TEXT("paused"), bPaused) ||
                !Rule->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks || Tracks->IsEmpty() ||
                !FMath::IsFinite(Duration) || !FMath::IsFinite(Delay))
            { OutError = TEXT("Motion Manifest rule has invalid timing fields."); Reset(); return false; }
            const int32 Iterations = static_cast<int32>(IterationsNumber);
            const int32 Direction = static_cast<int32>(DirectionNumber);
            const int32 Fill = static_cast<int32>(FillNumber);
            if (IterationsNumber != Iterations || Iterations < 0 || DirectionNumber != Direction || Direction < 0 || Direction > 3 ||
                FillNumber != Fill || Fill < 0 || Fill > 3)
            { OutError = TEXT("Motion Manifest rule has an invalid iteration, direction, or fill value."); Reset(); return false; }

            FRuleState RuleState;
            RuleState.Selector = Selector;
            RuleState.Name = Name;
            RuleState.bPaused = bPaused;
            RuleState.bDependsOnAncestor = SelectorDependsOnAncestor(Selector);
            if (Rule->HasField(TEXT("requiredOnLoad")) &&
                !Rule->TryGetBoolField(TEXT("requiredOnLoad"), RuleState.bRequiredOnLoad))
            { OutError = TEXT("Motion Manifest requiredOnLoad must be a boolean."); Reset(); return false; }
            for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
            {
                const TSharedPtr<FJsonObject>* TrackPointer = nullptr;
                if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackPointer) || !TrackPointer || !TrackPointer->IsValid())
                { OutError = TEXT("Motion Manifest track must be an object."); Reset(); return false; }
                const TSharedPtr<FJsonObject>& Track = *TrackPointer;
                double PropertyNumber = 0.0;
                const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
                if (!Track->TryGetNumberField(TEXT("propertyId"), PropertyNumber) || !Track->TryGetArrayField(TEXT("keyframes"), Frames) ||
                    !Frames || Frames->Num() < 2 || Frames->Num() > 4096)
                { OutError = TEXT("Motion Manifest track is invalid."); Reset(); return false; }
                const int32 PropertyId = static_cast<int32>(PropertyNumber);
                if (PropertyNumber != PropertyId || PropertyId < 1 || PropertyId > 13)
                { OutError = TEXT("Motion Manifest property id is invalid."); Reset(); return false; }
                const ERmlUiAnimatedProperty Property = static_cast<ERmlUiAnimatedProperty>(PropertyId);
                FRmlUiAnimationDefinitionHandle Definition;
                if (Property == ERmlUiAnimatedProperty::Transform2D)
                {
                    FRmlUiTransform2DAnimationDefinition Desc;
                    Desc.DurationSeconds = Duration; Desc.DelaySeconds = Delay; Desc.Iterations = Iterations;
                    Desc.Direction = static_cast<ERmlUiAnimationDirection>(Direction); Desc.Fill = static_cast<ERmlUiAnimationFillMode>(Fill);
                    for (const TSharedPtr<FJsonValue>& FrameValue : *Frames)
                    {
                        const TSharedPtr<FJsonObject>* FramePointer = nullptr; TArray<double> Values;
                        FRmlUiTransform2DAnimationKeyframe Frame;
                        if (!FrameValue.IsValid() || !FrameValue->TryGetObject(FramePointer) || !FramePointer ||
                            !ReadFrameHeader(*FramePointer, Frame.Offset, Frame.EasingToNext) || !ReadNumbers(*FramePointer, TEXT("values"), 7, Values))
                        { OutError = TEXT("Motion Manifest transform keyframe is invalid."); Reset(); return false; }
                        Frame.Value = {static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]),
                            static_cast<float>(Values[3]), static_cast<float>(Values[4]), static_cast<float>(Values[5]), static_cast<float>(Values[6])};
                        Desc.Keyframes.Add(Frame);
                    }
                    Definition = Runtime.RegisterTransform2DDefinition(Desc);
                }
                else if (Property >= ERmlUiAnimatedProperty::Color)
                {
                    FRmlUiColorAnimationDefinition Desc;
                    Desc.DurationSeconds = Duration; Desc.DelaySeconds = Delay; Desc.Iterations = Iterations;
                    Desc.Direction = static_cast<ERmlUiAnimationDirection>(Direction); Desc.Fill = static_cast<ERmlUiAnimationFillMode>(Fill);
                    for (const TSharedPtr<FJsonValue>& FrameValue : *Frames)
                    {
                        const TSharedPtr<FJsonObject>* FramePointer = nullptr; TArray<double> Values;
                        FRmlUiColorAnimationKeyframe Frame;
                        if (!FrameValue.IsValid() || !FrameValue->TryGetObject(FramePointer) || !FramePointer ||
                            !ReadFrameHeader(*FramePointer, Frame.Offset, Frame.EasingToNext) || !ReadNumbers(*FramePointer, TEXT("values"), 4, Values) ||
                            Values.ContainsByPredicate([](double Value) { return Value < 0.0 || Value > 1.0; }))
                        { OutError = TEXT("Motion Manifest color keyframe is invalid."); Reset(); return false; }
                        Frame.Value = {static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), static_cast<float>(Values[3])};
                        Desc.Keyframes.Add(Frame);
                    }
                    Definition = Runtime.RegisterColorDefinition(Property, Desc);
                }
                else
                {
                    FRmlUiFloatAnimationDefinition Desc;
                    Desc.DurationSeconds = Duration; Desc.DelaySeconds = Delay; Desc.Iterations = Iterations;
                    Desc.Direction = static_cast<ERmlUiAnimationDirection>(Direction); Desc.Fill = static_cast<ERmlUiAnimationFillMode>(Fill);
                    for (const TSharedPtr<FJsonValue>& FrameValue : *Frames)
                    {
                        const TSharedPtr<FJsonObject>* FramePointer = nullptr; TArray<double> Values;
                        FRmlUiFloatAnimationKeyframe Frame;
                        if (!FrameValue.IsValid() || !FrameValue->TryGetObject(FramePointer) || !FramePointer)
                        { OutError = TEXT("Motion Manifest scalar keyframe is not an object."); Reset(); return false; }
                        if (!ReadFrameHeader(*FramePointer, Frame.Offset, Frame.EasingToNext))
                        { OutError = TEXT("Motion Manifest scalar keyframe has an invalid offset or easing."); Reset(); return false; }
                        if (!ReadNumbers(*FramePointer, TEXT("values"), 1, Values))
                        { OutError = TEXT("Motion Manifest scalar keyframe values must contain one finite number."); Reset(); return false; }
                        if ((Property == ERmlUiAnimatedProperty::Opacity && (Values[0] < 0.0 || Values[0] > 1.0)) ||
                            ((Property == ERmlUiAnimatedProperty::WidthPx || Property == ERmlUiAnimatedProperty::HeightPx) && Values[0] < 0.0))
                        { OutError = TEXT("Motion Manifest scalar keyframe is outside the property range."); Reset(); return false; }
                        Frame.Value = static_cast<float>(Values[0]); Desc.Keyframes.Add(Frame);
                    }
                    Definition = Runtime.RegisterFloatDefinition(Property, Desc);
                }
                if (!Definition.IsValid())
                {
                    OutError = FString::Printf(TEXT("Motion Manifest definition rejected: animation=%s selector=%s propertyId=%d"),
                        *Name, *Selector, PropertyId);
                    Reset(); return false;
                }
                RuleState.Definitions.Add(Definition);
                RuleState.PropertyIds.Add(PropertyId);
            }
            RuleStates.Add(MoveTemp(RuleState));
            FRuleState& InstalledRule = RuleStates.Last();
            const RmlUE_Node RootNode = RmlUE_GetRootNode(View);
            const int32 NodeCount = RootNode
                ? RmlUE_QueryNodes(View, RootNode, TCHAR_TO_UTF8(*Selector), nullptr, 0)
                : -1;
            if (NodeCount < 0 || NodeCount > 65536)
            { OutError = FString::Printf(TEXT("Motion Manifest selector is invalid or oversized: %s"), *Selector); Reset(); return false; }
            TArray<RmlUE_Node> Nodes; Nodes.SetNumUninitialized(NodeCount);
            if (NodeCount > 0 && RmlUE_QueryNodes(View, RootNode, TCHAR_TO_UTF8(*Selector), Nodes.GetData(), NodeCount) != NodeCount)
            { OutError = FString::Printf(TEXT("Motion Manifest selector changed while binding: %s"), *Selector); Reset(); return false; }
            for (const RmlUE_Node Node : Nodes)
                if (!ActivateNode(Runtime, InstalledRule, Node, OutError)) { Reset(); return false; }
        }
        RmlUE_SetNodeMutationCallback(View, &FRmlUiCssAnimationSession::NodeMutated, this);
        PostAdvanceHandle = Runtime.AddPostAdvanceCallback(
            FSimpleDelegate::CreateRaw(this, &FRmlUiCssAnimationSession::FlushLifecycleEvents));
        // Reconcile one initial style boundary even if a caller changed the
        // tree without the mutation callback before the first flush.
        bNeedsInitialScan = true;
        return true;
    }

    void NotifyNodeMutation(uint32 Node, bool bIncludeSubtree, bool bSelectorContext)
    {
        if (!View || !Node) return;
        ++MutationRevision;
        const bool bNeedsWake = DirtyNodes.IsEmpty() && DirtySubtrees.IsEmpty();
        DirtyNodes.Add(Node);
        if (bIncludeSubtree)
        {
            DirtySubtrees.Add(Node);
            bHasStructuralMutation = true;
        }
        if (bSelectorContext) DirtySelectorContexts.Add(Node);
        if (bNeedsWake && WakeCallback) WakeCallback();
    }

    virtual bool RequestRestart(RmlUE_Node Node) override
    {
        if (!View || !RmlUE_IsNodeValid(View, Node)) return false;
        bool bActive = false;
        for (const FRuleState& Rule : RuleStates)
            bActive |= Rule.ActiveNodes.Contains(Node);
        if (!bActive) return false;
        RestartNodes.Add(Node);
        NotifyNodeMutation(Node, false, false);
        return true;
    }

    virtual bool FlushActivationChanges(FString& OutError) override
    {
        if (!View || (DirtyNodes.IsEmpty() && !bNeedsInitialScan)) return true;
        FRmlUiAnimationRuntime& Runtime = FRmlUiUnrealModule::Get().GetAnimationRuntime();
        TSet<RmlUE_Node> Pending = MoveTemp(DirtyNodes);
        TSet<RmlUE_Node> PendingSubtrees = MoveTemp(DirtySubtrees);
        TSet<RmlUE_Node> PendingSelectorContexts = MoveTemp(DirtySelectorContexts);
        const TSet<RmlUE_Node> PendingRestarts = MoveTemp(RestartNodes);
        const bool bSweepInvalidNodes = bHasStructuralMutation;
        DirtyNodes.Reset();
        DirtySubtrees.Reset();
        DirtySelectorContexts.Reset();
        RestartNodes.Reset();
        bHasStructuralMutation = false;
        if (bNeedsInitialScan)
        {
            // The full selector scan subsumes mount-time dirty nodes and subtrees.
            Pending.Reset();
            PendingSubtrees.Reset();
            PendingSelectorContexts.Reset();
            for (const FRuleState& Rule : RuleStates)
                for (const TPair<RmlUE_Node, FNodeActivation>& Pair : Rule.ActiveNodes)
                    Pending.Add(Pair.Key);
            const RmlUE_Node RootNode = RmlUE_GetRootNode(View);
            for (const FRuleState& Rule : RuleStates)
            {
                if (!RootNode) continue;
                const int32 NodeCount = RmlUE_QueryNodes(View, RootNode,
                    TCHAR_TO_UTF8(*Rule.Selector), nullptr, 0);
                if (NodeCount < 0 || NodeCount > 65536)
                {
                    OutError = FString::Printf(TEXT("Motion Manifest initial selector is invalid or oversized: %s"), *Rule.Selector);
                    return false;
                }
                TArray<RmlUE_Node> Nodes;
                Nodes.SetNumUninitialized(NodeCount);
                if (NodeCount > 0 && RmlUE_QueryNodes(View, RootNode,
                    TCHAR_TO_UTF8(*Rule.Selector), Nodes.GetData(), NodeCount) != NodeCount)
                {
                    OutError = FString::Printf(TEXT("Motion Manifest initial selector changed while activating: %s"), *Rule.Selector);
                    return false;
                }
                for (const RmlUE_Node Node : Nodes) Pending.Add(Node);
            }
            bNeedsInitialScan = false;
        }
        for (FRuleState& Rule : RuleStates)
        {
            if (bSweepInvalidNodes)
            {
                for (auto It = Rule.ActiveNodes.CreateIterator(); It; ++It)
                {
                    if (!RmlUE_IsNodeValid(View, It.Key()))
                    {
                        ReleaseActivation(Runtime, It.Value());
                        It.RemoveCurrent();
                    }
                }
            }
            TSet<RmlUE_Node> CandidateNodes = Pending;
            TSet<RmlUE_Node> RootsToScan = PendingSubtrees;
            if (Rule.bDependsOnAncestor || bHasRelationalPlayStateSelector)
                for (const RmlUE_Node RootNode : PendingSelectorContexts) RootsToScan.Add(RootNode);
            for (const RmlUE_Node RootNode : RootsToScan)
            {
                if (!RmlUE_IsNodeValid(View, RootNode)) continue;
                for (const TPair<RmlUE_Node, FNodeActivation>& Active : Rule.ActiveNodes)
                    if (RmlUE_ContainsNode(View, RootNode, Active.Key)) CandidateNodes.Add(Active.Key);
                const int32 NodeCount = RmlUE_QueryNodes(
                    View, RootNode, TCHAR_TO_UTF8(*Rule.Selector), nullptr, 0);
                if (NodeCount < 0 || NodeCount > 65536)
                {
                    OutError = FString::Printf(TEXT("Motion Manifest mutation selector is invalid or oversized: %s"), *Rule.Selector);
                    return false;
                }
                TArray<RmlUE_Node> Descendants;
                Descendants.SetNumUninitialized(NodeCount);
                if (NodeCount > 0 && RmlUE_QueryNodes(View, RootNode, TCHAR_TO_UTF8(*Rule.Selector), Descendants.GetData(), NodeCount) != NodeCount)
                {
                    OutError = FString::Printf(TEXT("Motion Manifest subtree changed while activating: %s"), *Rule.Selector);
                    return false;
                }
                for (const RmlUE_Node Descendant : Descendants) CandidateNodes.Add(Descendant);
            }
            for (const RmlUE_Node Node : CandidateNodes)
            {
                const bool bStillMatches = RmlUE_IsNodeValid(View, Node) &&
                    RmlUE_MatchesNode(View, Node, TCHAR_TO_UTF8(*Rule.Selector));
                if (bStillMatches && !PendingRestarts.Contains(Node)) continue;
                FNodeActivation Activation;
                if (Rule.ActiveNodes.RemoveAndCopyValue(Node, Activation)) ReleaseActivation(Runtime, Activation);
            }
            for (const RmlUE_Node Node : CandidateNodes)
            {
                if (RmlUE_IsNodeValid(View, Node) && !Rule.ActiveNodes.Contains(Node) &&
                    RmlUE_MatchesNode(View, Node, TCHAR_TO_UTF8(*Rule.Selector)) &&
                    !ActivateNode(Runtime, Rule, Node, OutError))
                    return false;
            }
            for (const RmlUE_Node Node : CandidateNodes) ReconcilePlayState(Runtime, Node);
        }
        return true;
    }

    virtual bool ValidateInitialBindings(FString& OutError) override
    {
        for (const FRuleState& Rule : RuleStates)
        {
            if (Rule.bRequiredOnLoad && Rule.ActiveNodes.IsEmpty())
            {
                OutError = FString::Printf(TEXT("Motion Manifest requiredOnLoad rule has no bound nodes: animation=%s selector=%s revision=%llu"),
                    *Rule.Name, *Rule.Selector, static_cast<unsigned long long>(MutationRevision));
                return false;
            }
        }
        return true;
    }

    virtual FString GetActivationDiagnostics() const override
    {
        TArray<TSharedPtr<FJsonValue>> Rules;
        Rules.Reserve(RuleStates.Num());
        for (const FRuleState& Rule : RuleStates)
        {
            TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("selector"), Rule.Selector);
            Entry->SetStringField(TEXT("animationName"), Rule.Name);
            Entry->SetBoolField(TEXT("requiredOnLoad"), Rule.bRequiredOnLoad);
            Entry->SetNumberField(TEXT("matched"), static_cast<double>(Rule.Matched));
            Entry->SetNumberField(TEXT("bound"), static_cast<double>(Rule.Bound));
            Entry->SetNumberField(TEXT("played"), static_cast<double>(Rule.Played));
            Entry->SetNumberField(TEXT("started"), static_cast<double>(Rule.Started));
            Entry->SetNumberField(TEXT("activeNodes"), Rule.ActiveNodes.Num());
            Entry->SetNumberField(TEXT("lastMutationRevision"), static_cast<double>(Rule.LastMutationRevision));
            Rules.Add(MakeShared<FJsonValueObject>(Entry));
        }
        TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
        Snapshot->SetNumberField(TEXT("mutationRevision"), static_cast<double>(MutationRevision));
        Snapshot->SetArrayField(TEXT("rules"), MoveTemp(Rules));
        FString Json;
        FJsonSerializer::Serialize(Snapshot, TJsonWriterFactory<>::Create(&Json));
        return Json;
    }

    virtual void FlushLifecycleEvents() override
    {
        if (PendingLifecycleEvents.IsEmpty()) return;
        TArray<FLifecycleEvent> Events = MoveTemp(PendingLifecycleEvents);
        PendingLifecycleEvents.Reset();
        if (!LifecycleEventBatchCallback) return;
        TArray<TSharedPtr<FJsonValue>> JsonEvents;
        JsonEvents.Reserve(Events.Num());
        for (const FLifecycleEvent& Event : Events)
        {
            TSharedRef<FJsonObject> JsonEvent = MakeShared<FJsonObject>();
            JsonEvent->SetNumberField(TEXT("target"), Event.Node);
            JsonEvent->SetStringField(TEXT("animationName"), Event.Name);
            JsonEvent->SetStringField(TEXT("type"), Event.Type);
            JsonEvent->SetNumberField(TEXT("iteration"), static_cast<double>(Event.Iteration));
            JsonEvents.Add(MakeShared<FJsonValueObject>(JsonEvent));
        }
        TSharedRef<FJsonObject> Batch = MakeShared<FJsonObject>();
        Batch->SetStringField(TEXT("type"), TEXT("css-animation-events"));
        Batch->SetArrayField(TEXT("events"), MoveTemp(JsonEvents));
        FString Json;
        FJsonSerializer::Serialize(Batch, TJsonWriterFactory<>::Create(&Json));
        LifecycleEventBatchCallback(Json);
    }

private:
    static bool SelectorDependsOnAncestor(const FString& Selector)
    {
        return Selector.Contains(TEXT(" ")) || Selector.Contains(TEXT("\t")) ||
            Selector.Contains(TEXT("\n")) || Selector.Contains(TEXT("\r")) || Selector.Contains(TEXT(">"));
    }

    static void NodeMutated(void* User, RmlUE_Node Node, uint32_t Flags)
    {
        auto* Self = static_cast<FRmlUiCssAnimationSession*>(User);
        if (Self)
            Self->NotifyNodeMutation(Node, (Flags & RMLUE_NODE_MUTATION_SUBTREE) != 0,
                (Flags & RMLUE_NODE_MUTATION_SELECTOR_CONTEXT) != 0);
    }

    static void ReleaseActivation(FRmlUiAnimationRuntime& Runtime, FNodeActivation& Activation)
    {
        for (const FRmlUiAnimationHandle Handle : Activation.Animations) Runtime.Cancel(Handle);
        for (const FRmlUiAnimationBindingHandle Handle : Activation.Bindings) Runtime.ReleaseBinding(Handle);
        Activation.Animations.Reset(); Activation.Bindings.Reset();
    }

    bool ActivateNode(FRmlUiAnimationRuntime& Runtime, FRuleState& Rule, RmlUE_Node Node, FString& OutError)
    {
        if (Rule.ActiveNodes.Contains(Node)) return true;
        ++Rule.Matched;
        Rule.LastMutationRevision = MutationRevision;
        FNodeActivation Activation;
        const TSharedRef<FLifecycleGroup> Lifecycle = MakeShared<FLifecycleGroup>();
        Lifecycle->RemainingTracks = Rule.Definitions.Num();
        for (int32 DefinitionIndex = 0; DefinitionIndex < Rule.Definitions.Num(); ++DefinitionIndex)
        {
            const FRmlUiAnimationDefinitionHandle Definition = Rule.Definitions[DefinitionIndex];
            const FRmlUiAnimationBindingHandle Binding = Runtime.BindNode(Definition, View, Node);
            if (!Binding.IsValid())
            {
                OutError = FString::Printf(TEXT("Motion Manifest bind failed: animation=%s selector=%s node=%u propertyId=%d revision=%llu"),
                    *Rule.Name, *Rule.Selector, Node, Rule.PropertyIds[DefinitionIndex],
                    static_cast<unsigned long long>(MutationRevision));
                ReleaseActivation(Runtime, Activation); return false;
            }
            Activation.Bindings.Add(Binding);
            ++Rule.Bound;
            const FRmlUiAnimationCompletionCallback Completion =
                [this, Node, Name = Rule.Name, Lifecycle](FRmlUiAnimationHandle, ERmlUiAnimationCompletionReason Reason)
                {
                    --Lifecycle->RemainingTracks;
                    if (Lifecycle->bTerminalQueued) return;
                    if (Reason != ERmlUiAnimationCompletionReason::Completed)
                    {
                        Lifecycle->bTerminalQueued = true;
                        PendingLifecycleEvents.Add({Node, Name, TEXT("animationcancel"), 0});
                    }
                    else if (Lifecycle->RemainingTracks == 0)
                    {
                        Lifecycle->bTerminalQueued = true;
                        PendingLifecycleEvents.Add({Node, Name, TEXT("animationend"), 0});
                    }
                };
            FRmlUiAnimationLifecycleCallback LifecycleCallback;
            if (DefinitionIndex == 0)
                LifecycleCallback = [this, Node, Name = Rule.Name, RuleIndex = static_cast<int32>(&Rule - RuleStates.GetData())](FRmlUiAnimationHandle,
                    ERmlUiAnimationLifecyclePhase Phase, int64 Iteration)
                {
                    if (Phase == ERmlUiAnimationLifecyclePhase::Started && RuleStates.IsValidIndex(RuleIndex))
                        ++RuleStates[RuleIndex].Started;
                    PendingLifecycleEvents.Add({Node, Name,
                        Phase == ERmlUiAnimationLifecyclePhase::Started
                            ? TEXT("animationstart") : TEXT("animationiteration"), Iteration});
                };
            const FRmlUiAnimationHandle Animation = Runtime.PlayBinding(
                Binding, {}, Completion, MoveTemp(LifecycleCallback));
            if (!Animation.IsValid())
            {
                OutError = FString::Printf(TEXT("Motion Manifest play failed: animation=%s selector=%s node=%u propertyId=%d revision=%llu"),
                    *Rule.Name, *Rule.Selector, Node, Rule.PropertyIds[DefinitionIndex],
                    static_cast<unsigned long long>(MutationRevision));
                ReleaseActivation(Runtime, Activation); return false;
            }
            Activation.Animations.Add(Animation);
            ++Rule.Played;
            if (Rule.bPaused && !Runtime.Pause(Animation))
            {
                OutError = FString::Printf(TEXT("Motion Manifest pause failed: animation=%s selector=%s node=%u propertyId=%d revision=%llu"),
                    *Rule.Name, *Rule.Selector, Node, Rule.PropertyIds[DefinitionIndex],
                    static_cast<unsigned long long>(MutationRevision));
                ReleaseActivation(Runtime, Activation); return false;
            }
        }
        Rule.ActiveNodes.Add(Node, MoveTemp(Activation));
        ReconcilePlayState(Runtime, Node);
        return true;
    }

    void ReconcilePlayState(FRmlUiAnimationRuntime& Runtime, RmlUE_Node Node)
    {
        if (!RmlUE_IsNodeValid(View, Node)) return;
        TOptional<bool> Paused;
        for (const FPlayStateRule& State : PlayStateRules)
            if (RmlUE_MatchesNode(View, Node, TCHAR_TO_UTF8(*State.Selector))) Paused = State.bPaused;
        if (!Paused.IsSet()) return;
        for (FRuleState& Rule : RuleStates)
            if (FNodeActivation* Activation = Rule.ActiveNodes.Find(Node))
                for (const FRmlUiAnimationHandle Handle : Activation->Animations)
                    if (Paused.GetValue()) Runtime.Pause(Handle); else Runtime.Resume(Handle);
    }

    RmlUE_View* View = nullptr;
    TArray<FRuleState> RuleStates;
    TArray<FPlayStateRule> PlayStateRules;
    TSet<RmlUE_Node> DirtyNodes;
    TSet<RmlUE_Node> DirtySubtrees;
    TSet<RmlUE_Node> DirtySelectorContexts;
    TSet<RmlUE_Node> RestartNodes;
    bool bHasStructuralMutation = false;
    bool bHasRelationalPlayStateSelector = false;
    bool bNeedsInitialScan = false;
    uint64 MutationRevision = 0;
    TFunction<void()> WakeCallback;
    TFunction<void(const FString&)> LifecycleEventBatchCallback;
    TArray<FLifecycleEvent> PendingLifecycleEvents;
    FDelegateHandle PostAdvanceHandle;
};

IRmlUiCssAnimationSession* CreateRmlUiCssAnimationSession()
{
    return new FRmlUiCssAnimationSession();
}

URmlUiWebWidget::~URmlUiWebWidget()
{
    if (const TSharedPtr<SRmlUiWidget> SlateWidget = GetSlateRmlWidget();
        CssAnimationFrameHandle.IsValid() && SlateWidget.IsValid())
        SlateWidget->OnBeforeRender.Remove(CssAnimationFrameHandle);
    delete CssAnimationSession;
    CssAnimationSession = nullptr;
}

RmlUE_StyleSheet* URmlUiWebWidget::GetBaseStyleSheet() const
{
    if (CompatibilityProfile.IsNone() || CompatibilityProfile == TEXT("RawRml")) return nullptr;
    return FRmlUiWebCompatModule::Get().FindProfile(CompatibilityProfile);
}

bool URmlUiWebWidget::SetCompatibilityProfile(FName ProfileId)
{
    if (!ProfileId.IsNone() && ProfileId != TEXT("RawRml") && !FRmlUiWebCompatModule::Get().FindProfile(ProfileId))
        return false;
    CompatibilityProfile = ProfileId;
    SynchronizeProperties();
    return true;
}

TArray<FName> URmlUiWebWidget::GetAvailableCompatibilityProfiles() const
{
    TArray<FName> Result{TEXT("RawRml")};
    Result.Append(FRmlUiWebCompatModule::Get().GetProfileIds());
    return Result;
}

FName URmlUiWebWidget::GetDynamicDocumentCompilerId() const
{
    return FRmlUiWebCompatModule::Get().GetDocumentCompilerId();
}

bool URmlUiWebWidget::PrepareDocumentMarkup(const FString& Markup, const FString& SourcePath, FString& OutMarkup)
{
    LastCompatibilityDiagnostics.Reset();
    bLastCompatibilityCompileCacheHit = false;
    PendingMotionManifest.Reset();
    if ((!bCompileDynamicBrowserCss && !bEnforceRendererCapabilities) || CompatibilityProfile.IsNone() || CompatibilityProfile == TEXT("RawRml"))
    {
        OutMarkup = Markup;
        return true;
    }
    FRmlUiCssCompileOptions Options;
    if (bEnforceRendererCapabilities)
    {
        Options.CapabilityProfile = bUseSlateRenderer ? TEXT("slate-rhi") : TEXT("dx11-compat");
        Options.CapabilityMode = AllowedCssDegradations.IsEmpty() ? TEXT("strict") : TEXT("degrade");
        Options.AllowedDegradations = AllowedCssDegradations;
    }
    return FRmlUiWebCompatModule::Get().CompileDynamicDocument(Markup, SourcePath, OutMarkup,
        LastCompatibilityDiagnostics, bLastCompatibilityCompileCacheHit, Options, &PendingMotionManifest);
}

void URmlUiWebWidget::OnDocumentLoaded(bool bSuccess)
{
    const TSharedPtr<SRmlUiWidget> SlateWidget = GetSlateRmlWidget();
    if (CssAnimationFrameHandle.IsValid() && SlateWidget.IsValid())
        SlateWidget->OnBeforeRender.Remove(CssAnimationFrameHandle);
    CssAnimationFrameHandle.Reset();
    if (!CssAnimationSession) CssAnimationSession = CreateRmlUiCssAnimationSession();
    CssAnimationSession->Reset();
    if (!bSuccess || CompatibilityProfile.IsNone() || CompatibilityProfile == TEXT("RawRml")) return;
    FString Manifest = DocumentPath.IsEmpty() ? PendingMotionManifest : FString();
    if (!DocumentPath.IsEmpty())
    {
        const FString SidecarPath = FRmlUiUnrealModule::Get().ResolveDocumentPath(DocumentPath) + TEXT(".webcompat.json");
        FString SidecarJson;
        TSharedPtr<FJsonObject> Sidecar;
        const TSharedPtr<FJsonObject>* Motion = nullptr;
        if (FFileHelper::LoadFileToString(SidecarJson, *SidecarPath) &&
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SidecarJson), Sidecar) && Sidecar.IsValid() &&
            Sidecar->TryGetObjectField(TEXT("motionManifest"), Motion) && Motion && Motion->IsValid())
            FJsonSerializer::Serialize(Motion->ToSharedRef(), TJsonWriterFactory<>::Create(&Manifest));
    }
    if (Manifest.IsEmpty()) return;
    FString Error;
    if (SlateWidget.IsValid())
    {
        const TWeakPtr<SRmlUiWidget> WeakSlate = SlateWidget;
        CssAnimationSession->SetWakeCallback([WeakSlate]()
        {
            if (const TSharedPtr<SRmlUiWidget> Pinned = WeakSlate.Pin()) Pinned->RequestScheduledRender();
        });
    }
    if (!SlateWidget.IsValid() || !CssAnimationSession->Install(Manifest, SlateWidget->GetNativeView(), Error) ||
        !CssAnimationSession->FlushActivationChanges(Error) || !CssAnimationSession->ValidateInitialBindings(Error))
    {
        LastCompatibilityDiagnostics += FString::Printf(TEXT("\nMotion Manifest runtime error: %s"), *Error);
        UE_LOG(LogRmlUiWebCompat, Error, TEXT("%s"), *Error);
        CssAnimationSession->Reset();
        return;
    }
    CssAnimationFrameHandle = SlateWidget->OnBeforeRender.AddUObject(
        this, &URmlUiWebWidget::AdvanceCssAnimationActivation);
}

void URmlUiWebWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    if (const TSharedPtr<SRmlUiWidget> SlateWidget = GetSlateRmlWidget();
        CssAnimationFrameHandle.IsValid() && SlateWidget.IsValid())
        SlateWidget->OnBeforeRender.Remove(CssAnimationFrameHandle);
    CssAnimationFrameHandle.Reset();
    if (CssAnimationSession) CssAnimationSession->Reset();
    Super::ReleaseSlateResources(bReleaseChildren);
}

void URmlUiWebWidget::AdvanceCssAnimationActivation(float DeltaSeconds)
{
    (void)DeltaSeconds;
    if (!CssAnimationSession) return;
    FString Error;
    if (!CssAnimationSession->FlushActivationChanges(Error))
    {
        LastCompatibilityDiagnostics += FString::Printf(TEXT("\nMotion Manifest activation error: %s"), *Error);
        UE_LOG(LogRmlUiWebCompat, Error, TEXT("%s"), *Error);
    }
}

FString URmlUiWebWidget::GetCssAnimationDiagnostics() const
{
    return CssAnimationSession ? CssAnimationSession->GetActivationDiagnostics() : TEXT("{\"rules\":[]}");
}
