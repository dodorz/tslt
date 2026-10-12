#ifndef TSLT_LLM_PROTOCOL_H
#define TSLT_LLM_PROTOCOL_H

#include <string>

enum class LlmProtocol {
    OpenAi,
    Anthropic
};

inline std::wstring LowercaseAscii(const std::wstring& value) {
    std::wstring lowered;
    lowered.reserve(value.size());
    for (const wchar_t ch : value) {
        lowered.push_back(ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a') : ch);
    }
    return lowered;
}

inline LlmProtocol ParseLlmProtocol(const std::wstring& value) {
    if (LowercaseAscii(value) == L"anthropic") {
        return LlmProtocol::Anthropic;
    }
    return LlmProtocol::OpenAi;
}

#endif
