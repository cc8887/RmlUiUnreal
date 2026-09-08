#include "RmlUiWidget.h"

#include "SRmlUiWidget.h"
#include "RmlUiBridge.h"
#include "RmlUiUnrealModule.h"

TSharedRef<SWidget> URmlUiWidget::RebuildWidget()
{
    MyRmlWidget = SNew(SRmlUiWidget)
        .DocumentPath(DocumentPath)
        .InlineDocument(InlineDocument)
        .SourcePath(InlineSourcePath)
        .DesiredSize(DesiredSize)
        .MaxTextureDimension(MaxTextureDimension)
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
        if (InlineDocument.IsEmpty()) MyRmlWidget->LoadDocument(DocumentPath);
        else MyRmlWidget->LoadDocumentFromString(InlineDocument, InlineSourcePath);
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
    InlineDocument = Markup;
    InlineSourcePath = SourcePath;
    DocumentPath.Reset();
    TakeWidget();
    return MyRmlWidget.IsValid() && MyRmlWidget->LoadDocumentFromString(Markup, SourcePath);
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
