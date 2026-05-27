#ifndef TSLT_MAIN_WINDOW_H
#define TSLT_MAIN_WINDOW_H

#include <string>
#include <windows.h>

#include "AppConfig.h"

class MainWindow {
public:
    MainWindow(HINSTANCE instance, std::wstring appName, AppConfig config, std::wstring statePath);

    bool Create();
    HWND hwnd() const noexcept;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool OnCreate();
    void OnSize(UINT sizeType, int width, int height);
    void OnMove(int x, int y);
    void OnCommand(int controlId, int notifyCode, HWND controlHwnd);
    void OnClose();
    void OnDestroy();
    void OnTranslateClicked();
    void OnCopyClicked();
    void OnTargetLanguageChanged();
    void OnTranslateDone(struct TranslateResult* result);
    void OnTranslateError(struct TranslateError* error);
    void LayoutControls(int clientWidth, int clientHeight);
    void SetTranslating(bool translating, const std::wstring& statusText = L"");
    void UpdateWindowState();
    void ApplyWindowState();
    void CenterWindow();
    bool IsNormalWindowState() const;
    std::wstring GetWindowTextCopy(HWND control) const;
    void SetOutputText(const std::wstring& text);
    bool CopyTextToClipboard(const std::wstring& text);
    void ApplyVisualStyle();
    void ApplyUiFont();
    void ApplyThemeToControl(HWND control, const wchar_t* subAppName, const wchar_t* subIdList);

private:
    HINSTANCE instance_ = nullptr;
    std::wstring appName_;
    HWND hwnd_ = nullptr;
    HWND inputEdit_ = nullptr;
    HWND targetLangCombo_ = nullptr;
    HWND translateButton_ = nullptr;
    HWND outputEdit_ = nullptr;
    HWND copyButton_ = nullptr;
    HWND statusStatic_ = nullptr;
    HFONT uiFont_ = nullptr;
    AppConfig config_;
    std::wstring statePath_;
    WindowState windowState_{};
    bool isTranslating_ = false;
};

#endif
