#ifndef TSLT_CONFIG_KEYS_H
#define TSLT_CONFIG_KEYS_H

#include <algorithm>
#include <string>
#include <vector>

// Pure (Windows-free) helpers used to validate tslt.ini keys.
// Unknown keys are ignored by the reader, so the loader reports them instead of
// silently dropping a mistyped key such as `api_style`.
namespace config_keys {

inline std::wstring NormalizeKey(const std::wstring& value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch == L'-' || ch == L'_' || ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
            continue;
        }
        if (ch >= L'A' && ch <= L'Z') {
            normalized.push_back(static_cast<wchar_t>(ch - L'A' + L'a'));
            continue;
        }
        normalized.push_back(ch);
    }
    return normalized;
}

inline const std::vector<std::wstring>& LlmSectionKeys() {
    static const std::vector<std::wstring> keys = {L"provider"};
    return keys;
}

inline const std::vector<std::wstring>& LlmProviderKeys() {
    static const std::vector<std::wstring> keys = {
        L"name",
        L"base_url",
        L"api_key",
        L"model",
        L"protocol",
        L"auth",
        L"max_tokens",
        L"temperature",
    };
    return keys;
}

inline const std::vector<std::wstring>& TranslateKeys() {
    static const std::vector<std::wstring> keys = {L"source_language", L"target_language", L"temperature"};
    return keys;
}

inline const std::vector<std::wstring>& DictionaryKeys() {
    static const std::vector<std::wstring> keys = {L"provider", L"auto_select_max_characters"};
    return keys;
}

inline bool IsKnownKey(const std::vector<std::wstring>& knownKeys, const std::wstring& key) {
    const std::wstring normalized = NormalizeKey(key);
    for (const std::wstring& known : knownKeys) {
        if (NormalizeKey(known) == normalized) {
            return true;
        }
    }
    return false;
}

inline size_t EditDistance(const std::wstring& left, const std::wstring& right) {
    std::vector<size_t> previous(right.size() + 1);
    std::vector<size_t> current(right.size() + 1);
    for (size_t index = 0; index <= right.size(); ++index) {
        previous[index] = index;
    }

    for (size_t i = 1; i <= left.size(); ++i) {
        current[0] = i;
        for (size_t j = 1; j <= right.size(); ++j) {
            const size_t substitution = previous[j - 1] + (left[i - 1] == right[j - 1] ? 0 : 1);
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
        }
        previous.swap(current);
    }
    return previous[right.size()];
}

// Common misspellings seen in hand-written configs. The target is only offered
// when it is a key of the section being validated.
struct KeyAlias {
    const wchar_t* alias;
    const wchar_t* key;
};

inline const std::vector<KeyAlias>& KeyAliases() {
    static const std::vector<KeyAlias> aliases = {
        {L"api_style", L"protocol"},
        {L"api_type", L"protocol"},
        {L"api_protocol", L"protocol"},
        {L"apistyle", L"protocol"},
        {L"style", L"protocol"},
        {L"type", L"protocol"},
        {L"provider_name", L"provider"},
        {L"providername", L"provider"},
        {L"url", L"base_url"},
        {L"baseurl", L"base_url"},
        {L"base", L"base_url"},
        {L"endpoint", L"base_url"},
        {L"api_base", L"base_url"},
        {L"host", L"base_url"},
        {L"key", L"api_key"},
        {L"apikey", L"api_key"},
        {L"token", L"api_key"},
        {L"api_token", L"api_key"},
        {L"secret", L"api_key"},
        {L"model_name", L"model"},
        {L"modelname", L"model"},
        {L"auth_style", L"auth"},
        {L"auth_type", L"auth"},
        {L"authtype", L"auth"},
        {L"maxtokens", L"max_tokens"},
        {L"max_output_tokens", L"max_tokens"},
        {L"max_completion_tokens", L"max_tokens"},
    };
    return aliases;
}

// Returns the closest known key for a mistyped key, or an empty string.
inline std::wstring SuggestKey(const std::vector<std::wstring>& knownKeys, const std::wstring& key) {
    const std::wstring normalized = NormalizeKey(key);

    for (const KeyAlias& alias : KeyAliases()) {
        if (NormalizeKey(alias.alias) != normalized) {
            continue;
        }
        for (const std::wstring& known : knownKeys) {
            if (NormalizeKey(known) == NormalizeKey(alias.key)) {
                return known;
            }
        }
    }

    std::wstring best;
    size_t bestDistance = static_cast<size_t>(-1);
    for (const std::wstring& known : knownKeys) {
        const size_t distance = EditDistance(normalized, NormalizeKey(known));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = known;
        }
    }

    // Only suggest genuine typos; anything further away is likely a foreign key.
    if (!best.empty() && bestDistance <= 2 && normalized.size() >= 4) {
        return best;
    }
    return L"";
}

inline std::wstring DescribeUnknownKey(const std::wstring& sectionName, const std::wstring& key,
    const std::vector<std::wstring>& knownKeys) {
    std::wstring message = L"[" + sectionName + L"] unknown key '" + key + L"' ignored";
    const std::wstring suggestion = SuggestKey(knownKeys, key);
    if (!suggestion.empty()) {
        message += L"; did you mean '" + suggestion + L"'?";
    }
    return message;
}

inline std::wstring DescribeInvalidValue(const std::wstring& sectionName, const std::wstring& key,
    const std::wstring& value, const wchar_t* expected) {
    return L"[" + sectionName + L"] invalid " + key + L" value '" + value + L"'; expected " + expected;
}

inline std::wstring DescribeIgnoredSection(const std::wstring& sectionName, const std::wstring& suggestion) {
    std::wstring message = L"[" + sectionName + L"] section ignored";
    if (!suggestion.empty()) {
        message += L"; did you mean [" + suggestion + L"]?";
    } else {
        message += L"; providers must be named [LLM.<name>]";
    }
    return message;
}

}

#endif
