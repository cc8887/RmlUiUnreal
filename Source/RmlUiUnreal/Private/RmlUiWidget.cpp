#include "RmlUiWidget.h"

#include "SRmlUiWidget.h"
#include "Engine/Texture.h"
#include "MaterialDomain.h"
#include "RmlUiBridge.h"
#include "RmlUiUnrealModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"

TSharedRef<SWidget> URmlUiWidget::RebuildWidget()
{
    FString PreparedDocument = InlineDocument;
    FString PreparedDocumentPath = DocumentPath;
    if (!InlineDocument.IsEmpty() && !PrepareDocumentMarkup(InlineDocument, InlineSourcePath, PreparedDocument))
    {
        UE_LOG(LogRmlUiUnreal, Warning, TEXT("Could not prepare inline RmlUi document; the widget will remain empty."));
        PreparedDocument.Reset();
        PreparedDocumentPath.Reset();
    }
    MyRmlWidget = SNew(SRmlUiWidget)
        .DocumentPath(PreparedDocumentPath)
        .InlineDocument(PreparedDocument)
        .SourcePath(InlineSourcePath)
        .DesiredSize(DesiredSize)
        .MaxTextureDimension(MaxTextureDimension)
        .UseSlateRenderer(bUseSlateRenderer)
        .BaseStyleSheet(GetBaseStyleSheet())
        .OnDocumentEvent(FOnSlateRmlUiDocumentEvent::CreateUObject(this, &URmlUiWidget::HandleDocumentEvent));
    return MyRmlWidget.ToSharedRef();
}

void URmlUiWidget::SynchronizeProperties()
{
    Super::SynchronizeProperties();
    if (MyRmlWidget)
    {
        MyRmlWidget->SetDesiredSize(DesiredSize);
        MyRmlWidget->SetMaxTextureDimension(MaxTextureDimension);
        MyRmlWidget->SetUseSlateRenderer(bUseSlateRenderer);
        for (const auto& Pair : MaterialInstances)
        {
            MyRmlWidget->RegisterMaterial(Pair.Key, Pair.Value);
            if (const TMap<FName, TWeakObjectPtr<UTexture>>* Bindings = MaterialTextureBindings.Find(Pair.Key))
                for (const auto& Binding : *Bindings)
                    if (UTexture* Texture = Binding.Value.Get())
                        MyRmlWidget->TrackMaterialTexture(Pair.Key, Binding.Key, Texture);
        }
        MyRmlWidget->SetBaseStyleSheet(GetBaseStyleSheet());
        if (InlineDocument.IsEmpty()) MyRmlWidget->LoadDocument(DocumentPath);
        else
        {
            FString PreparedDocument;
            if (PrepareDocumentMarkup(InlineDocument, InlineSourcePath, PreparedDocument))
                MyRmlWidget->LoadDocumentFromString(PreparedDocument, InlineSourcePath);
        }
    }
}

void URmlUiWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    Super::ReleaseSlateResources(bReleaseChildren);
    MyRmlWidget.Reset();
}

bool URmlUiWidget::LoadDocument(const FString& Path)
{
    DocumentPath = Path;
    InlineDocument.Reset();
    InlineSourcePath.Reset();
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->LoadDocument(Path);
}

bool URmlUiWidget::LoadDocumentFromString(const FString& Markup, const FString& SourcePath)
{
    FString PreparedDocument;
    if (!PrepareDocumentMarkup(Markup, SourcePath, PreparedDocument)) return false;
    InlineDocument = Markup;
    InlineSourcePath = SourcePath;
    DocumentPath.Reset();
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->LoadDocumentFromString(PreparedDocument, SourcePath);
}

bool URmlUiWidget::ReloadDocument()
{
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->ReloadDocument();
}

bool URmlUiWidget::SetElementInnerRml(const FString& Id, const FString& Rml)
{
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->SetElementInnerRml(Id, Rml);
}

bool URmlUiWidget::SetElementText(const FString& Id, const FString& Text)
{
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->SetElementText(Id, Text);
}

bool URmlUiWidget::SetElementProperty(const FString& Id, const FString& Property, const FString& Value)
{
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->SetElementProperty(Id, Property, Value);
}

bool URmlUiWidget::SetElementAttribute(const FString& Id, const FString& Attribute, const FString& Value)
{
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->SetElementAttribute(Id, Attribute, Value);
}

bool URmlUiWidget::GetElementAttribute(const FString& Id, const FString& Attribute, FString& Value)
{
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->GetElementAttribute(Id, Attribute, Value);
}

void URmlUiWidget::SetDebuggerVisible(bool bVisible)
{
    TakeWidget();
    if (MyRmlWidget) MyRmlWidget->SetDebuggerVisible(bVisible);
}

bool URmlUiWidget::LoadFontFace(const FString& FontPath, bool bFallback)
{
    FRmlUiUnrealModule& Module = FRmlUiUnrealModule::Get();
    if (!Module.IsInitialized()) return false;
    const FString ResolvedPath = Module.ResolveDocumentPath(FontPath);
    if (!RmlUE_LoadFont(TCHAR_TO_UTF8(*ResolvedPath), bFallback ? 1 : 0))
    {
        UE_LOG(LogRmlUiUnreal, Warning, TEXT("Could not load font %s: %s"), *ResolvedPath, UTF8_TO_TCHAR(RmlUE_GetLastError()));
        return false;
    }
    return true;
}

bool URmlUiWidget::RegisterMaterial(FName Alias, UMaterialInterface* Material)
{
    if (Alias.IsNone() || !Material || !Material->GetMaterial() || Material->GetMaterial()->MaterialDomain != MD_UI)
    {
        UE_LOG(LogRmlUiUnreal, Warning, TEXT("RmlUi material '%s' must be a valid User Interface domain material."), *Alias.ToString());
        return false;
    }
    UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, this);
    if (!Instance) return false;
    MaterialInstances.Add(Alias, Instance);
    MaterialTextureBindings.Remove(Alias);
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->RegisterMaterial(Alias, Instance);
}

void URmlUiWidget::UnregisterMaterial(FName Alias)
{
    MaterialInstances.Remove(Alias);
    MaterialTextureBindings.Remove(Alias);
    if (MyRmlWidget) MyRmlWidget->UnregisterMaterial(Alias);
}

bool URmlUiWidget::SetMaterialScalar(FName Alias, FName Parameter, float Value)
{
    if (TObjectPtr<UMaterialInstanceDynamic>* Instance = MaterialInstances.Find(Alias))
    {
        (*Instance)->SetScalarParameterValue(Parameter, Value);
        if (MyRmlWidget) MyRmlWidget->Invalidate(EInvalidateWidgetReason::Paint);
        return true;
    }
    return false;
}

bool URmlUiWidget::SetMaterialVector(FName Alias, FName Parameter, FLinearColor Value)
{
    if (TObjectPtr<UMaterialInstanceDynamic>* Instance = MaterialInstances.Find(Alias))
    {
        (*Instance)->SetVectorParameterValue(Parameter, Value);
        if (MyRmlWidget) MyRmlWidget->Invalidate(EInvalidateWidgetReason::Paint);
        return true;
    }
    return false;
}

bool URmlUiWidget::SetMaterialTexture(FName Alias, FName Parameter, UTexture* Value)
{
    if (TObjectPtr<UMaterialInstanceDynamic>* Instance = MaterialInstances.Find(Alias))
    {
        (*Instance)->SetTextureParameterValue(Parameter, Value);
        if (Value) MaterialTextureBindings.FindOrAdd(Alias).Add(Parameter, Value);
        else if (TMap<FName, TWeakObjectPtr<UTexture>>* Bindings = MaterialTextureBindings.Find(Alias))
        {
            Bindings->Remove(Parameter);
            if (Bindings->IsEmpty()) MaterialTextureBindings.Remove(Alias);
        }
        if (MyRmlWidget) MyRmlWidget->TrackMaterialTexture(Alias, Parameter, Value);
        return true;
    }
    return false;
}

FString URmlUiWidget::GetLastError() const
{
    return MyRmlWidget.IsValid() ? MyRmlWidget->GetLastError() : FString();
}

void URmlUiWidget::HandleDocumentEvent(const FString& Type, const FString& ElementId, const FString& Value)
{
    OnDocumentEvent.Broadcast(Type, ElementId, Value);
}

#if WITH_EDITOR
const FText URmlUiWidget::GetPaletteCategory()
{
    return NSLOCTEXT("RmlUiUnreal", "PaletteCategory", "RmlUi");
}
#endif
