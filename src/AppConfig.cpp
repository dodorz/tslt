#include "AppConfig.h"

#include <algorithm>
#include <filesystem>
#include <shlobj.h>
#include <vector>

#include "ConfigKeys.h"
#include "StringUtils.h"

namespace {
constexpr wchar_t kConfigFileName[] = L"tslt.ini";
constexpr wchar_t kStateFileName[] = L"state.dat";
constexpr int kMaxPromptHistory = 20;

std::wstring EncodePrompt(const std::wstring& value) {
    std::wstring encoded;
    encoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t ch = value[index];
        if (ch == L'\\') {
            encoded += L"\\\\";
        } else if (ch == L'\r') {
            encoded += L"\\n";
            if (index + 1 < value.size() && value[index + 1] == L'\n') {
                ++index;
            }
        } else if (ch == L'\n') {
            encoded += L"\\n";
        } else {
            encoded.push_back(ch);
        }
    }
    return encoded;
}

std::wstring DecodePrompt(const std::wstring& value) {
    std::wstring decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'\\' && index + 1 < value.size()) {
            if (value[index + 1] == L'\\') {
                decoded.push_back(L'\\');
                ++index;
                continue;
            }
            if (value[index + 1] == L'n') {
                decoded.push_back(L'\n');
                ++index;
                continue;
            }
        }
        decoded.push_back(value[index]);
    }
    return decoded;
}

void AddUniquePrompt(std::vector<std::wstring>& prompts, const std::wstring& prompt) {
    if (prompt.empty() || std::find(prompts.begin(), prompts.end(), prompt) != prompts.end()) {
        return;
    }
    prompts.push_back(prompt);
}

// Reads every key of a section through the profile API so comments, value formats and
// duplicates are handled consistently with ReadString/ReadInt.
std::vector<std::pair<std::wstring, std::wstring>> ReadSectionEntries(const std::wstring& filePath,
    const std::wstring& sectionName) {
    std::vector<wchar_t> buffer(8192, L'\0');
    DWORD copied = GetPrivateProfileSectionW(sectionName.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), filePath.c_str());
    while (copied >= buffer.size() - 2) {
        buffer.resize(buffer.size() * 2, L'\0');
        copied = GetPrivateProfileSectionW(sectionName.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), filePath.c_str());
    }

    std::vector<std::pair<std::wstring, std::wstring>> entries;
    const wchar_t* current = buffer.data();
    while (*current != L'\0') {
        std::wstring line(current);
        current += line.size() + 1;

        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            entries.emplace_back(TrimCopy(line), L"");
            continue;
        }
        entries.emplace_back(TrimCopy(line.substr(0, equals)), TrimCopy(line.substr(equals + 1)));
    }
    return entries;
}

// Reports keys that the loader never reads. Without this a mistyped key such as
// `api_style = responses` silently keeps the default protocol.
void AddUnknownKeyWarnings(const std::wstring& filePath, const std::wstring& sectionName,
    const std::vector<std::wstring>& knownKeys, std::vector<std::wstring>& warnings) {
    for (const auto& [key, value] : ReadSectionEntries(filePath, sectionName)) {
        (void)value;
        if (key.empty() || config_keys::IsKnownKey(knownKeys, key)) {
            continue;
        }
        warnings.push_back(config_keys::DescribeUnknownKey(sectionName, key, knownKeys));
    }
}

// A provider section must be spelled exactly "LLM.<name>". Near misses such as
// "Llm.RightGPT" or "LLM-RightGPT" would otherwise be dropped without any feedback.
bool LooksLikeLlmSection(const std::wstring& sectionName) {
    const std::wstring lowered = config_keys::NormalizeKey(sectionName);
    if (lowered.rfind(L"llm", 0) != 0 || lowered.size() <= 3) {
        return false;
    }
    const wchar_t separator = sectionName[3];
    return separator == L'.' || separator == L'-' || separator == L'_' ||
        separator == L' ' || separator == L'\t';
}

bool IsProviderSectionName(const std::wstring& sectionName) {
    constexpr wchar_t prefix[] = L"LLM.";
    const size_t prefixLength = std::size(prefix) - 1;
    return sectionName.size() > prefixLength && sectionName.rfind(prefix, 0) == 0;
}

std::wstring SuggestLlmSectionName(const std::wstring& sectionName) {
    if (!LooksLikeLlmSection(sectionName)) {
        return L"";
    }

    std::wstring suffix = sectionName.substr(3);
    while (!suffix.empty() && (suffix.front() == L'.' || suffix.front() == L'-' || suffix.front() == L'_' ||
               suffix.front() == L' ' || suffix.front() == L'\t')) {
        suffix.erase(suffix.begin());
    }
    if (suffix.empty()) {
        return L"";
    }
    return L"LLM." + suffix;
}

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
    AddUnknownKeyWarnings(config.loadedPath, L"LLM", config_keys::LlmSectionKeys(), config.warnings);

    const auto sectionNames = ReadSectionNames(config.loadedPath);
    for (const std::wstring& sectionName : sectionNames) {
        if (!IsProviderSectionName(sectionName)) {
            if (LooksLikeLlmSection(sectionName)) {
                config.warnings.push_back(config_keys::DescribeIgnoredSection(
                    sectionName, SuggestLlmSectionName(sectionName)));
            }
            continue;
        }

        AddUnknownKeyWarnings(config.loadedPath, sectionName, config_keys::LlmProviderKeys(), config.warnings);

        LlmProviderConfig provider;
        provider.sectionName = sectionName.substr(4);
        provider.displayName = ReadString(config.loadedPath, sectionName, L"name", L"");
        if (provider.displayName.empty()) {
            provider.displayName = provider.sectionName;
        }
        provider.baseUrl = ReadString(config.loadedPath, sectionName, L"base_url", L"");
        provider.apiKey = ReadString(config.loadedPath, sectionName, L"api_key", L"");
        provider.model = ReadString(config.loadedPath, sectionName, L"model", L"");
        const std::wstring protocolValue = ReadString(config.loadedPath, sectionName, L"protocol", L"");
        if (!protocolValue.empty() && !TryParseLlmProtocol(protocolValue, provider.protocol)) {
            config.warnings.push_back(config_keys::DescribeInvalidValue(sectionName, L"protocol", protocolValue,
                L"openai, responses or anthropic"));
        }

        const std::wstring authValue = ReadString(config.loadedPath, sectionName, L"auth", L"");
        if (!authValue.empty() && !TryParseLlmAuthStyle(authValue, provider.authStyle)) {
            config.warnings.push_back(config_keys::DescribeInvalidValue(sectionName, L"auth", authValue,
                L"auto, bearer, x-api-key, api-key or none"));
        }

        provider.maxTokens = std::max(1, ReadInt(config.loadedPath, sectionName, L"max_tokens", 8192));
        provider.timeoutSeconds = std::clamp(ReadInt(config.loadedPath, sectionName, L"timeout", 300), 10, 3600);
        provider.stream = ReadInt(config.loadedPath, sectionName, L"stream", 1) != 0;

        // Optional per-provider override; a negative value omits temperature from the
        // request body, which reasoning-only models (for example gpt-5) require.
        const std::wstring temperatureValue = ReadString(config.loadedPath, sectionName, L"temperature", L"");
        if (!temperatureValue.empty()) {
            provider.hasTemperature = true;
            provider.temperature = ReadDouble(config.loadedPath, sectionName, L"temperature", provider.temperature);
        }
        config.llm.providers.push_back(std::move(provider));
    }

    if (!config.llm.providers.empty()) {
        config.llm.provider = ResolveProviderName(config.llm.provider, config.llm.providers);
    }

    AddUnknownKeyWarnings(config.loadedPath, L"Translate", config_keys::TranslateKeys(), config.warnings);
    AddUnknownKeyWarnings(config.loadedPath, L"Dictionary", config_keys::DictionaryKeys(), config.warnings);

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
    config.dictionary.autoSelectMaxCharacters = static_cast<size_t>(
        std::max(1, ReadInt(config.loadedPath, L"Dictionary", L"auto_select_max_characters", 20)));

    return config;
}

std::wstring ConfigStore::GetStatePath() const {
    return GetLocalAppDataDirectory() + L"\\" + kStateFileName;
}

std::vector<std::wstring> ConfigStore::LoadPromptHistory() const {
    const std::wstring path = GetStatePath();
    const int count = std::clamp(ReadInt(path, L"Prompts", L"count", 0), 0, kMaxPromptHistory);

    std::vector<std::wstring> prompts;
    prompts.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        const std::wstring key = L"item" + std::to_wstring(index);
        AddUniquePrompt(prompts, DecodePrompt(ReadString(path, L"Prompts", key, L"")));
    }
    return prompts;
}

bool ConfigStore::SavePromptHistory(const std::vector<std::wstring>& prompts) const {
    const std::wstring path = GetStatePath();
    EnsureParentDirectory(path);

    std::vector<std::wstring> uniquePrompts;
    uniquePrompts.reserve(std::min(prompts.size(), static_cast<size_t>(kMaxPromptHistory)));
    for (const std::wstring& prompt : prompts) {
        AddUniquePrompt(uniquePrompts, prompt);
        if (uniquePrompts.size() >= static_cast<size_t>(kMaxPromptHistory)) {
            break;
        }
    }

    const int previousCount = std::clamp(ReadInt(path, L"Prompts", L"count", 0), 0, kMaxPromptHistory);
    bool ok = true;
    for (size_t index = 0; index < uniquePrompts.size(); ++index) {
        const std::wstring key = L"item" + std::to_wstring(index);
        const std::wstring value = EncodePrompt(uniquePrompts[index]);
        ok = WritePrivateProfileStringW(L"Prompts", key.c_str(), value.c_str(), path.c_str()) != 0 && ok;
    }
    for (int index = static_cast<int>(uniquePrompts.size()); index < previousCount; ++index) {
        const std::wstring key = L"item" + std::to_wstring(index);
        ok = WritePrivateProfileStringW(L"Prompts", key.c_str(), nullptr, path.c_str()) != 0 && ok;
    }

    const std::wstring count = std::to_wstring(uniquePrompts.size());
    ok = WritePrivateProfileStringW(L"Prompts", L"count", count.c_str(), path.c_str()) != 0 && ok;
    return ok;
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

std::wstring ConfigStore::GetLocalAppDataDirectory() const {
    PWSTR localPath = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localPath)) && localPath != nullptr) {
        result = std::wstring(localPath) + L"\\" + appName_;
        CoTaskMemFree(localPath);
    }
    return result;
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
