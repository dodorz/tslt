#include <windows.h>

#include "AppConfig.h"
#include "MainWindow.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const std::wstring appName = L"tslt";
    ConfigStore configStore(appName);
    AppConfig config = configStore.Load();

    MainWindow window(instance, appName, std::move(config), configStore.GetStatePath());
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
