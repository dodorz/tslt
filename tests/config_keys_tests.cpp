// Portable unit tests for the tslt.ini key validation helpers.
#include <cstdlib>
#include <iostream>
#include <string>

#include "ConfigKeys.h"
#include "LlmProtocol.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

std::string Narrow(const std::wstring& value) {
    std::string narrowed;
    narrowed.reserve(value.size());
    for (const wchar_t ch : value) {
        narrowed.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return narrowed;
}

void CheckSuggestion(const std::wstring& key, const std::wstring& expected, const std::string& message) {
    const std::wstring actual = config_keys::SuggestKey(config_keys::LlmProviderKeys(), key);
    Check(actual == expected, message + " (got \"" + Narrow(actual) + "\", expected \"" + Narrow(expected) + "\")");
}

void TestKnownKeys() {
    Check(config_keys::IsKnownKey(config_keys::LlmProviderKeys(), L"protocol"), "protocol is known");
    Check(config_keys::IsKnownKey(config_keys::LlmProviderKeys(), L"API_KEY"), "keys are case insensitive");
    Check(config_keys::IsKnownKey(config_keys::LlmProviderKeys(), L"base-url"), "dashes are ignored");
    Check(config_keys::IsKnownKey(config_keys::LlmProviderKeys(), L" max_tokens "), "whitespace is ignored");
    Check(!config_keys::IsKnownKey(config_keys::LlmProviderKeys(), L"api_style"), "api_style is unknown");

    Check(config_keys::IsKnownKey(config_keys::LlmSectionKeys(), L"provider"), "LLM.provider is known");
    Check(!config_keys::IsKnownKey(config_keys::LlmSectionKeys(), L"model"), "model belongs to providers");
    Check(config_keys::IsKnownKey(config_keys::TranslateKeys(), L"target_language"), "target_language is known");
    Check(config_keys::IsKnownKey(config_keys::DictionaryKeys(), L"auto_select_max_characters"), "dictionary key is known");
}

void TestSuggestions() {
    CheckSuggestion(L"api_style", L"protocol", "api_style maps to protocol");
    CheckSuggestion(L"api_type", L"protocol", "api_type maps to protocol");
    CheckSuggestion(L"maxtoken", L"max_tokens", "typo maps to max_tokens");
    CheckSuggestion(L"baseurl", L"base_url", "baseurl maps to base_url");
    CheckSuggestion(L"apikey", L"api_key", "apikey maps to api_key");
    CheckSuggestion(L"protocl", L"protocol", "edit distance typo is suggested");
    CheckSuggestion(L"temprature", L"temperature", "long key typo is suggested");
    CheckSuggestion(L"authenticaton", L"", "keys beyond the edit distance are not guessed");
    CheckSuggestion(L"completely-unrelated-thing", L"", "unrelated keys get no suggestion");
    CheckSuggestion(L"x", L"", "very short unknown keys get no suggestion");
}

void TestMessages() {
    const std::wstring unknown = config_keys::DescribeUnknownKey(L"LLM.RightGPT", L"api_style",
        config_keys::LlmProviderKeys());
    Check(unknown.find(L"LLM.RightGPT") != std::wstring::npos, "message names the section");
    Check(unknown.find(L"api_style") != std::wstring::npos, "message names the key");
    Check(unknown.find(L"protocol") != std::wstring::npos, "message suggests the key");

    const std::wstring ignored = config_keys::DescribeIgnoredSection(L"Llm.RightGPT", L"LLM.RightGPT");
    Check(ignored.find(L"Llm.RightGPT") != std::wstring::npos, "ignored section message");
    Check(ignored.find(L"LLM.RightGPT") != std::wstring::npos, "ignored section suggestion");

    const std::wstring invalid = config_keys::DescribeInvalidValue(L"LLM.RightBad", L"protocol", L"responsez",
        L"openai, responses or anthropic");
    Check(invalid.find(L"LLM.RightBad") != std::wstring::npos, "invalid value message names the section");
    Check(invalid.find(L"responsez") != std::wstring::npos, "invalid value message keeps the value");
    Check(invalid.find(L"responses") != std::wstring::npos, "invalid value message lists valid values");
}

}

int main() {
    TestKnownKeys();
    TestSuggestions();
    TestMessages();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all config key checks passed\n";
    return EXIT_SUCCESS;
}
