#include "TranslatorClient.h"

#include <algorithm>
#include <mutex>
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

struct ActiveTranslationState {
    std::mutex mutex;
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    bool cancelRequested = false;
};

ActiveTranslationState g_activeTranslation;
constexpr size_t kPreferredChunkChars = 6000;
constexpr size_t kHardSplitChars = 1800;

bool IsCancellationError(DWORD error) {
    return error == ERROR_WINHTTP_OPERATION_CANCELLED || error == ERROR_OPERATION_ABORTED;
}

void ThrowIfCancelledOr(const char* message) {
    if (const DWORD error = GetLastError(); IsCancellationError(error)) {
        throw std::runtime_error("Translation aborted.");
    }
    throw std::runtime_error(message);
}

void RegisterActiveHandles(HINTERNET session, HINTERNET connection, HINTERNET request) {
    std::lock_guard<std::mutex> lock(g_activeTranslation.mutex);
    g_activeTranslation.session = session;
    g_activeTranslation.connection = connection;
    g_activeTranslation.request = request;
}

void BeginActiveTranslation() {
    std::lock_guard<std::mutex> lock(g_activeTranslation.mutex);
    g_activeTranslation.session = nullptr;
    g_activeTranslation.connection = nullptr;
    g_activeTranslation.request = nullptr;
    g_activeTranslation.cancelRequested = false;
}

void EndActiveTranslation() {
    std::lock_guard<std::mutex> lock(g_activeTranslation.mutex);
    g_activeTranslation.session = nullptr;
    g_activeTranslation.connection = nullptr;
    g_activeTranslation.request = nullptr;
    g_activeTranslation.cancelRequested = false;
}

bool CloseOwnedHandle(HINTERNET handle, HINTERNET ActiveTranslationState::* member) {
    if (handle == nullptr) {
        return false;
    }

    bool shouldClose = false;
    {
        std::lock_guard<std::mutex> lock(g_activeTranslation.mutex);
        if (g_activeTranslation.*member == handle) {
            g_activeTranslation.*member = nullptr;
            shouldClose = true;
        }
    }

    if (shouldClose) {
        WinHttpCloseHandle(handle);
    }
    return shouldClose;
}

bool IsTranslationCancelled() {
    std::lock_guard<std::mutex> lock(g_activeTranslation.mutex);
    return g_activeTranslation.cancelRequested;
}

std::vector<std::wstring> SplitByDelimiter(const std::wstring& text, const std::wstring& delimiter) {
    std::vector<std::wstring> parts;
    if (text.empty()) {
        return parts;
    }

    size_t start = 0;
    while (start < text.size()) {
        size_t pos = text.find(delimiter, start);
        if (pos == std::wstring::npos) {
            parts.push_back(text.substr(start));
            break;
        }

        const size_t end = pos + delimiter.size();
        parts.push_back(text.substr(start, end - start));
        start = end;
    }

    return parts;
}

std::vector<std::wstring> SplitBySentence(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::wstring current;
    current.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        current.push_back(ch);

        const bool isSentenceBreak = ch == L'.' || ch == L'!' || ch == L'?' || ch == L';' ||
            ch == static_cast<wchar_t>(0x3002) || ch == static_cast<wchar_t>(0xFF01) ||
            ch == static_cast<wchar_t>(0xFF1F) || ch == static_cast<wchar_t>(0xFF1B);
        if (!isSentenceBreak) {
            continue;
        }

        while (i + 1 < text.size() && (text[i + 1] == L' ' || text[i + 1] == L'\t' || text[i + 1] == L'\r' || text[i + 1] == L'\n')) {
            current.push_back(text[i + 1]);
            ++i;
        }
        parts.push_back(current);
        current.clear();
    }

    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::vector<std::wstring> SplitByWhitespace(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::wstring current;
    current.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        current.push_back(text[i]);
        if (text[i] == L' ' || text[i] == L'\t') {
            parts.push_back(current);
            current.clear();
        }
    }

    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::vector<std::wstring> HardSplitText(const std::wstring& text, size_t maxChars) {
    std::vector<std::wstring> parts;
    for (size_t start = 0; start < text.size(); start += maxChars) {
        parts.push_back(text.substr(start, std::min(maxChars, text.size() - start)));
    }
    return parts;
}

std::vector<std::wstring> SplitOversizedUnit(const std::wstring& text, size_t maxChars) {
    if (text.size() <= maxChars) {
        return {text};
    }

    const std::vector<std::vector<std::wstring>> strategies = {
        SplitByDelimiter(text, L"\r\n"),
        SplitBySentence(text),
        SplitByWhitespace(text),
    };

    for (const auto& parts : strategies) {
        if (parts.size() <= 1) {
            continue;
        }

        bool canPack = true;
        std::vector<std::wstring> packed;
        std::wstring current;
        for (const std::wstring& part : parts) {
            if (part.size() > maxChars) {
                canPack = false;
                break;
            }
            if (!current.empty() && current.size() + part.size() > maxChars) {
                packed.push_back(current);
                current.clear();
            }
            current += part;
        }
        if (!current.empty()) {
            packed.push_back(current);
        }
        if (canPack) {
            return packed;
        }
    }

    return HardSplitText(text, kHardSplitChars);
}

std::vector<std::wstring> BuildTranslationChunks(const std::wstring& text) {
    if (text.size() <= kPreferredChunkChars) {
        return {text};
    }

    std::vector<std::wstring> chunks;
    std::wstring current;
    for (const std::wstring& paragraph : SplitByDelimiter(text, L"\r\n\r\n")) {
        if (paragraph.size() > kPreferredChunkChars) {
            for (const std::wstring& subChunk : SplitOversizedUnit(paragraph, kPreferredChunkChars)) {
                if (!current.empty()) {
                    chunks.push_back(current);
                    current.clear();
                }
                chunks.push_back(subChunk);
            }
            continue;
        }

        if (!current.empty() && current.size() + paragraph.size() > kPreferredChunkChars) {
            chunks.push_back(current);
            current.clear();
        }
        current += paragraph;
    }

    if (!current.empty()) {
        chunks.push_back(current);
    }

    return chunks.empty() ? std::vector<std::wstring>{text} : chunks;
}

std::wstring BuildPrompt(const TranslateRequest& request) {
    std::wstring prompt = L"Translate the following text";
    if (!request.sourceLanguage.empty() && request.sourceLanguage != L"auto") {
        prompt += L" from ";
        prompt += request.sourceLanguage;
    }
    prompt += L" to ";
    prompt += request.targetLanguage.empty() ? L"zh-CN" : request.targetLanguage;
    prompt += L". This input may be one chunk of a longer text. Preserve paragraph breaks, code blocks, URLs, and identifiers. Return only the translated text for this chunk.\n\n";
    prompt += request.inputText;
    return prompt;
}

std::wstring GetDefaultSystemPrompt() {
    return L"You are a translation engine. Return only the translated text.";
}

std::string BuildRequestBody(const TranslateRequest& request) {
    const std::wstring systemPrompt = TrimCopy(request.systemPrompt).empty() ? GetDefaultSystemPrompt() : request.systemPrompt;
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
    RegisterActiveHandles(session, nullptr, nullptr);

    HINTERNET connection = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
    if (connection == nullptr) {
        CloseOwnedHandle(session, &ActiveTranslationState::session);
        ThrowIfCancelledOr("WinHttpConnect failed.");
    }
    RegisterActiveHandles(session, connection, nullptr);

    const DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET requestHandle = WinHttpOpenRequest(connection, L"POST", parsed.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (requestHandle == nullptr) {
        CloseOwnedHandle(connection, &ActiveTranslationState::connection);
        CloseOwnedHandle(session, &ActiveTranslationState::session);
        ThrowIfCancelledOr("WinHttpOpenRequest failed.");
    }
    RegisterActiveHandles(session, connection, requestHandle);

    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer ";
    headers += request.apiKey;
    headers += L"\r\n";

    const BOOL sent = WinHttpSendRequest(requestHandle, headers.c_str(), static_cast<DWORD>(headers.size()),
        reinterpret_cast<LPVOID>(const_cast<char*>(body.data())), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);
    if (!sent) {
        CloseOwnedHandle(requestHandle, &ActiveTranslationState::request);
        CloseOwnedHandle(connection, &ActiveTranslationState::connection);
        CloseOwnedHandle(session, &ActiveTranslationState::session);
        ThrowIfCancelledOr("WinHttpSendRequest failed.");
    }

    if (!WinHttpReceiveResponse(requestHandle, nullptr)) {
        CloseOwnedHandle(requestHandle, &ActiveTranslationState::request);
        CloseOwnedHandle(connection, &ActiveTranslationState::connection);
        CloseOwnedHandle(session, &ActiveTranslationState::session);
        ThrowIfCancelledOr("WinHttpReceiveResponse failed.");
    }

    std::string response;
    DWORD available = 0;
    do {
        if (!WinHttpQueryDataAvailable(requestHandle, &available)) {
            CloseOwnedHandle(requestHandle, &ActiveTranslationState::request);
            CloseOwnedHandle(connection, &ActiveTranslationState::connection);
            CloseOwnedHandle(session, &ActiveTranslationState::session);
            ThrowIfCancelledOr("WinHttpQueryDataAvailable failed.");
        }
        if (available == 0) {
            break;
        }

        std::string chunk(available, '\0');
        DWORD downloaded = 0;
        if (!WinHttpReadData(requestHandle, chunk.data(), available, &downloaded)) {
            CloseOwnedHandle(requestHandle, &ActiveTranslationState::request);
            CloseOwnedHandle(connection, &ActiveTranslationState::connection);
            CloseOwnedHandle(session, &ActiveTranslationState::session);
            ThrowIfCancelledOr("WinHttpReadData failed.");
        }
        chunk.resize(downloaded);
        response += chunk;
    } while (available > 0);

    CloseOwnedHandle(requestHandle, &ActiveTranslationState::request);
    CloseOwnedHandle(connection, &ActiveTranslationState::connection);
    CloseOwnedHandle(session, &ActiveTranslationState::session);

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

std::wstring TranslateWithChunking(const TranslateRequest& request) {
    const std::vector<std::wstring> chunks = BuildTranslationChunks(request.inputText);
    if (chunks.size() == 1) {
        return SendTranslationRequest(request);
    }

    std::wstring combined;
    combined.reserve(request.inputText.size() + (request.inputText.size() / 8));

    TranslateRequest chunkRequest = request;
    for (const std::wstring& chunk : chunks) {
        if (IsTranslationCancelled()) {
            throw std::runtime_error("Translation aborted.");
        }
        chunkRequest.inputText = chunk;
        combined += SendTranslationRequest(chunkRequest);
    }

    return combined;
}

}

void StartTranslateWorker(TranslateRequest request) {
    std::thread([request = std::move(request)]() mutable {
        try {
            BeginActiveTranslation();
            if (TrimCopy(request.baseUrl).empty() || TrimCopy(request.apiKey).empty() || TrimCopy(request.model).empty()) {
                PostError(request.ownerHwnd, L"LLM configuration is incomplete.");
                EndActiveTranslation();
                return;
            }
            if (TrimCopy(request.inputText).empty()) {
                PostError(request.ownerHwnd, L"Input text is empty.");
                EndActiveTranslation();
                return;
            }

            auto* result = new TranslateResult{TranslateWithChunking(request)};
            EndActiveTranslation();
            if (!PostMessageW(request.ownerHwnd, WM_APP_TRANSLATE_DONE, 0, reinterpret_cast<LPARAM>(result))) {
                delete result;
            }
        } catch (const std::exception& ex) {
            EndActiveTranslation();
            PostError(request.ownerHwnd, Utf8ToWide(ex.what()));
        } catch (...) {
            EndActiveTranslation();
            PostError(request.ownerHwnd, L"Unexpected translation error.");
        }
    }).detach();
}

void AbortActiveTranslation() {
    HINTERNET request = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET session = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_activeTranslation.mutex);
        g_activeTranslation.cancelRequested = true;
        request = g_activeTranslation.request;
        connection = g_activeTranslation.connection;
        session = g_activeTranslation.session;
        g_activeTranslation.request = nullptr;
        g_activeTranslation.connection = nullptr;
        g_activeTranslation.session = nullptr;
    }

    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    if (connection != nullptr) {
        WinHttpCloseHandle(connection);
    }
    if (session != nullptr) {
        WinHttpCloseHandle(session);
    }
}
