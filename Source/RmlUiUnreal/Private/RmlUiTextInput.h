#pragma once

#include "CoreMinimal.h"
#include "GenericPlatform/ITextInputMethodSystem.h"
#include "Layout/Geometry.h"
#include "RmlUiTextInputBridge.h"

class SRmlUiWidget;
class SWindow;

// One registered platform context per Slate widget; the native focus node may change.
class FRmlUiTextInputMethodContext final : public ITextInputMethodContext,
    public TSharedFromThis<FRmlUiTextInputMethodContext>
{
public:
    explicit FRmlUiTextInputMethodContext(TWeakPtr<SRmlUiWidget> InOwner);
    void Initialize();
    void Shutdown();
    void Update(const FGeometry& Geometry, float PixelScale);
    void Deactivate(bool Cancel);
    void CancelComposition();
    bool ConsumeCompositionKey(const FKey& Key);
    bool ConsumeCompositionCharacter(TCHAR Character);

    virtual bool IsComposing() override;
    virtual bool IsReadOnly() override;
    virtual uint32 GetTextLength() override;
    virtual void GetSelectionRange(uint32& Begin, uint32& Length, ECaretPosition& Caret) override;
    virtual void SetSelectionRange(uint32 Begin, uint32 Length, ECaretPosition Caret) override;
    virtual void GetTextInRange(uint32 Begin, uint32 Length, FString& Text) override;
    virtual void SetTextInRange(uint32 Begin, uint32 Length, const FString& Text) override;
    virtual int32 GetCharacterIndexFromPoint(const FVector2D& Point) override;
    virtual bool GetTextBounds(uint32 Begin, uint32 Length, FVector2D& Position, FVector2D& Size) override;
    virtual void GetScreenBounds(FVector2D& Position, FVector2D& Size) override;
    virtual TSharedPtr<FGenericWindow> GetWindow() override;
    virtual void BeginComposition() override;
    virtual void UpdateCompositionRange(int32 Begin, uint32 Length) override;
    virtual void EndComposition() override;

private:
    RmlUE_View* View() const;
    FString ReadText() const;
    void RefreshSnapshot();
    void ConvertBounds(const RmlUE_Rect& Rect, FVector2D& Position, FVector2D& Size) const;
    TWeakPtr<SRmlUiWidget> Owner;
    TWeakPtr<SWindow> Window;
    ITextInputMethodSystem* System = nullptr;
    TSharedPtr<ITextInputMethodChangeNotifier> Notifier;
    FGeometry CachedGeometry;
    float CachedPixelScale = 1;
    FString CachedText;
    RmlUE_TextInputState CachedState{};
    RmlUE_Node PlatformNode = 0;
    bool bRegistered = false;
    bool bActive = false;
    bool bSuppressCommitCharacter = false;
    bool bCancelling = false;
    bool bSwitchingPlatformFocus = false;
};
