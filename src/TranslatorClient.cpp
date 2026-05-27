#include "TranslatorClient.h"

#include <stdexcept>
#include <thread>
#include <vector>
#include <winhttp.h>

#include "Messages.h"
#include "StringUtils.h"

namespace {

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

std::wstring BuildPrompt(const TranslateRequest& request) {
    std::wstring prompt = L"Translate the following text";
    if (!request.sourceLanguage.empty() && request.sourceLanguage != L"auto") {
        prompt += L" from ";
        prompt += request.sourceLanguage;
    }
    prompt += L" to ";
    prompt += request.targetLanguage.empty() ? L"zh-CN" : request.targetLanguage;
    prompt += L". Preserve paragraph breaks, code blocks, URLs, and identifiers. Return only the translated text.\n\n";
    prompt += request.inputText;
    return prompt;
}

std::string BuildRequestBody(const TranslateRequest& request) {
    const std::wstring systemPrompt = L"You are a translation engine. Return only the translated text.";
    const std::wstring userPrompt = BuildPrompt(request);

    std::string body = "{";
    body += "\"model\":\"" + JsonEscapeUtf8(request.model) + "\",";
    body += "\"messages\":[";
    body += "{\"role\":\"system\",\"content\":\"" + JsonEscapeUtf8(systemPrompt) + "\"},";
    body += "{\"role\":\"user\",\"content\":\"" + JsonEscapeUtf8(userPrompt) + "\"}";
    body += "],";
    body += "\"temperature\":" + std::to_string(request.temperature);
    body += "}";
    return body;
}

bool ParseBaseUrl(const std::wstring& baseUrl, ParsedUrl& parsed) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);

    std::vector<wchar_t> host(256, L'\0');
    std::vector<wchar_t> path(2048, L'\0');
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUrlPath = path.data();
    components.dwUrlPathLength = static_cast<DWORD>(path.size());
    components.dwSchemeLength = 1;

    if (!WinHttpCrackUrl(baseUrl.c_str(), 0, 0, &components)) {
        return false;
    }

    parsed.host.assign(components.lpszHostName, components.dwHostNameLength);
    parsed.port = components.nPort;
    parsed.secure = components.nScheme == INTERNET_SCHEME_HTTPS;

    std::wstring basePath;
    if (components.dwUrlPathLength > 0) {
        basePath.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (basePath.empty()) {
        basePath = L"/";
    }
    if (!basePath.empty() && basePath.back() == L'/') {
        basePath.pop_back();
    }
    parsed.path = basePath + L"/chat/completions";
    return true;
}

std::wstring SendTranslationRequest(const TranslateRequest& request) {
    ParsedUrl parsed;
    if (!ParseBaseUrl(request.baseUrl, parsed)) {
        throw std::runtime_error("Failed to parse base URL.");
    }

    const std::string body = BuildRequestBody(request);

    HINTERNET session = WinHttpOpen(L"tslt/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        throw std::runtime_error("WinHttpOpen failed.");
    }

    HINTERNET connection = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
    if (connection == nullptr) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpConnect failed.");
    }

    const DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET requestHandle = WinHttpOpenRequest(connection, L"POST", parsed.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (requestHandle == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpOpenRequest failed.");
    }

    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer ";
    headers += request.apiKey;
    headers += L"\r\n";

    const BOOL sent = WinHttpSendRequest(requestHandle, headers.c_str(), static_cast<DWORD>(headers.size()),
        reinterpret_cast<LPVOID>(const_cast<char*>(body.data())), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);
    if (!sent) {
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpSendRequest failed.");
    }

    if (!WinHttpReceiveResponse(requestHandle, nullptr)) {
        WinHttpCloseHandle(requestHandle);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpReceiveResponse failed.");
    }

    std::string response;
    DWORD available = 0;
    do {
        if (!WinHttpQueryDataAvailable(requestHandle, &available)) {
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpQueryDataAvailable failed.");
        }
        if (available == 0) {
            break;
        }

        std::string chunk(available, '\0');
        DWORD downloaded = 0;
        if (!WinHttpReadData(requestHandle, chunk.data(), available, &downloaded)) {
            WinHttpCloseHandle(requestHandle);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpReadData failed.");
        }
        chunk.resize(downloaded);
        response += chunk;
    } while (available > 0);

    WinHttpCloseHandle(requestHandle);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    std::wstring content = ExtractJsonStringField(response, "content");
    if (content.empty()) {
        std::wstring message = ExtractJsonStringField(response, "message");
        if (!message.empty()) {
            throw std::runtime_error(WideToUtf8(message));
        }
        throw std::runtime_error("Translation response did not contain content.");
    }

    return content;
}

void PostError(HWND owner, const std::wstring& message) {
    auto* error = new TranslateError{message};
    if (!PostMessageW(owner, WM_APP_TRANSLATE_ERROR, 0, reinterpret_cast<LPARAM>(error))) {
        delete error;
    }
}

}

void StartTranslateWorker(TranslateRequest request) {
    std::thread([request = std::move(request)]() mutable {
        try {
            if (TrimCopy(request.baseUrl).empty() || TrimCopy(request.apiKey).empty() || TrimCopy(request.model).empty()) {
                PostError(request.ownerHwnd, L"LLM configuration is incomplete.");
                return;
            }
            if (TrimCopy(request.inputText).empty()) {
                PostError(request.ownerHwnd, L"Input text is empty.");
                return;
            }

            auto* result = new TranslateResult{SendTranslationRequest(request)};
            if (!PostMessageW(request.ownerHwnd, WM_APP_TRANSLATE_DONE, 0, reinterpret_cast<LPARAM>(result))) {
                delete result;
            }
        } catch (const std::exception& ex) {
            PostError(request.ownerHwnd, Utf8ToWide(ex.what()));
        } catch (...) {
            PostError(request.ownerHwnd, L"Unexpected translation error.");
        }
    }).detach();
}
