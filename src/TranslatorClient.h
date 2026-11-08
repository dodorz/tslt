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
    // Receive/send timeout in seconds. Reasoning models can take minutes before the
    // first byte, and the WinHTTP default (30s) times those requests out.
    int timeoutSeconds = 300;
    // Ask for a streamed response. Non-streaming replies only start sending bytes once
    // the whole answer is generated, which trips gateway timeouts on long inputs.
    bool stream = true;
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
