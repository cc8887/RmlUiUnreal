#include "RmlUiWebWidget.h"

#include "RmlUiWebCompatModule.h"

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
    if (!bCompileDynamicBrowserCss || CompatibilityProfile.IsNone() || CompatibilityProfile == TEXT("RawRml"))
    {
        OutMarkup = Markup;
        return true;
    }
    return FRmlUiWebCompatModule::Get().CompileDynamicDocument(Markup, SourcePath, OutMarkup,
        LastCompatibilityDiagnostics, bLastCompatibilityCompileCacheHit);
}
