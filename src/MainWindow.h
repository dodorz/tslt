#ifndef TSLT_MAIN_WINDOW_H
#define TSLT_MAIN_WINDOW_H

#include <string>
#include <windows.h>

#include "AppConfig.h"

class MainWindow {
public:
    static constexpr wchar_t kWindowClassName[] = L"TsltMainWindow";

    MainWindow(HINSTANCE instance, std::wstring appName, AppConfig config, std::wstring statePath, std::wstring initialInputText);

    bool Create();
    HWND hwnd() const noexcept;
    bool HandleGlobalShortcut(const MSG& msg);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool OnCreate();
    void OnPaint();
    void OnSize(UINT sizeType, int width, int height);
    void OnMove(int x, int y);
    void OnCommand(int controlId, int notifyCode, HWND controlHwnd);
    void OnClose();
    void OnDestroy();
    void OnTranslateClicked();
    void OnInputChanged();
    void OnTargetLanguageChanged();
    void OnProviderChanged();
    void OnSystemPromptClicked();
    void OnTranslateDone(struct TranslateResult* result);
    void OnTranslateError(struct TranslateError* error);
    void OnDictLookupDone(struct DictionaryLookupResult* result);
    void OnDictLookupError(struct DictionaryLookupError* error);
    void LayoutControls(int clientWidth, int clientHeight);
    void SetTranslating(bool translating, const std::wstring& statusText = L"");
    void UpdateWindowState();
    void ApplyWindowState();
    void CenterWindow();
    bool IsNormalWindowState() const;
    std::wstring GetWindowTextCopy(HWND control) const;
    void SetInputText(const std::wstring& text);
    void SetOutputText(const std::wstring& text);
    bool CopyTextToClipboard(const std::wstring& text);
    std::wstring ReadTextFromClipboard() const;
    bool IsWindowMessageTarget(HWND target) const;
    bool IsEditControl(HWND target) const;
    bool HasSelectedText(HWND target) const;
    void ApplyVisualStyle();
    void ApplyUiFont();
    const LlmProviderConfig* FindProviderConfig(const std::wstring& providerName) const;
    void ApplyThemeToControl(HWND control, const wchar_t* subAppName, const wchar_t* subIdList);

private:
    HINSTANCE instance_ = nullptr;
    std::wstring appName_;
    HWND hwnd_ = nullptr;
    HWND inputEdit_ = nullptr;
    HWND targetLangCombo_ = nullptr;
    HWND providerCombo_ = nullptr;
    HWND systemPromptButton_ = nullptr;
    HWND translateButton_ = nullptr;
    HWND outputEdit_ = nullptr;
    HWND statusStatic_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT emphasisFont_ = nullptr;
    AppConfig config_;
    std::wstring statePath_;
    std::wstring initialInputText_;
    WindowState windowState_{};
    bool isTranslating_ = false;
    DWORD lastEscapeTick_ = 0;
    std::wstring systemPromptOverride_;
};

#endif
