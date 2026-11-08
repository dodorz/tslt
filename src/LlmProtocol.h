#ifndef TSLT_LLM_PROTOCOL_H
#define TSLT_LLM_PROTOCOL_H

#include <string>

enum class LlmProtocol {
    OpenAi,
    OpenAiResponses,
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

inline std::wstring NormalizeToken(const std::wstring& value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    for (const wchar_t ch : LowercaseAscii(value)) {
        if (ch == L'-' || ch == L'_' || ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
            continue;
        }
        normalized.push_back(ch);
    }
    return normalized;
}

// Accepts "openai", "responses", "openai-responses", "anthropic", ...
// Separators (-, _, whitespace) are ignored, unknown values fall back to OpenAI chat completions.
inline LlmProtocol ParseLlmProtocol(const std::wstring& value) {
    const std::wstring normalized = NormalizeToken(value);

    if (normalized == L"anthropic" || normalized == L"anthropicmessages" || normalized == L"claude" ||
        normalized == L"messages") {
        return LlmProtocol::Anthropic;
    }
    if (normalized == L"responses" || normalized == L"response" || normalized == L"openairesponses" ||
        normalized == L"responsesapi") {
        return LlmProtocol::OpenAiResponses;
    }
    if (normalized == L"chat" || normalized == L"chatcompletions" || normalized == L"openai" ||
        normalized == L"openaichat" || normalized == L"openaichatcompletions") {
        return LlmProtocol::OpenAi;
    }
    return LlmProtocol::OpenAi;
}

enum class LlmAuthStyle {
    Auto,
    Bearer,
    XApiKey,
    ApiKey,
    None
};

// "auto" picks the style expected by the protocol; "bearer" sends
// Authorization: Bearer, "x-api-key" sends the Anthropic style header,
// "api-key" sends an Azure style api-key header, "none" sends no credentials.
inline LlmAuthStyle ParseLlmAuthStyle(const std::wstring& value) {
    const std::wstring normalized = NormalizeToken(value);
    if (normalized == L"bearer" || normalized == L"authorization" || normalized == L"authorizationbearer") {
        return LlmAuthStyle::Bearer;
    }
    if (normalized == L"xapikey" || normalized == L"anthropic") {
        return LlmAuthStyle::XApiKey;
    }
    if (normalized == L"apikey") {
        return LlmAuthStyle::ApiKey;
    }
    if (normalized == L"none" || normalized == L"no" || normalized == L"off" || normalized == L"empty") {
        return LlmAuthStyle::None;
    }
    return LlmAuthStyle::Auto;
}

inline LlmAuthStyle ResolveLlmAuthStyle(LlmProtocol protocol, LlmAuthStyle style) {
    if (style != LlmAuthStyle::Auto) {
        return style;
    }
    return protocol == LlmProtocol::Anthropic ? LlmAuthStyle::XApiKey : LlmAuthStyle::Bearer;
}

#endif
