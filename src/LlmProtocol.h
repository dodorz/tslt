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
// Separators (-, _, whitespace) are ignored. Returns false for unrecognised values so
// callers can report a typo instead of silently using the default protocol.
inline bool TryParseLlmProtocol(const std::wstring& value, LlmProtocol& protocol) {
    const std::wstring normalized = NormalizeToken(value);

    if (normalized == L"anthropic" || normalized == L"anthropicmessages" || normalized == L"claude" ||
        normalized == L"messages") {
        protocol = LlmProtocol::Anthropic;
        return true;
    }
    if (normalized == L"responses" || normalized == L"response" || normalized == L"openairesponses" ||
        normalized == L"responsesapi") {
        protocol = LlmProtocol::OpenAiResponses;
        return true;
    }
    if (normalized == L"chat" || normalized == L"chatcompletions" || normalized == L"openai" ||
        normalized == L"openaichat" || normalized == L"openaichatcompletions") {
        protocol = LlmProtocol::OpenAi;
        return true;
    }
    return false;
}

// Unknown values fall back to OpenAI chat completions.
inline LlmProtocol ParseLlmProtocol(const std::wstring& value) {
    LlmProtocol protocol = LlmProtocol::OpenAi;
    TryParseLlmProtocol(value, protocol);
    return protocol;
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
// Returns false for unrecognised values so callers can report a typo.
inline bool TryParseLlmAuthStyle(const std::wstring& value, LlmAuthStyle& style) {
    const std::wstring normalized = NormalizeToken(value);
    if (normalized.empty() || normalized == L"auto") {
        style = LlmAuthStyle::Auto;
        return true;
    }
    if (normalized == L"bearer" || normalized == L"authorization" || normalized == L"authorizationbearer") {
        style = LlmAuthStyle::Bearer;
        return true;
    }
    if (normalized == L"xapikey" || normalized == L"anthropic") {
        style = LlmAuthStyle::XApiKey;
        return true;
    }
    if (normalized == L"apikey") {
        style = LlmAuthStyle::ApiKey;
        return true;
    }
    if (normalized == L"none" || normalized == L"no" || normalized == L"off" || normalized == L"empty") {
        style = LlmAuthStyle::None;
        return true;
    }
    return false;
}

inline LlmAuthStyle ParseLlmAuthStyle(const std::wstring& value) {
    LlmAuthStyle style = LlmAuthStyle::Auto;
    TryParseLlmAuthStyle(value, style);
    return style;
}

inline LlmAuthStyle ResolveLlmAuthStyle(LlmProtocol protocol, LlmAuthStyle style) {
    if (style != LlmAuthStyle::Auto) {
        return style;
    }
    return protocol == LlmProtocol::Anthropic ? LlmAuthStyle::XApiKey : LlmAuthStyle::Bearer;
}

#endif
