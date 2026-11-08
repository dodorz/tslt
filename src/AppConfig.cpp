#include "AppConfig.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <shlobj.h>
#include <vector>

#include "ConfigKeys.h"
#include "IniFile.h"
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

// Reads a text file the way a hand-maintained config is expected to be encoded:
// UTF-8 (with or without BOM), UTF-16 LE/BE with BOM, or ANSI (system code page).
bool ReadUtf8OrAnsiTextFile(const std::wstring& filePath, std::wstring& text) {
    text.clear();

    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (16 * 1024 * 1024)) {
        CloseHandle(file);
        return false;
    }

    std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
    size_t read = 0;
    bool ok = true;
    while (read < bytes.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - read, 64 * 1024));
        if (!ReadFile(file, bytes.data() + read, want, &chunk, nullptr) || chunk == 0) {
            ok = false;
            break;
        }
        read += chunk;
    }
    CloseHandle(file);
    if (!ok) {
        return false;
    }
    bytes.resize(read);

    const auto* raw = reinterpret_cast<const unsigned char*>(bytes.data());
    // Decode UTF-16 pair by pair instead of reinterpreting the buffer, so the result
    // does not depend on sizeof(wchar_t) or on host endianness.
    const auto decodeUtf16 = [&](size_t start, bool bigEndian) {
        text.reserve((bytes.size() - start) / 2);
        for (size_t index = start; index + 1 < bytes.size(); index += 2) {
            const unsigned int unit = bigEndian
                ? (static_cast<unsigned int>(raw[index]) << 8) | raw[index + 1]
                : (static_cast<unsigned int>(raw[index + 1]) << 8) | raw[index];
            text.push_back(static_cast<wchar_t>(unit));
        }
    };

    if (bytes.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        decodeUtf16(2, false);
        return true;
    }
    if (bytes.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        decodeUtf16(2, true);
        return true;
    }

    const size_t offset = bytes.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? 3 : 0;
    const std::string payload(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());

    // Decode strictly: MB_ERR_INVALID_CHARS rejects ANSI files that happen to look
    // like broken UTF-8, so they fall through to the ANSI branch instead of turning
    // into mojibake. An empty file is valid UTF-8.
    if (payload.empty()) {
        return true;
    }
    const int utf8Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, payload.data(),
        static_cast<int>(payload.size()), nullptr, 0);
    if (utf8Length > 0) {
        text.resize(static_cast<size_t>(utf8Length));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, payload.data(),
            static_cast<int>(payload.size()), text.data(), utf8Length);
        return true;
    }

    const int wideLength = MultiByteToWideChar(CP_ACP, 0, payload.data(), static_cast<int>(payload.size()), nullptr, 0);
    if (wideLength <= 0) {
        return false;
    }
    text.resize(static_cast<size_t>(wideLength));
    MultiByteToWideChar(CP_ACP, 0, payload.data(), static_cast<int>(payload.size()), text.data(), wideLength);
    return true;
}

// Reports keys that the loader never reads. Without this a mistyped key such as
// `api_style = responses` silently keeps the default protocol.
void AddUnknownKeyWarnings(const std::wstring& sectionName,
    const std::vector<std::pair<std::wstring, std::wstring>>& entries,
    const std::vector<std::wstring>& knownKeys, std::vector<std::wstring>& warnings) {
    for (const auto& [key, value] : entries) {
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
    AddUnknownKeyWarnings(L"LLM", ReadSectionEntries(config.loadedPath, L"LLM"),
        config_keys::LlmSectionKeys(), config.warnings);

    const auto sectionNames = ReadSectionNames(config.loadedPath);
    for (const std::wstring& sectionName : sectionNames) {
        if (!IsProviderSectionName(sectionName)) {
            if (LooksLikeLlmSection(sectionName)) {
                config.warnings.push_back(config_keys::DescribeIgnoredSection(
                    sectionName, SuggestLlmSectionName(sectionName)));
            }
            continue;
        }

        const auto entries = ReadSectionEntries(config.loadedPath, sectionName);
        AddUnknownKeyWarnings(sectionName, entries, config_keys::LlmProviderKeys(), config.warnings);

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

    AddUnknownKeyWarnings(L"Translate", ReadSectionEntries(config.loadedPath, L"Translate"),
        config_keys::TranslateKeys(), config.warnings);
    AddUnknownKeyWarnings(L"Dictionary", ReadSectionEntries(config.loadedPath, L"Dictionary"),
        config_keys::DictionaryKeys(), config.warnings);

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
    InvalidateDocument();
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
    InvalidateDocument();
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

// Reads and caches the ini document so every key lookup in one Load() sees the same
// content. A missing or unreadable file yields an empty document, and callers fall
// back to their defaults.
const ini::Document& ConfigStore::ConfigDocument(const std::wstring& filePath) const {
    if (documentLoaded_ && documentPath_ == filePath) {
        return document_;
    }

    document_.Clear();
    documentPath_ = filePath;
    documentLoaded_ = true;

    std::wstring text;
    if (!filePath.empty() && ReadUtf8OrAnsiTextFile(filePath, text)) {
        document_.Parse(text);
    }
    return document_;
}

void ConfigStore::InvalidateDocument() const {
    documentLoaded_ = false;
    documentPath_.clear();
}

std::wstring ConfigStore::ReadString(const std::wstring& filePath, const std::wstring& section, const std::wstring& key,
    const std::wstring& defaultValue) const {
    return ConfigDocument(filePath).Get(section, key, defaultValue);
}

std::vector<std::wstring> ConfigStore::ReadSectionNames(const std::wstring& filePath) const {
    return ConfigDocument(filePath).SectionNames();
}

std::vector<std::pair<std::wstring, std::wstring>> ConfigStore::ReadSectionEntries(
    const std::wstring& filePath, const std::wstring& section) const {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    for (const ini::Document::Entry& entry : ConfigDocument(filePath).Entries(section)) {
        entries.emplace_back(entry.key, entry.value);
    }
    return entries;
}

int ConfigStore::ReadInt(const std::wstring& filePath, const std::wstring& section, const std::wstring& key, int defaultValue) const {
    const std::wstring value = ReadString(filePath, section, key, L"");
    if (value.empty()) {
        return defaultValue;
    }
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        return consumed == value.size() ? parsed : defaultValue;
    } catch (...) {
        return defaultValue;
    }
}

double ConfigStore::ReadDouble(const std::wstring& filePath, const std::wstring& section, const std::wstring& key, double defaultValue) const {
    const std::wstring value = ReadString(filePath, section, key, L"");
    if (value.empty()) {
        return defaultValue;
    }
    try {
        size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        return consumed == value.size() ? parsed : defaultValue;
    } catch (...) {
        return defaultValue;
    }
}
