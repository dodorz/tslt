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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const std::wstring appName = L"tslt";
    ConfigStore configStore(appName);
    AppConfig config = configStore.Load();
    std::wstring initialInputText = ReadPipedInput();

    MainWindow window(instance, appName, std::move(config), configStore.GetStatePath(), std::move(initialInputText));
    if (!window.Create()) {
        MessageBoxW(nullptr, L"Failed to create main window.", L"tslt", MB_ICONERROR | MB_OK);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
