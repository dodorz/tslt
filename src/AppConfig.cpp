#include "AppConfig.h"

#include <filesystem>
#include <shlobj.h>
#include <vector>

#include "StringUtils.h"

namespace {
constexpr wchar_t kConfigFileName[] = L"tslt.ini";
constexpr wchar_t kStateFileName[] = L"state.dat";

std::wstring ResolveProviderName(const std::wstring& configuredProvider, const std::vector<LlmProviderConfig>& providers) {
    if (providers.empty()) {
        return configuredProvider;
    }

    for (const LlmProviderConfig& provider : providers) {
        if (provider.sectionName == configuredProvider) {
            return provider.sectionName;
        }
    }

    for (const LlmProviderConfig& provider : providers) {
        if (provider.displayName == configuredProvider) {
            return provider.sectionName;
        }
    }

    return providers.front().sectionName;
}
}

ConfigStore::ConfigStore(std::wstring appName)
    : appName_(std::move(appName)) {
}

AppConfig ConfigStore::Load() {
    AppConfig config;
    config.loadedPath = ResolveConfigPath();

    if (config.loadedPath.empty()) {
        config.loadedPath = GetAppDataDirectory() + L"\\" + kConfigFileName;
        return config;
    }

    config.llm.provider = ReadString(config.loadedPath, L"LLM", L"provider", L"");

    const auto sectionNames = ReadSectionNames(config.loadedPath);
    for (const std::wstring& sectionName : sectionNames) {
        constexpr wchar_t prefix[] = L"LLM.";
        if (sectionName.rfind(prefix, 0) != 0 || sectionName.size() <= std::size(prefix) - 1) {
            continue;
        }

        LlmProviderConfig provider;
        provider.sectionName = sectionName.substr(std::size(prefix) - 1);
        provider.displayName = ReadString(config.loadedPath, sectionName, L"name", L"");
        if (provider.displayName.empty()) {
            provider.displayName = provider.sectionName;
        }
        provider.baseUrl = ReadString(config.loadedPath, sectionName, L"base_url", L"");
        provider.apiKey = ReadString(config.loadedPath, sectionName, L"api_key", L"");
        provider.model = ReadString(config.loadedPath, sectionName, L"model", L"");
        config.llm.providers.push_back(std::move(provider));
    }

    if (!config.llm.providers.empty()) {
        config.llm.provider = ResolveProviderName(config.llm.provider, config.llm.providers);
    }

    config.translate.sourceLanguage = ReadString(config.loadedPath, L"Translate", L"source_language", L"auto");
    config.translate.targetLanguage = ReadString(config.loadedPath, L"Translate", L"target_language", L"zh-CN");
    config.translate.temperature = ReadDouble(config.loadedPath, L"Translate", L"temperature", 0.2);

    const std::wstring dictProvider = ReadString(config.loadedPath, L"Dictionary", L"provider", L"dict.cn");
    if (dictProvider == L"dict.cn") {
        config.dictionary.provider = DictionaryProvider::DictCn;
    } else if (dictProvider == L"youdao") {
        config.dictionary.provider = DictionaryProvider::Youdao;
    } else {
        config.dictionary.provider = DictionaryProvider::None;
    }
    config.dictionary.autoSelectThreshold = static_cast<size_t>(ReadInt(config.loadedPath, L"Dictionary", L"auto_select_threshold", 3));

    return config;
}

std::wstring ConfigStore::GetStatePath() const {
    return GetAppDataDirectory() + L"\\" + kStateFileName;
}

bool ConfigStore::SaveWindowState(const WindowState& state) const {
    const std::wstring path = GetStatePath();
    EnsureParentDirectory(path);

    const std::wstring section = L"Window";
    const auto writeInt = [&](const wchar_t* key, int value) {
        return WritePrivateProfileStringW(section.c_str(), key, std::to_wstring(value).c_str(), path.c_str()) != 0;
    };

    bool ok = true;
    ok = writeInt(L"x", state.x) && ok;
    ok = writeInt(L"y", state.y) && ok;
    ok = writeInt(L"width", state.width) && ok;
    ok = writeInt(L"height", state.height) && ok;
    ok = writeInt(L"maximized", state.maximized ? 1 : 0) && ok;
    return ok;
}

WindowState ConfigStore::LoadWindowState() const {
    WindowState state;
    const std::wstring path = GetStatePath();
    if (!FileExists(path)) {
        return state;
    }

    state.x = ReadInt(path, L"Window", L"x", state.x);
    state.y = ReadInt(path, L"Window", L"y", state.y);
    state.width = ReadInt(path, L"Window", L"width", state.width);
    state.height = ReadInt(path, L"Window", L"height", state.height);
    state.maximized = ReadInt(path, L"Window", L"maximized", 0) != 0;
    return state;
}

std::wstring ConfigStore::ResolveConfigPath() const {
    const std::wstring exePath = GetExeDirectory() + L"\\" + kConfigFileName;
    if (FileExists(exePath)) {
        return exePath;
    }

    const std::wstring appDataPath = GetAppDataDirectory() + L"\\" + kConfigFileName;
    if (FileExists(appDataPath)) {
        return appDataPath;
    }

    return L"";
}

std::wstring ConfigStore::GetAppDataDirectory() const {
    PWSTR roamingPath = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath)) && roamingPath != nullptr) {
        result = std::wstring(roamingPath) + L"\\" + appName_;
        CoTaskMemFree(roamingPath);
    }
    return result;
}

std::wstring ConfigStore::GetExeDirectory() const {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring path(buffer, buffer + length);
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, pos);
}

void ConfigStore::EnsureParentDirectory(const std::wstring& filePath) const {
    const size_t pos = filePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return;
    }
    std::filesystem::create_directories(filePath.substr(0, pos));
}

std::wstring ConfigStore::ReadString(const std::wstring& filePath, const std::wstring& section, const std::wstring& key,
    const std::wstring& defaultValue) const {
    wchar_t buffer[2048] = {};
    GetPrivateProfileStringW(section.c_str(), key.c_str(), defaultValue.c_str(), buffer, static_cast<DWORD>(std::size(buffer)), filePath.c_str());
    return TrimCopy(buffer);
}

std::vector<std::wstring> ConfigStore::ReadSectionNames(const std::wstring& filePath) const {
    std::vector<wchar_t> buffer(8192, L'\0');
    DWORD copied = GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), filePath.c_str());
    while (copied >= buffer.size() - 2) {
        buffer.resize(buffer.size() * 2, L'\0');
        copied = GetPrivateProfileSectionNamesW(buffer.data(), static_cast<DWORD>(buffer.size()), filePath.c_str());
    }

    std::vector<std::wstring> names;
    const wchar_t* current = buffer.data();
    while (*current != L'\0') {
        names.emplace_back(current);
        current += names.back().size() + 1;
    }
    return names;
}

int ConfigStore::ReadInt(const std::wstring& filePath, const std::wstring& section, const std::wstring& key, int defaultValue) const {
    return static_cast<int>(GetPrivateProfileIntW(section.c_str(), key.c_str(), defaultValue, filePath.c_str()));
}

double ConfigStore::ReadDouble(const std::wstring& filePath, const std::wstring& section, const std::wstring& key, double defaultValue) const {
    const std::wstring value = ReadString(filePath, section, key, L"");
    if (value.empty()) {
        return defaultValue;
    }
    try {
        return std::stod(value);
    } catch (...) {
        return defaultValue;
    }
}
