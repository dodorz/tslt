#include "MainWindow.h"

#include <cstring>
#include <cwctype>
#include <commctrl.h>
#include <shellapi.h>
#include <string_view>
#include <uxtheme.h>
#include <vector>

#include "Ids.h"
#include "Messages.h"
#include "Resource.h"
#include "StringUtils.h"
#include "TranslatorClient.h"
#include "DictionaryClient.h"

namespace {
constexpr wchar_t kWindowTitle[] = L"LLM Translator";
constexpr int kMinWindowWidth = 760;
constexpr int kMinWindowHeight = 480;
constexpr int kMargin = 16;
constexpr int kTopRowHeight = 36;
constexpr int kToolbarPanelPadding = 14;
constexpr int kCardPadding = 10;
constexpr int kBottomRowHeight = 24;
constexpr int kButtonWidth = 112;
constexpr int kPrimaryButtonWidth = 124;
constexpr int kComboWidth = 168;
constexpr int kProviderComboWidth = 168;
constexpr int kPromptButtonWidth = 96;
constexpr DWORD kEscapeExitIntervalMs = 500;
constexpr int kPromptHistoryControlId = 2001;
constexpr wchar_t kPromptDialogClassName[] = L"TsltPromptDialog";

struct PromptDialogState {
    HWND owner = nullptr;
    HWND window = nullptr;
    HWND edit = nullptr;
    HWND okButton = nullptr;
    HWND cancelButton = nullptr;
    HFONT font = nullptr;
    WNDPROC editProc = nullptr;
    std::wstring text;
    bool accepted = false;
    HWND promptCombo = nullptr;
    std::vector<std::wstring> history;
};

std::wstring ReadUnicodeTextFromClipboard(HWND owner) {
    if (!OpenClipboard(owner)) {
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

    std::wstring result(text);
    GlobalUnlock(data);
    CloseClipboard();
    return result;
}

LRESULT CALLBACK PromptEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PromptDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state == nullptr || state->editProc == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_PASTE) {
        const std::wstring clipboardText = ReadUnicodeTextFromClipboard(hwnd);
        if (!clipboardText.empty()) {
            const std::wstring normalized = NormalizeEditControlLineEndings(clipboardText);
            SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(normalized.c_str()));
            return 0;
        }
    }

    return CallWindowProcW(state->editProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK PromptDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PromptDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<PromptDialogState*>(createStruct->lpCreateParams);
        state->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_CREATE: {
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            16, 16, 536, 196, hwnd, nullptr, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), nullptr);
        state->promptCombo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            16, 220, 536, 240, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPromptHistoryControlId)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), nullptr);

        for (const std::wstring& prompt : state->history) {
            SendMessageW(state->promptCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(prompt.c_str()));
        }
        for (int index = 0; index < static_cast<int>(state->history.size()); ++index) {
            if (state->history[static_cast<size_t>(index)] == state->text) {
                SendMessageW(state->promptCombo, CB_SETCURSEL, index, 0);
                break;
            }
        }

        state->okButton = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            360, 264, 92, 32, hwnd, reinterpret_cast<HMENU>(IDOK), reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), nullptr);
        state->cancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            460, 264, 92, 32, hwnd, reinterpret_cast<HMENU>(IDCANCEL), reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), nullptr);

        state->editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(state->edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PromptEditProc)));
        SetWindowLongPtrW(state->edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        if (state->font != nullptr) {
            const HWND controls[] = {state->promptCombo, state->edit, state->okButton, state->cancelButton};
            for (HWND control : controls) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            }
        }

        SetFocus(state->edit);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kPromptHistoryControlId:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int index = static_cast<int>(SendMessageW(state->promptCombo, CB_GETCURSEL, 0, 0));
                if (index >= 0 && index < static_cast<int>(state->history.size())) {
                    SetWindowTextW(state->edit, state->history[static_cast<size_t>(index)].c_str());
                    SendMessageW(state->edit, EM_SETSEL, 0, -1);
                }
            }
            return 0;
        case IDOK: {
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(state->edit, text.data(), length + 1);
            text.resize(static_cast<size_t>(length));
            state->text = text;
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowSystemPromptDialog(HINSTANCE instance, HWND owner, HFONT font, std::wstring& promptText,
    const std::vector<std::wstring>& promptHistory) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PromptDialogProc;
    wc.hInstance = instance;
    wc.lpszClassName = kPromptDialogClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    PromptDialogState state{};
    state.owner = owner;
    state.font = font;
    state.text = promptText;
    state.history = promptHistory;

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int dialogWidth = 568;
    const int dialogHeight = 344;
    const int x = ownerRect.left + (((ownerRect.right - ownerRect.left) - dialogWidth) / 2);
    const int y = ownerRect.top + (((ownerRect.bottom - ownerRect.top) - dialogHeight) / 2);

    EnableWindow(owner, FALSE);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, kPromptDialogClassName, L"Temporary System Prompt",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, dialogWidth, dialogHeight, owner, nullptr, instance, &state);

    if (dialog == nullptr) {
        EnableWindow(owner, TRUE);
        return false;
    }

    MSG msg{};
    while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    SetForegroundWindow(owner);

    if (state.accepted) {
        promptText = state.text;
        return true;
    }
    return false;
}

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect{};
    rect.left = left;
    rect.top = top;
    rect.right = right;
    rect.bottom = bottom;
    return rect;
}

bool IsChineseCodePoint(unsigned int codePoint) {
    return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
        (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
        (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
        (codePoint >= 0x20000 && codePoint <= 0x2FA1F) ||
        (codePoint >= 0x30000 && codePoint <= 0x323AF);
}

unsigned int NextCodePoint(std::wstring_view text, size_t& index) {
    const unsigned int first = text[index++];
    if (first >= 0xD800 && first <= 0xDBFF && index < text.size()) {
        const unsigned int second = text[index];
        if (second >= 0xDC00 && second <= 0xDFFF) {
            ++index;
            return 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
        }
    }
    return first;
}

bool IsLanguageCharacter(unsigned int codePoint) {
    if (codePoint > 0xFFFF) {
        return false;
    }

    const wchar_t character = static_cast<wchar_t>(codePoint);
    WORD characterType = 0;
    return GetStringTypeW(CT_CTYPE1, &character, 1, &characterType) != FALSE && (characterType & C1_ALPHA) != 0;
}

size_t CountNonWhitespaceCodePoints(const std::wstring& text) {
    size_t count = 0;
    for (size_t index = 0; index < text.size();) {
        const unsigned int codePoint = NextCodePoint(text, index);
        if (codePoint > 0xFFFF || !iswspace(static_cast<wint_t>(codePoint))) {
            ++count;
        }
    }
    return count;
}
}

MainWindow::MainWindow(HINSTANCE instance, std::wstring appName, AppConfig config, std::wstring statePath, std::wstring initialInputText)
    : instance_(instance),
      appName_(std::move(appName)),
      config_(std::move(config)),
      statePath_(std::move(statePath)),
      initialInputText_(std::move(initialInputText)) {
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

LRESULT CALLBACK MainWindow::InputEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr || self->inputEditProc_ == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    if (msg == WM_PASTE) {
        const std::wstring clipboardText = ReadUnicodeTextFromClipboard(hwnd);
        if (!clipboardText.empty()) {
            const std::wstring normalized = NormalizeEditControlLineEndings(clipboardText);
            SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(normalized.c_str()));
            return 0;
        }
    }

    return CallWindowProcW(self->inputEditProc_, hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCREATE:
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    case WM_CREATE:
        return OnCreate() ? 0 : -1;
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_SIZE:
        OnSize(static_cast<UINT>(wParam), LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MOVE:
        OnMove(static_cast<int>(static_cast<short>(LOWORD(lParam))), static_cast<int>(static_cast<short>(HIWORD(lParam))));
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(inputEdit_);
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
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
    case WM_APP_DICT_LOOKUP_DONE:
        OnDictLookupDone(reinterpret_cast<DictionaryLookupResult*>(lParam));
        return 0;
    case WM_APP_DICT_LOOKUP_ERROR:
        OnDictLookupError(reinterpret_cast<DictionaryLookupError*>(lParam));
        return 0;
    case WM_COPYDATA: {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        if (cds != nullptr && cds->dwData == 1 && cds->lpData != nullptr && cds->cbData > 0) {
            const auto* data = static_cast<const wchar_t*>(cds->lpData);
            const size_t charCount = cds->cbData / sizeof(wchar_t);
            if (charCount > 0 && data[charCount - 1] == L'\0') {
                SetInputText(std::wstring(data, charCount - 1));
            } else {
                SetInputText(std::wstring(data, charCount));
            }
            SetFocus(inputEdit_);
            SetTranslating(isTranslating_, L"Loaded text from new instance.");
        }
        return TRUE;
    }
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

    systemPromptButton_ = CreateWindowExW(0, L"BUTTON", L"Prompt",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_SYSTEM_PROMPT), instance_, nullptr);

    translateButton_ = CreateWindowExW(0, L"BUTTON", L"Translate",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_TRANSLATE), instance_, nullptr);

    outputEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_OUTPUT), instance_, nullptr);

    statusStatic_ = CreateWindowExW(0, L"STATIC", L"Ready",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_STATUS), instance_, nullptr);

    if (inputEdit_ == nullptr || targetLangCombo_ == nullptr || providerCombo_ == nullptr || systemPromptButton_ == nullptr ||
        translateButton_ == nullptr ||
        outputEdit_ == nullptr || statusStatic_ == nullptr) {
        return false;
    }

    inputEditProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        inputEdit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MainWindow::InputEditProc)));
    SetWindowLongPtrW(inputEdit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

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
    int dictProviderIndex = -1;
    int defaultLlmIndex = 0;

    for (int i = 0; i < static_cast<int>(config_.llm.providers.size()); ++i) {
        const LlmProviderConfig& provider = config_.llm.providers[static_cast<size_t>(i)];
        SendMessageW(providerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(provider.displayName.c_str()));
        if (provider.sectionName == config_.llm.provider) {
            selectedProviderIndex = i;
            defaultLlmIndex = i;
        }
    }

    SendMessageW(providerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"dict.cn"));
    SendMessageW(providerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"youdao"));
    dictProviderIndex = static_cast<int>(config_.llm.providers.size()) + 
        (config_.dictionary.provider == DictionaryProvider::DictCn ? 0 : 1);

    if (!config_.llm.providers.empty()) {
        SendMessageW(providerCombo_, CB_SETCURSEL, selectedProviderIndex, 0);
    }

    config_.dictionary._defaultLlmIndex = defaultLlmIndex;
    config_.dictionary._dictStartIndex = static_cast<int>(config_.llm.providers.size());

    if (!initialInputText_.empty()) {
        SetInputText(initialInputText_);
    }

    ApplyWindowState();
    SetTranslating(false, config_.loadedPath.empty() ? L"Ready. tslt.ini not found." : L"Ready");
    SetFocus(inputEdit_);
    return true;
}

void MainWindow::OnSize(UINT sizeType, int width, int height) {
    if (sizeType != SIZE_MINIMIZED) {
        LayoutControls(width, height);
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindowState();
    }
}

void MainWindow::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    if (hdc == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);

    const HBRUSH backgroundBrush = CreateSolidBrush(RGB(247, 247, 245));
    FillRect(hdc, &client, backgroundBrush);

    const HBRUSH cardBrush = CreateSolidBrush(RGB(251, 251, 250));
    const HBRUSH cardBorderBrush = CreateSolidBrush(RGB(220, 218, 214));
    RECT inputRect{};
    RECT outputRect{};
    if (inputEdit_ != nullptr) {
        GetWindowRect(inputEdit_, &inputRect);
        MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<LPPOINT>(&inputRect), 2);
        InflateRect(&inputRect, kCardPadding, kCardPadding);
        FillRect(hdc, &inputRect, cardBrush);
        FrameRect(hdc, &inputRect, cardBorderBrush);
    }
    if (outputEdit_ != nullptr) {
        GetWindowRect(outputEdit_, &outputRect);
        MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<LPPOINT>(&outputRect), 2);
        InflateRect(&outputRect, kCardPadding, kCardPadding);
        FillRect(hdc, &outputRect, cardBrush);
        FrameRect(hdc, &outputRect, cardBorderBrush);
    }

    DeleteObject(cardBorderBrush);
    DeleteObject(cardBrush);
    DeleteObject(backgroundBrush);
    EndPaint(hwnd_, &ps);
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
    case IDC_INPUT:
        if (notifyCode == EN_CHANGE) {
            OnInputChanged();
        }
        break;
    case IDC_TARGET_LANG:
        if (notifyCode == CBN_SELCHANGE) {
            OnTargetLanguageChanged();
            SetFocus(inputEdit_);
        } else if (notifyCode == CBN_CLOSEUP) {
            SetFocus(inputEdit_);
        }
        break;
    case IDC_PROVIDER:
        if (notifyCode == CBN_SELCHANGE) {
            OnProviderChanged();
            SetFocus(inputEdit_);
        } else if (notifyCode == CBN_CLOSEUP) {
            SetFocus(inputEdit_);
        }
        break;
    case IDC_SYSTEM_PROMPT:
        if (notifyCode == BN_CLICKED) {
            OnSystemPromptClicked();
        }
        break;
    default:
        break;
    }
}

void MainWindow::OnClose() {
    if (isTranslating_) {
        AbortActiveTranslation();
    }
    UpdateWindowState();
    ConfigStore(appName_).SaveWindowState(windowState_);
    DestroyWindow(hwnd_);
}

void MainWindow::OnDestroy() {
    if (emphasisFont_ != nullptr) {
        DeleteObject(emphasisFont_);
        emphasisFont_ = nullptr;
    }
    if (uiFont_ != nullptr) {
        DeleteObject(uiFont_);
        uiFont_ = nullptr;
    }
    PostQuitMessage(0);
}

void MainWindow::OnTranslateClicked() {
    SetFocus(outputEdit_);
    lastEscapeTick_ = 0;
    if (isTranslating_) {
        AbortActiveTranslation();
        SetTranslating(true, L"Aborting...");
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

    if (config_.dictionary.provider != DictionaryProvider::None) {
        DictionaryLookupRequest request;
        request.ownerHwnd = hwnd_;
        request.provider = config_.dictionary.provider;
        request.word = TrimCopy(inputText);

        SetOutputText(L"");
        SetTranslating(true, L"Looking up in dictionary...");
        StartDictionaryLookupWorker(std::move(request));
        return;
    }

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
    request.systemPrompt = systemPromptOverride_;
    request.sourceLanguage = config_.translate.sourceLanguage;
    request.targetLanguage = config_.translate.targetLanguage;
    request.temperature = config_.translate.temperature;
    request.inputText = inputText;

    SetOutputText(L"");
    SetTranslating(true, L"Translating...");
    StartTranslateWorker(std::move(request));
}

void MainWindow::OnInputChanged() {
    if (isTranslating_) {
        return;
    }

    const std::wstring inputText = GetWindowTextCopy(inputEdit_);
    UpdateTargetLanguageForInput(inputText);

    const size_t wordCount = CountWords(inputText);
    const size_t characterCount = CountNonWhitespaceCodePoints(inputText);
    const bool useDictionary = wordCount == 1 && characterCount <= config_.dictionary.autoSelectMaxCharacters;

    if (useDictionary) {
        int targetIndex = config_.dictionary._dictStartIndex;
        if (config_.dictionary.provider == DictionaryProvider::Youdao) {
            targetIndex += 1;
        }
        int currentIndex = static_cast<int>(SendMessageW(providerCombo_, CB_GETCURSEL, 0, 0));
        if (currentIndex != targetIndex) {
            SendMessageW(providerCombo_, CB_SETCURSEL, targetIndex, 0);
            OnProviderChanged();
        }
    } else {
        if (config_.dictionary.provider != DictionaryProvider::None) {
            SendMessageW(providerCombo_, CB_SETCURSEL, config_.dictionary._defaultLlmIndex, 0);
            OnProviderChanged();
        }
    }
}

void MainWindow::UpdateTargetLanguageForInput(const std::wstring& inputText) {
    size_t chineseCharacterCount = 0;
    size_t otherLanguageCharacterCount = 0;

    for (size_t index = 0; index < inputText.size();) {
        const unsigned int codePoint = NextCodePoint(inputText, index);
        if (IsChineseCodePoint(codePoint)) {
            ++chineseCharacterCount;
        } else if (IsLanguageCharacter(codePoint)) {
            ++otherLanguageCharacterCount;
        }
    }

    if (chineseCharacterCount == 0 && otherLanguageCharacterCount == 0) {
        return;
    }

    const wchar_t* targetLanguage = chineseCharacterCount > otherLanguageCharacterCount ? L"en" : L"zh-CN";
    if (config_.translate.targetLanguage == targetLanguage) {
        return;
    }

    const int targetIndex = static_cast<int>(SendMessageW(targetLangCombo_, CB_FINDSTRINGEXACT, -1,
        reinterpret_cast<LPARAM>(targetLanguage)));
    if (targetIndex == CB_ERR) {
        return;
    }

    SendMessageW(targetLangCombo_, CB_SETCURSEL, targetIndex, 0);
    config_.translate.targetLanguage = targetLanguage;
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
    if (index == CB_ERR) {
        return;
    }

    if (index >= config_.dictionary._dictStartIndex) {
        config_.llm.provider = L"";
        if (index == config_.dictionary._dictStartIndex) {
            config_.dictionary.provider = DictionaryProvider::DictCn;
        } else {
            config_.dictionary.provider = DictionaryProvider::Youdao;
        }
    } else if (index < static_cast<int>(config_.llm.providers.size())) {
        config_.llm.provider = config_.llm.providers[static_cast<size_t>(index)].sectionName;
        config_.dictionary.provider = DictionaryProvider::None;
    }
}

void MainWindow::OnSystemPromptClicked() {
    std::wstring promptText = systemPromptOverride_;
    if (promptText.empty()) {
        promptText = L"You are a translation engine. Return only the translated text.";
    }

    ConfigStore configStore(appName_);
    const std::vector<std::wstring> promptHistory = configStore.LoadPromptHistory();
    const bool accepted = ShowSystemPromptDialog(instance_, hwnd_, uiFont_, promptText, promptHistory);
    if (accepted) {
        systemPromptOverride_ = TrimCopy(promptText);
        if (systemPromptOverride_.empty()) {
            SetTranslating(isTranslating_, L"Using default system prompt.");
        } else {
            std::vector<std::wstring> updatedHistory;
            updatedHistory.reserve(promptHistory.size() + 1);
            updatedHistory.push_back(systemPromptOverride_);
            for (const std::wstring& previousPrompt : promptHistory) {
                if (previousPrompt != systemPromptOverride_) {
                    updatedHistory.push_back(previousPrompt);
                }
            }
            configStore.SavePromptHistory(updatedHistory);
            SetTranslating(isTranslating_, L"Temporary system prompt applied.");
        }
    }

    SetFocus(inputEdit_);
}

void MainWindow::OnTranslateDone(TranslateResult* result) {
    if (result != nullptr) {
        const std::wstring text = result->translatedText;
        SetOutputText(text);
        delete result;
        if (!TrimCopy(text).empty()) {
            CopyTextToClipboard(text);
            SetTranslating(false, L"Done. Copied to clipboard.");
            return;
        }
    }
    SetTranslating(false, L"Done.");
}

void MainWindow::OnTranslateError(TranslateError* error) {
    if (error != nullptr) {
        if (error->message == L"Translation aborted.") {
            SetOutputText(L"");
            SetTranslating(false, L"Translation aborted.");
            delete error;
            return;
        }
        SetOutputText(error->message);
        SetTranslating(false, error->message);
        delete error;
        return;
    }
    SetTranslating(false, L"Translation failed.");
}

void MainWindow::OnDictLookupDone(DictionaryLookupResult* result) {
    if (result != nullptr) {
        std::wstring output;
        output += result->word;
        if (!result->phonetic.empty()) {
            output += L" " + result->phonetic;
        }
        output += L"\r\n\r\n";
        if (!result->definition.empty()) {
            output += result->definition;
        }
        if (!result->examples.empty()) {
            output += L"\r\n\r\n" + result->examples;
        }
        SetOutputText(output);
        delete result;
        if (!TrimCopy(output).empty()) {
            CopyTextToClipboard(output);
            SetTranslating(false, L"Done. Copied to clipboard.");
            return;
        }
    }
    SetTranslating(false, L"Done.");
}

void MainWindow::OnDictLookupError(DictionaryLookupError* error) {
    if (error != nullptr) {
        SetOutputText(error->message);
        SetTranslating(false, error->message);
        delete error;
        return;
    }
    SetTranslating(false, L"Dictionary lookup failed.");
}

void MainWindow::LayoutControls(int clientWidth, int clientHeight) {
    const int topY = kMargin;
    const int statusY = clientHeight - kMargin - kBottomRowHeight;
    const int contentY = topY + kTopRowHeight + kToolbarPanelPadding + kCardPadding;
    const int contentHeight = statusY - contentY - kMargin;
    const int outputHeight = (contentHeight - (kMargin + (kCardPadding * 4))) / 2;
    const int inputHeight = contentHeight - outputHeight - (kMargin + (kCardPadding * 4));

    const int comboVisibleHeight = 24;
    const int comboTop = topY + ((kTopRowHeight - comboVisibleHeight) / 2);
    const int buttonTop = topY;
    const int primaryButtonTop = topY - 1;
    const int promptButtonX = kMargin + kComboWidth + kMargin;
    const int providerX = promptButtonX + kPromptButtonWidth + kMargin;
    const int translateX = clientWidth - kMargin - kPrimaryButtonWidth;

    MoveWindow(targetLangCombo_, kMargin, comboTop, kComboWidth, 400, TRUE);
    MoveWindow(systemPromptButton_, promptButtonX, buttonTop, kPromptButtonWidth, kTopRowHeight, TRUE);
    MoveWindow(providerCombo_, providerX, comboTop, kProviderComboWidth, 400, TRUE);
    MoveWindow(translateButton_, translateX, primaryButtonTop, kPrimaryButtonWidth, kTopRowHeight + 2, TRUE);
    MoveWindow(inputEdit_, kMargin + kCardPadding, contentY, clientWidth - ((kMargin + kCardPadding) * 2), inputHeight, TRUE);
    MoveWindow(outputEdit_, kMargin + kCardPadding, contentY + inputHeight + kMargin + (kCardPadding * 2),
        clientWidth - ((kMargin + kCardPadding) * 2), outputHeight, TRUE);
    MoveWindow(statusStatic_, kMargin, statusY, clientWidth - (kMargin * 2), kBottomRowHeight, TRUE);
}

void MainWindow::SetTranslating(bool translating, const std::wstring& statusText) {
    isTranslating_ = translating;
    SetWindowTextW(translateButton_, translating ? L"Abort" : L"Translate");
    EnableWindow(translateButton_, TRUE);
    EnableWindow(targetLangCombo_, translating ? FALSE : TRUE);
    EnableWindow(providerCombo_, translating ? FALSE : TRUE);
    EnableWindow(systemPromptButton_, translating ? FALSE : TRUE);
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

    LOGFONTW emphasisLogFont = metrics.lfMessageFont;
    emphasisLogFont.lfWeight = FW_SEMIBOLD;
    emphasisLogFont.lfHeight = static_cast<LONG>(emphasisLogFont.lfHeight * 11 / 10);
    emphasisFont_ = CreateFontIndirectW(&emphasisLogFont);

    const HWND controls[] = {inputEdit_, targetLangCombo_, providerCombo_, systemPromptButton_, translateButton_, outputEdit_, statusStatic_};
    for (HWND control : controls) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
    if (emphasisFont_ != nullptr) {
        SendMessageW(translateButton_, WM_SETFONT, reinterpret_cast<WPARAM>(emphasisFont_), TRUE);
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
