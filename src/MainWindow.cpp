#include "MainWindow.h"

#include <cstring>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <vector>

#include "Ids.h"
#include "Messages.h"
#include "Resource.h"
#include "StringUtils.h"
#include "TranslatorClient.h"

namespace {
constexpr wchar_t kWindowClassName[] = L"TsltMainWindow";
constexpr wchar_t kWindowTitle[] = L"LLM Translator";
constexpr int kMinWindowWidth = 640;
constexpr int kMinWindowHeight = 480;
constexpr int kMargin = 16;
constexpr int kTopRowHeight = 32;
constexpr int kBottomRowHeight = 24;
constexpr int kButtonWidth = 112;
constexpr int kComboWidth = 168;
constexpr int kProviderComboWidth = 168;
constexpr DWORD kEscapeExitIntervalMs = 500;
}

MainWindow::MainWindow(HINSTANCE instance, std::wstring appName, AppConfig config, std::wstring statePath, std::wstring initialInputText)
    : instance_(instance),
      appName_(std::move(appName)),
      config_(std::move(config)),
      configPath_(config_.loadedPath),
      statePath_(std::move(statePath)),
      initialInputText_(std::move(initialInputText)),
      configWatcherStop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
}

bool MainWindow::Create() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWindow::WndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kWindowClassName;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    windowState_ = WindowState{};

    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowState_.width,
        windowState_.height,
        nullptr,
        nullptr,
        instance_,
        this);

    return hwnd_ != nullptr;
}

HWND MainWindow::hwnd() const noexcept {
    return hwnd_;
}

bool MainWindow::HandleGlobalShortcut(const MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) {
        return false;
    }

    const HWND target = msg.hwnd;
    if (!IsWindowMessageTarget(target)) {
        return false;
    }

    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg.wParam) {
    case 'C':
        if (!ctrlDown) {
            break;
        }
        if (IsEditControl(target) && HasSelectedText(target)) {
            return false;
        }
        if (const std::wstring text = ReadTextFromClipboard(); !text.empty()) {
            SetInputText(text);
            SetFocus(inputEdit_);
            SetTranslating(isTranslating_, L"Loaded clipboard into input.");
        } else {
            SetTranslating(isTranslating_, L"Clipboard is empty.");
        }
        return true;
    case 'A':
        if (!ctrlDown) {
            break;
        }
        if (!IsEditControl(target)) {
            break;
        }
        SendMessageW(target, EM_SETSEL, 0, -1);
        return true;
    case 'V':
        if (!ctrlDown) {
            break;
        }
        if (target == inputEdit_) {
            return false;
        }
        OnCopyClicked();
        return true;
    case 'W':
        if (!ctrlDown) {
            break;
        }
        OnTranslateClicked();
        return true;
    case 'Q':
        if (!ctrlDown) {
            break;
        }
        OnClose();
        return true;
    case VK_ESCAPE: {
        const DWORD now = GetTickCount();
        if (now - lastEscapeTick_ <= kEscapeExitIntervalMs) {
            OnClose();
        } else {
            lastEscapeTick_ = now;
            SetTranslating(isTranslating_, L"Press Esc again to quit.");
        }
        return true;
    }
    default:
        break;
    }

    return false;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(createStruct->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->HandleMessage(msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCREATE:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    case WM_CREATE:
        return OnCreate() ? 0 : -1;
    case WM_SIZE:
        OnSize(static_cast<UINT>(wParam), LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MOVE:
        OnMove(static_cast<int>(static_cast<short>(LOWORD(lParam))), static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = kMinWindowWidth;
        info->ptMinTrackSize.y = kMinWindowHeight;
        return 0;
    }
    case WM_CLOSE:
        OnClose();
        return 0;
    case WM_DESTROY:
        OnDestroy();
        return 0;
    case WM_APP_TRANSLATE_DONE:
        OnTranslateDone(reinterpret_cast<TranslateResult*>(lParam));
        return 0;
    case WM_APP_TRANSLATE_ERROR:
        OnTranslateError(reinterpret_cast<TranslateError*>(lParam));
        return 0;
    case WM_APP_CONFIG_CHANGED:
        OnConfigChanged();
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

bool MainWindow::OnCreate() {
    inputEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_INPUT), instance_, nullptr);

    targetLangCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_TARGET_LANG), instance_, nullptr);

    providerCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_PROVIDER), instance_, nullptr);

    translateButton_ = CreateWindowExW(0, L"BUTTON", L"Translate",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_TRANSLATE), instance_, nullptr);

    outputEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_OUTPUT), instance_, nullptr);

    copyButton_ = CreateWindowExW(0, L"BUTTON", L"Copy",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_COPY), instance_, nullptr);

    statusStatic_ = CreateWindowExW(0, L"STATIC", L"Ready",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_STATUS), instance_, nullptr);

    if (inputEdit_ == nullptr || targetLangCombo_ == nullptr || providerCombo_ == nullptr || translateButton_ == nullptr ||
        outputEdit_ == nullptr || copyButton_ == nullptr || statusStatic_ == nullptr) {
        return false;
    }

    ApplyVisualStyle();
    ApplyUiFont();

    const wchar_t* languages[] = {L"zh-CN", L"en", L"ja", L"ko", L"fr", L"de"};
    int selectedIndex = 0;
    for (int i = 0; i < static_cast<int>(std::size(languages)); ++i) {
        SendMessageW(targetLangCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(languages[i]));
        if (config_.translate.targetLanguage == languages[i]) {
            selectedIndex = i;
        }
    }
    SendMessageW(targetLangCombo_, CB_SETCURSEL, selectedIndex, 0);

    int selectedProviderIndex = 0;
    for (int i = 0; i < static_cast<int>(config_.llm.providers.size()); ++i) {
        const LlmProviderConfig& provider = config_.llm.providers[static_cast<size_t>(i)];
        SendMessageW(providerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(provider.displayName.c_str()));
        if (provider.sectionName == config_.llm.provider) {
            selectedProviderIndex = i;
        }
    }
    if (!config_.llm.providers.empty()) {
        SendMessageW(providerCombo_, CB_SETCURSEL, selectedProviderIndex, 0);
        OnProviderChanged();
    }

    if (!initialInputText_.empty()) {
        SetWindowTextW(inputEdit_, initialInputText_.c_str());
    }

    ApplyWindowState();
    SetTranslating(false, config_.loadedPath.empty() ? L"Ready. config.ini not found." : L"Ready");
    StartConfigWatcher();
    return true;
}

void MainWindow::OnSize(UINT sizeType, int width, int height) {
    if (sizeType != SIZE_MINIMIZED) {
        LayoutControls(width, height);
        UpdateWindowState();
    }
}

void MainWindow::OnMove(int, int) {
    UpdateWindowState();
}

void MainWindow::OnCommand(int controlId, int notifyCode, HWND) {
    switch (controlId) {
    case IDC_TRANSLATE:
        if (notifyCode == BN_CLICKED) {
            OnTranslateClicked();
        }
        break;
    case IDC_COPY:
        if (notifyCode == BN_CLICKED) {
            OnCopyClicked();
        }
        break;
    case IDC_TARGET_LANG:
        if (notifyCode == CBN_SELCHANGE) {
            OnTargetLanguageChanged();
        }
        break;
    case IDC_PROVIDER:
        if (notifyCode == CBN_SELCHANGE) {
            OnProviderChanged();
        }
        break;
    default:
        break;
    }
}

void MainWindow::OnClose() {
    UpdateWindowState();
    ConfigStore(appName_).SaveWindowState(windowState_);
    DestroyWindow(hwnd_);
}

void MainWindow::OnDestroy() {
    StopConfigWatcher();
    if (uiFont_ != nullptr) {
        DeleteObject(uiFont_);
        uiFont_ = nullptr;
    }
    PostQuitMessage(0);
}

void MainWindow::OnTranslateClicked() {
    lastEscapeTick_ = 0;
    if (isTranslating_) {
        return;
    }

    const std::wstring inputText = GetWindowTextCopy(inputEdit_);
    if (TrimCopy(inputText).empty()) {
        SetOutputText(L"");
        SetTranslating(false, L"Input text is empty.");
        return;
    }

    const int index = static_cast<int>(SendMessageW(targetLangCombo_, CB_GETCURSEL, 0, 0));
    wchar_t languageBuffer[64] = {};
    SendMessageW(targetLangCombo_, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(languageBuffer));
    config_.translate.targetLanguage = languageBuffer;

    const LlmProviderConfig* provider = FindProviderConfig(config_.llm.provider);
    if (provider == nullptr || TrimCopy(provider->baseUrl).empty() || TrimCopy(provider->apiKey).empty() || TrimCopy(provider->model).empty()) {
        SetOutputText(L"LLM configuration is incomplete.");
        SetTranslating(false, L"LLM configuration is incomplete.");
        return;
    }

    TranslateRequest request;
    request.ownerHwnd = hwnd_;
    request.baseUrl = provider->baseUrl;
    request.apiKey = provider->apiKey;
    request.model = provider->model;
    request.sourceLanguage = config_.translate.sourceLanguage;
    request.targetLanguage = config_.translate.targetLanguage;
    request.temperature = config_.translate.temperature;
    request.inputText = inputText;

    SetOutputText(L"");
    SetTranslating(true, L"Translating...");
    StartTranslateWorker(std::move(request));
}

void MainWindow::OnCopyClicked() {
    const std::wstring text = GetWindowTextCopy(outputEdit_);
    if (text.empty()) {
        SetTranslating(isTranslating_, L"Nothing to copy.");
        return;
    }
    SetTranslating(isTranslating_, CopyTextToClipboard(text) ? L"Copied." : L"Copy failed.");
}

void MainWindow::OnTargetLanguageChanged() {
    const int index = static_cast<int>(SendMessageW(targetLangCombo_, CB_GETCURSEL, 0, 0));
    if (index == CB_ERR) {
        return;
    }
    wchar_t languageBuffer[64] = {};
    SendMessageW(targetLangCombo_, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(languageBuffer));
    config_.translate.targetLanguage = languageBuffer;
}

void MainWindow::OnProviderChanged() {
    const int index = static_cast<int>(SendMessageW(providerCombo_, CB_GETCURSEL, 0, 0));
    if (index == CB_ERR || index < 0 || index >= static_cast<int>(config_.llm.providers.size())) {
        return;
    }

    config_.llm.provider = config_.llm.providers[static_cast<size_t>(index)].sectionName;
}

void MainWindow::OnTranslateDone(TranslateResult* result) {
    if (result != nullptr) {
        SetOutputText(result->translatedText);
        delete result;
    }
    SetTranslating(false, L"Done.");
}

void MainWindow::OnTranslateError(TranslateError* error) {
    if (error != nullptr) {
        SetOutputText(error->message);
        SetTranslating(false, error->message);
        delete error;
        return;
    }
    SetTranslating(false, L"Translation failed.");
}

void MainWindow::LayoutControls(int clientWidth, int clientHeight) {
    const int topY = kMargin;
    const int statusY = clientHeight - kMargin - kBottomRowHeight;
    const int contentY = topY + kTopRowHeight + kMargin;
    const int contentHeight = statusY - contentY - kMargin;
    const int topButtonsWidth = kComboWidth + kMargin + kProviderComboWidth + kMargin + kButtonWidth + kMargin + kButtonWidth;
    const int outputHeight = contentHeight / 2;
    const int inputHeight = contentHeight - outputHeight - kMargin;

    const int providerX = kMargin + kComboWidth + kMargin;
    MoveWindow(targetLangCombo_, kMargin, topY, kComboWidth, 400, TRUE);
    MoveWindow(providerCombo_, providerX, topY, kProviderComboWidth, 400, TRUE);
    MoveWindow(translateButton_, clientWidth - kMargin - (kButtonWidth * 2) - kMargin, topY, kButtonWidth, kTopRowHeight, TRUE);
    MoveWindow(copyButton_, clientWidth - kMargin - kButtonWidth, topY, kButtonWidth, kTopRowHeight, TRUE);
    MoveWindow(inputEdit_, kMargin, contentY, clientWidth - (kMargin * 2), inputHeight, TRUE);
    MoveWindow(outputEdit_, kMargin, contentY + inputHeight + kMargin, clientWidth - (kMargin * 2), outputHeight, TRUE);
    MoveWindow(statusStatic_, kMargin, statusY, clientWidth - (kMargin * 2), kBottomRowHeight, TRUE);
    (void)topButtonsWidth;
}

void MainWindow::SetTranslating(bool translating, const std::wstring& statusText) {
    isTranslating_ = translating;
    EnableWindow(translateButton_, translating ? FALSE : TRUE);
    EnableWindow(targetLangCombo_, translating ? FALSE : TRUE);
    EnableWindow(providerCombo_, translating ? FALSE : TRUE);
    if (!statusText.empty()) {
        SetWindowTextW(statusStatic_, statusText.c_str());
    }
}

void MainWindow::UpdateWindowState() {
    if (hwnd_ == nullptr) {
        return;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd_, &placement)) {
        return;
    }

    windowState_.maximized = placement.showCmd == SW_MAXIMIZE;
    if (placement.showCmd != SW_NORMAL) {
        return;
    }

    RECT rect{};
    if (!GetWindowRect(hwnd_, &rect)) {
        return;
    }

    windowState_.x = rect.left;
    windowState_.y = rect.top;
    windowState_.width = rect.right - rect.left;
    windowState_.height = rect.bottom - rect.top;
}

void MainWindow::ApplyWindowState() {
    windowState_ = ConfigStore(appName_).LoadWindowState();

    bool centered = false;
    if (windowState_.x == CW_USEDEFAULT || windowState_.y == CW_USEDEFAULT) {
        CenterWindow();
        centered = true;
    } else {
        SetWindowPos(hwnd_, nullptr, windowState_.x, windowState_.y, windowState_.width, windowState_.height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
        if (monitor == nullptr) {
            CenterWindow();
            centered = true;
        }
    }

    ShowWindow(hwnd_, windowState_.maximized ? SW_MAXIMIZE : SW_SHOW);
    UpdateWindow(hwnd_);
    if (centered) {
        UpdateWindowState();
    }
}

void MainWindow::CenterWindow() {
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    const int width = windowState_.width;
    const int height = windowState_.height;
    const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;

    SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool MainWindow::IsNormalWindowState() const {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd_, &placement)) {
        return false;
    }
    return placement.showCmd == SW_NORMAL;
}

std::wstring MainWindow::GetWindowTextCopy(HWND control) const {
    const auto buffer = ReadEditText(control);
    return std::wstring(buffer.data());
}

void MainWindow::SetInputText(const std::wstring& text) {
    const std::wstring normalized = NormalizeEditControlLineEndings(text);
    SetWindowTextW(inputEdit_, normalized.c_str());
    SendMessageW(inputEdit_, EM_SETSEL, static_cast<WPARAM>(normalized.size()), static_cast<LPARAM>(normalized.size()));
}

void MainWindow::SetOutputText(const std::wstring& text) {
    const std::wstring normalized = NormalizeEditControlLineEndings(text);
    SetWindowTextW(outputEdit_, normalized.c_str());
}

bool MainWindow::CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(hwnd_)) {
        return false;
    }

    EmptyClipboard();
    const std::wstring normalized = NormalizeEditControlLineEndings(text);
    const size_t bytes = (normalized.size() + 1) * sizeof(wchar_t);
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (global == nullptr) {
        CloseClipboard();
        return false;
    }

    void* memory = GlobalLock(global);
    if (memory == nullptr) {
        GlobalFree(global);
        CloseClipboard();
        return false;
    }

    std::memcpy(memory, normalized.c_str(), bytes);
    GlobalUnlock(global);

    if (SetClipboardData(CF_UNICODETEXT, global) == nullptr) {
        GlobalFree(global);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

std::wstring MainWindow::ReadTextFromClipboard() const {
    if (!OpenClipboard(hwnd_)) {
        return L"";
    }

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data == nullptr) {
        CloseClipboard();
        return L"";
    }

    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text == nullptr) {
        CloseClipboard();
        return L"";
    }

    std::wstring result = NormalizeEditControlLineEndings(text);
    GlobalUnlock(data);
    CloseClipboard();
    return result;
}

bool MainWindow::IsWindowMessageTarget(HWND target) const {
    return target == hwnd_ || (target != nullptr && IsChild(hwnd_, target));
}

bool MainWindow::IsEditControl(HWND target) const {
    return target == inputEdit_ || target == outputEdit_;
}

bool MainWindow::HasSelectedText(HWND target) const {
    if (!IsEditControl(target)) {
        return false;
    }

    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(target, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    return start != end;
}

void MainWindow::ApplyVisualStyle() {
    ApplyThemeToControl(targetLangCombo_, L"Explorer", nullptr);
    ApplyThemeToControl(providerCombo_, L"Explorer", nullptr);
    ApplyThemeToControl(translateButton_, L"Explorer", nullptr);
    ApplyThemeToControl(copyButton_, L"Explorer", nullptr);
    ApplyThemeToControl(inputEdit_, L"Explorer", nullptr);
    ApplyThemeToControl(outputEdit_, L"Explorer", nullptr);
}

void MainWindow::ApplyUiFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return;
    }

    uiFont_ = CreateFontIndirectW(&metrics.lfMessageFont);
    if (uiFont_ == nullptr) {
        return;
    }

    const HWND controls[] = {inputEdit_, targetLangCombo_, providerCombo_, translateButton_, outputEdit_, copyButton_, statusStatic_};
    for (HWND control : controls) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
}

const LlmProviderConfig* MainWindow::FindProviderConfig(const std::wstring& providerName) const {
    for (const LlmProviderConfig& provider : config_.llm.providers) {
        if (provider.sectionName == providerName) {
            return &provider;
        }
    }
    return nullptr;
}

void MainWindow::ApplyThemeToControl(HWND control, const wchar_t* subAppName, const wchar_t* subIdList) {
    if (control != nullptr) {
        SetWindowTheme(control, subAppName, subIdList);
    }
}

void MainWindow::StartConfigWatcher() {
    std::wstring watchDir;
    std::wstring watchFile;

    if (!configPath_.empty()) {
        const size_t pos = configPath_.find_last_of(L"\\/");
        watchDir = (pos != std::wstring::npos) ? configPath_.substr(0, pos) : L".";
        watchFile = configPath_.substr(pos + 1);
    } else {
        watchDir = ConfigStore(appName_).GetStatePath();
        const size_t pos = watchDir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            watchDir = watchDir.substr(0, pos);
        }
        watchFile = L"config.ini";
    }

    if (watchDir.empty()) {
        return;
    }

    HANDLE dirHandle = CreateFileW(
        watchDir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (dirHandle == INVALID_HANDLE_VALUE) {
        return;
    }

    struct WatcherData {
        HANDLE dir;
        std::wstring file;
        HWND hwnd;
        HANDLE stopEvent;
        std::atomic<bool>* running;
    };

    auto* data = new WatcherData{dirHandle, std::move(watchFile), hwnd_, configWatcherStop_, &configWatcherRunning_};

    configWatcherRunning_.store(true);
    configWatcherThread_ = CreateThread(
        nullptr,
        0,
        [](LPVOID param) -> DWORD {
            auto* d = static_cast<WatcherData*>(param);
            HANDLE dir = d->dir;
            std::wstring file = std::move(d->file);
            HWND hwnd = d->hwnd;
            HANDLE stopEvent = d->stopEvent;
            std::atomic<bool>* running = d->running;
            delete d;

            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (overlapped.hEvent == nullptr) {
                CloseHandle(dir);
                running->store(false);
                return 1;
            }

            alignas(DWORD) BYTE buffer[4096];
            HANDLE handles[] = {overlapped.hEvent, stopEvent};

            while (running->load()) {
                ResetEvent(overlapped.hEvent);
                if (!ReadDirectoryChangesW(dir, buffer, sizeof(buffer), FALSE,
                        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                        nullptr, &overlapped, nullptr)) {
                    break;
                }

                const DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0) {
                    DWORD bytesReturned = 0;
                    if (!GetOverlappedResult(dir, &overlapped, &bytesReturned, FALSE)) {
                        continue;
                    }
                    if (bytesReturned == 0) {
                        continue;
                    }

                    auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
                    bool found = false;
                    while (true) {
                        const std::wstring name(info->FileName,
                            info->FileNameLength / sizeof(wchar_t));
                        if (_wcsicmp(name.c_str(), file.c_str()) == 0) {
                            found = true;
                            break;
                        }
                        if (info->NextEntryOffset == 0) {
                            break;
                        }
                        info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                            reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
                    }

                    if (found) {
                        PostMessageW(hwnd, WM_APP_CONFIG_CHANGED, 0, 0);
                    }
                } else {
                    break;
                }
            }

            CloseHandle(overlapped.hEvent);
            CloseHandle(dir);
            running->store(false);
            return 0;
        },
        data,
        0,
        nullptr);

    if (configWatcherThread_ == nullptr) {
        configWatcherRunning_.store(false);
        CloseHandle(dirHandle);
        delete data;
    }
}

void MainWindow::StopConfigWatcher() {
    if (configWatcherThread_ == nullptr) {
        return;
    }

    if (configWatcherStop_ != nullptr) {
        SetEvent(configWatcherStop_);
    }

    HANDLE handles[] = {configWatcherThread_};
    while (true) {
        const DWORD result = MsgWaitForMultipleObjects(1, handles, FALSE, INFINITE, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) {
            break;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CloseHandle(configWatcherThread_);
    configWatcherThread_ = nullptr;
}

void MainWindow::OnConfigChanged() {
    const int oldProviderIndex = static_cast<int>(SendMessageW(providerCombo_, CB_GETCURSEL, 0, 0));
    std::wstring savedProviderSection;
    if (oldProviderIndex >= 0 && oldProviderIndex < static_cast<int>(config_.llm.providers.size())) {
        savedProviderSection = config_.llm.providers[static_cast<size_t>(oldProviderIndex)].sectionName;
    }

    ConfigStore store(appName_);
    AppConfig newConfig = store.Load();

    if (newConfig.loadedPath.empty() && !configPath_.empty()) {
        return;
    }

    config_ = std::move(newConfig);
    configPath_ = config_.loadedPath;

    SendMessageW(providerCombo_, CB_RESETCONTENT, 0, 0);
    int newProviderIndex = 0;
    for (int i = 0; i < static_cast<int>(config_.llm.providers.size()); ++i) {
        const auto& provider = config_.llm.providers[static_cast<size_t>(i)];
        SendMessageW(providerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(provider.displayName.c_str()));
        if (provider.sectionName == savedProviderSection) {
            newProviderIndex = i;
        } else if (provider.sectionName == config_.llm.provider) {
            newProviderIndex = i;
        }
    }
    if (!config_.llm.providers.empty()) {
        SendMessageW(providerCombo_, CB_SETCURSEL, newProviderIndex, 0);
        OnProviderChanged();
    }

    SetTranslating(isTranslating_, L"Config reloaded.");
}
