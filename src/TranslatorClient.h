#ifndef TSLT_TRANSLATOR_CLIENT_H
#define TSLT_TRANSLATOR_CLIENT_H

#include <string>
#include <windows.h>

#include "LlmProtocol.h"

struct TranslateRequest {
    HWND ownerHwnd = nullptr;
    std::wstring baseUrl;
    std::wstring apiKey;
    std::wstring model;
    LlmProtocol protocol = LlmProtocol::OpenAi;
    LlmAuthStyle authStyle = LlmAuthStyle::Auto;
    int maxTokens = 8192;
    std::wstring systemPrompt;
    std::wstring sourceLanguage;
    std::wstring targetLanguage;
    double temperature = 0.2;
    std::wstring inputText;
};

struct TranslateResult {
    std::wstring translatedText;
    // Set when the provider rejected the configured temperature and the request
    // was retried without it (see ApiErrorHints.h).
    bool temperatureOmitted = false;
};

struct TranslateError {
    std::wstring message;
};

void StartTranslateWorker(TranslateRequest request);
void AbortActiveTranslation();

#endif
