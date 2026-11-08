#ifndef TSLT_APP_CONFIG_H
#define TSLT_APP_CONFIG_H

#include <string>
#include <vector>
#include <windows.h>

#include "LlmProtocol.h"
#include "IniFile.h"

struct LlmProviderConfig {
    std::wstring sectionName;
    std::wstring displayName;
    std::wstring baseUrl;
    std::wstring apiKey;
    std::wstring model;
    LlmProtocol protocol = LlmProtocol::OpenAi;
    LlmAuthStyle authStyle = LlmAuthStyle::Auto;
    int maxTokens = 8192;
    // Send/receive timeout in seconds; WinHTTP defaults to 30s, which is too short for
    // models that produce reasoning tokens before the first output byte.
    int timeoutSeconds = 300;
    // Stream the response (default on) so long generations do not hit gateway timeouts.
    bool stream = true;
    bool hasTemperature = false;
    double temperature = 0.2;
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

enum class DictionaryProvider {
    DictCn,
    Youdao,
    None
};

struct DictionaryConfig {
    DictionaryProvider provider = DictionaryProvider::DictCn;
    size_t autoSelectMaxCharacters = 20;
    int _defaultLlmIndex = 0;
    int _dictStartIndex = 0;
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
    DictionaryConfig dictionary;
    // Non-fatal configuration problems (unknown keys, misplaced sections) found while loading.
    std::vector<std::wstring> warnings;
};

class ConfigStore {
public:
    explicit ConfigStore(std::wstring appName);

    AppConfig Load();
    std::wstring GetStatePath() const;
    std::vector<std::wstring> LoadPromptHistory() const;
    bool SavePromptHistory(const std::vector<std::wstring>& prompts) const;
    bool SaveWindowState(const WindowState& state) const;
    WindowState LoadWindowState() const;

private:
    std::wstring ResolveConfigPath() const;
    std::wstring GetLocalAppDataDirectory() const;
    std::wstring GetAppDataDirectory() const;
    std::wstring GetExeDirectory() const;
    void EnsureParentDirectory(const std::wstring& filePath) const;

    // Parses (and caches) the ini document so every key lookup sees the same values.
    const ini::Document& ConfigDocument(const std::wstring& filePath) const;
    void InvalidateDocument() const;

    std::wstring ReadString(const std::wstring& filePath, const std::wstring& section, const std::wstring& key,
        const std::wstring& defaultValue) const;
    std::vector<std::wstring> ReadSectionNames(const std::wstring& filePath) const;
    std::vector<std::pair<std::wstring, std::wstring>> ReadSectionEntries(
        const std::wstring& filePath, const std::wstring& section) const;
    int ReadInt(const std::wstring& filePath, const std::wstring& section, const std::wstring& key, int defaultValue) const;
    double ReadDouble(const std::wstring& filePath, const std::wstring& section, const std::wstring& key, double defaultValue) const;

private:
    std::wstring appName_;
    mutable ini::Document document_;
    mutable std::wstring documentPath_;
    mutable bool documentLoaded_ = false;
};

#endif
