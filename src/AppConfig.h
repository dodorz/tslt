#ifndef TSLT_APP_CONFIG_H
#define TSLT_APP_CONFIG_H

#include <string>
#include <vector>
#include <windows.h>

struct LlmProviderConfig {
    std::wstring sectionName;
    std::wstring displayName;
    std::wstring baseUrl;
    std::wstring apiKey;
    std::wstring model;
};

struct LlmConfig {
    std::wstring provider;
    std::vector<LlmProviderConfig> providers;
};

struct TranslateConfig {
    std::wstring sourceLanguage = L"auto";
    std::wstring targetLanguage = L"zh-CN";
    double temperature = 0.2;
};

struct WindowState {
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int width = 960;
    int height = 720;
    bool maximized = false;
};

struct AppConfig {
    std::wstring loadedPath;
    LlmConfig llm;
    TranslateConfig translate;
};

class ConfigStore {
public:
    explicit ConfigStore(std::wstring appName);

    AppConfig Load();
    std::wstring GetStatePath() const;
    bool SaveWindowState(const WindowState& state) const;
    WindowState LoadWindowState() const;

private:
    std::wstring ResolveConfigPath() const;
    std::wstring GetAppDataDirectory() const;
    std::wstring GetExeDirectory() const;
    void EnsureParentDirectory(const std::wstring& filePath) const;

    std::wstring ReadString(const std::wstring& filePath, const std::wstring& section, const std::wstring& key,
        const std::wstring& defaultValue) const;
    std::vector<std::wstring> ReadSectionNames(const std::wstring& filePath) const;
    int ReadInt(const std::wstring& filePath, const std::wstring& section, const std::wstring& key, int defaultValue) const;
    double ReadDouble(const std::wstring& filePath, const std::wstring& section, const std::wstring& key, double defaultValue) const;

private:
    std::wstring appName_;
};

#endif
