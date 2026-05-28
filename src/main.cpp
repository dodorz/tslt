#include <windows.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

#include "AppConfig.h"
#include "MainWindow.h"
#include "StringUtils.h"

namespace {
bool HasCommandLineFlag(PCWSTR cmdLine, const wchar_t* flag) {
    const std::wstring args(cmdLine);
    const std::wstring flagStr(flag);
    const size_t pos = args.find(flagStr);
    if (pos == std::wstring::npos) {
        return false;
    }
    const size_t after = pos + flagStr.size();
    return after >= args.size() || args[after] == L' ' || args[after] == L'\t';
}

HWND FindExistingInstance() {
    return FindWindowW(MainWindow::kWindowClassName, nullptr);
}

bool ActivateWindow(HWND hwnd) {
    const DWORD foregroundThreadId = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD currentThreadId = GetCurrentThreadId();

    bool attached = false;
    if (foregroundThreadId != currentThreadId) {
        attached = AttachThreadInput(currentThreadId, foregroundThreadId, TRUE) != 0;
    }

    bool result = SetForegroundWindow(hwnd) != 0;

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }

    if (attached) {
        AttachThreadInput(currentThreadId, foregroundThreadId, FALSE);
    }

    return result;
}

bool SendTextToInstance(HWND hwnd, const std::wstring& text) {
    COPYDATASTRUCT cds{};
    cds.dwData = 1;
    cds.cbData = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(text.c_str());
    return SendMessageW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds)) != 0;
}

std::wstring ReadPipedInput() {
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    if (stdinHandle == nullptr || stdinHandle == INVALID_HANDLE_VALUE) {
        return L"";
    }

    const DWORD fileType = GetFileType(stdinHandle);
    if (fileType == FILE_TYPE_UNKNOWN || fileType == FILE_TYPE_CHAR) {
        return L"";
    }

    const int fd = _fileno(stdin);
    if (fd < 0) {
        return L"";
    }

    const int previousMode = _setmode(fd, _O_BINARY);
    if (previousMode == -1) {
        return L"";
    }

    std::vector<char> buffer;
    char chunk[4096];
    while (true) {
        const size_t read = std::fread(chunk, 1, sizeof(chunk), stdin);
        if (read > 0) {
            buffer.insert(buffer.end(), chunk, chunk + read);
        }
        if (read < sizeof(chunk)) {
            if (std::feof(stdin) || std::ferror(stdin)) {
                break;
            }
        }
    }

    _setmode(fd, previousMode);

    if (buffer.empty()) {
        return L"";
    }

    if (buffer.size() >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(buffer[0]);
        const unsigned char b1 = static_cast<unsigned char>(buffer[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
            const wchar_t* data = reinterpret_cast<const wchar_t*>(buffer.data() + 2);
            const size_t length = (buffer.size() - 2) / sizeof(wchar_t);
            return std::wstring(data, data + length);
        }
        if (b0 == 0xFE && b1 == 0xFF) {
            return L"";
        }
    }

    if (buffer.size() >= 3 &&
        static_cast<unsigned char>(buffer[0]) == 0xEF &&
        static_cast<unsigned char>(buffer[1]) == 0xBB &&
        static_cast<unsigned char>(buffer[2]) == 0xBF) {
        return Utf8ToWide(std::string(buffer.begin() + 3, buffer.end()));
    }

    return Utf8ToWide(std::string(buffer.begin(), buffer.end()));
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    const std::wstring appName = L"tslt";
    std::wstring initialInputText = ReadPipedInput();

    if (!HasCommandLineFlag(cmdLine, L"--new")) {
        HWND existing = FindExistingInstance();
        if (existing != nullptr) {
            if (!initialInputText.empty()) {
                SendTextToInstance(existing, initialInputText);
            }
            ActivateWindow(existing);
            return 0;
        }
    }

    ConfigStore configStore(appName);
    AppConfig config = configStore.Load();

    MainWindow window(instance, appName, std::move(config), configStore.GetStatePath(), std::move(initialInputText));
    if (!window.Create()) {
        MessageBoxW(nullptr, L"Failed to create main window.", L"tslt", MB_ICONERROR | MB_OK);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (window.HandleGlobalShortcut(msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
