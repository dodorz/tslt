#include "TranslatorClient.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <winhttp.h>

#include "ApiErrorHints.h"
#include "Messages.h"
#include "Json.h"
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

// Carries the HTTP status so callers can decide whether a request is worth retrying.
class ApiError : public std::runtime_error {
public:
    ApiError(std::string message, int statusCode)
        : std::runtime_error(std::move(message)), statusCode_(statusCode) {
    }

    int statusCode() const noexcept {
        return statusCode_;
    }

private:
    int statusCode_ = 0;
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

std::string FormatJsonNumber(double value) {
    // Force a '.' decimal separator regardless of the process locale.
    char buffer[32] = {};
    const bool integral = value >= -1e15 && value <= 1e15 &&
        value == static_cast<double>(static_cast<long long>(value));
    if (integral) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    }
    return buffer;
}

std::string BuildRequestBody(const TranslateRequest& request) {
    const std::wstring systemPrompt = TrimCopy(request.systemPrompt).empty() ? GetDefaultSystemPrompt() : request.systemPrompt;
    const std::wstring userPrompt = BuildPrompt(request);

    std::string body = "{";
    body += "\"model\":\"" + JsonEscapeUtf8(request.model) + "\",";

    switch (request.protocol) {
    case LlmProtocol::Anthropic:
        body += "\"max_tokens\":" + std::to_string(std::max(1, request.maxTokens)) + ",";
        body += "\"system\":\"" + JsonEscapeUtf8(systemPrompt) + "\",";
        body += "\"messages\":[";
        body += "{\"role\":\"user\",\"content\":\"" + JsonEscapeUtf8(userPrompt) + "\"}";
        body += "],";
        break;
    case LlmProtocol::OpenAiResponses:
        body += "\"instructions\":\"" + JsonEscapeUtf8(systemPrompt) + "\",";
        body += "\"input\":\"" + JsonEscapeUtf8(userPrompt) + "\",";
        body += "\"max_output_tokens\":" + std::to_string(std::max(1, request.maxTokens)) + ",";
        break;
    case LlmProtocol::OpenAi:
    default:
        // max_tokens is intentionally not sent here: OpenAI reasoning models reject it
        // (they expect max_completion_tokens), and the previous behavior relied on defaults.
        body += "\"messages\":[";
        body += "{\"role\":\"system\",\"content\":\"" + JsonEscapeUtf8(systemPrompt) + "\"},";
        body += "{\"role\":\"user\",\"content\":\"" + JsonEscapeUtf8(userPrompt) + "\"}";
        body += "],";
        break;
    }

    if (request.temperature >= 0.0) {
        body += "\"temperature\":" + FormatJsonNumber(request.temperature) + ",";
    }
    if (!body.empty() && body.back() == ',') {
        body.pop_back();
    }
    body += "}";
    return body;
}

std::wstring BuildRequestHeaders(const TranslateRequest& request) {
    std::wstring headers = L"Content-Type: application/json\r\n";

    switch (ResolveLlmAuthStyle(request.protocol, request.authStyle)) {
    case LlmAuthStyle::Bearer:
        if (!request.apiKey.empty()) {
            headers += L"Authorization: Bearer " + request.apiKey + L"\r\n";
        }
        break;
    case LlmAuthStyle::XApiKey:
        if (!request.apiKey.empty()) {
            headers += L"x-api-key: " + request.apiKey + L"\r\n";
        }
        break;
    case LlmAuthStyle::ApiKey:
        if (!request.apiKey.empty()) {
            headers += L"api-key: " + request.apiKey + L"\r\n";
        }
        break;
    case LlmAuthStyle::None:
    case LlmAuthStyle::Auto:
    default:
        break;
    }

    if (request.protocol == LlmProtocol::Anthropic) {
        headers += L"anthropic-version: 2023-06-01\r\n";
    }
    return headers;
}

bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }
    const std::wstring tail = LowercaseAscii(value.substr(value.size() - suffix.size()));
    return tail == LowercaseAscii(suffix);
}

// Endpoint resolution keeps the historical rule that base_url usually already carries
// the version prefix (https://api.openai.com/v1, https://api.anthropic.com). A base_url
// that points at the final endpoint is used as-is. Anthropic and Responses always need
// /v1, so a bare host gets the prefix there; chat completions keeps the legacy
// behaviour of appending only /chat/completions.
std::wstring ResolveEndpointPath(const std::wstring& basePath, LlmProtocol protocol) {
    std::wstring path = basePath.empty() ? L"/" : basePath;
    while (path.size() > 1 && path.back() == L'/') {
        path.pop_back();
    }
    if (path == L"/") {
        path.clear();
    }

    switch (protocol) {
    case LlmProtocol::Anthropic:
        if (EndsWithInsensitive(path, L"/messages")) {
            return path;
        }
        if (EndsWithInsensitive(path, L"/v1")) {
            return path + L"/messages";
        }
        return path + L"/v1/messages";
    case LlmProtocol::OpenAiResponses:
        if (EndsWithInsensitive(path, L"/responses")) {
            return path;
        }
        if (EndsWithInsensitive(path, L"/v1")) {
            return path + L"/responses";
        }
        // /responses only exists under /v1, so a bare host needs the prefix.
        return path + L"/v1/responses";
    case LlmProtocol::OpenAi:
    default:
        if (EndsWithInsensitive(path, L"/chat/completions")) {
            return path;
        }
        if (EndsWithInsensitive(path, L"/v1")) {
            return path + L"/chat/completions";
        }
        return path + L"/chat/completions";
    }
}

bool ParseBaseUrl(const std::wstring& baseUrl, LlmProtocol protocol, ParsedUrl& parsed) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);

    std::vector<wchar_t> host(256, L'\0');
    std::vector<wchar_t> path(4096, L'\0');
    std::vector<wchar_t> extra(2048, L'\0');
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUrlPath = path.data();
    components.dwUrlPathLength = static_cast<DWORD>(path.size());
    components.lpszExtraInfo = extra.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extra.size());
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
    parsed.path = ResolveEndpointPath(basePath, protocol);

    // Keep base_url query strings (for example Azure's api-version) intact.
    if (components.dwExtraInfoLength > 0) {
        parsed.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    return true;
}

std::wstring ExtractContentParts(const JsonValue* content) {
    if (content == nullptr) {
        return L"";
    }
    if (content->IsString()) {
        return Utf8ToWide(content->text);
    }
    if (!content->IsArray()) {
        return L"";
    }

    std::wstring text;
    for (const JsonValue& part : content->elements) {
        if (part.IsString()) {
            text += Utf8ToWide(part.text);
            continue;
        }
        if (!part.IsObject()) {
            continue;
        }
        if (const JsonValue* type = part.Find("type"); type != nullptr && type->IsString()) {
            if (type->text != "text" && type->text != "output_text") {
                continue;
            }
        }
        if (const JsonValue* value = part.Find("text"); value != nullptr && value->IsString()) {
            text += Utf8ToWide(value->text);
        }
    }
    return text;
}

std::wstring ExtractOpenAiChatText(const JsonValue& root) {
    const JsonValue* choices = root.Find("choices");
    if (choices == nullptr || !choices->IsArray() || choices->elements.empty()) {
        return L"";
    }

    const JsonValue& choice = choices->elements.front();
    if (const JsonValue* message = choice.Find("message")) {
        if (const JsonValue* content = message->Find("content")) {
            std::wstring text = ExtractContentParts(content);
            if (!text.empty()) {
                return text;
            }
        }
        if (const JsonValue* refusal = message->Find("refusal"); refusal != nullptr && refusal->IsString()) {
            return Utf8ToWide(refusal->text);
        }
    }
    if (const JsonValue* text = choice.Find("text"); text != nullptr && text->IsString()) {
        return Utf8ToWide(text->text);
    }
    return L"";
}

std::wstring ExtractOpenAiResponsesText(const JsonValue& root) {
    // Some OpenAI-compatible servers expose a convenience output_text field.
    if (const JsonValue* outputText = root.Find("output_text")) {
        std::wstring text = ExtractContentParts(outputText);
        if (!text.empty()) {
            return text;
        }
    }

    const JsonValue* output = root.Find("output");
    if (output == nullptr || !output->IsArray()) {
        return L"";
    }

    std::wstring text;
    for (const JsonValue& item : output->elements) {
        if (!item.IsObject()) {
            continue;
        }
        if (const JsonValue* type = item.Find("type"); type != nullptr && type->IsString() && type->text != "message") {
            continue;
        }
        text += ExtractContentParts(item.Find("content"));
    }
    return text;
}

std::wstring ExtractAnthropicText(const JsonValue& root) {
    return ExtractContentParts(root.Find("content"));
}

std::wstring ExtractErrorMessage(const JsonValue& root) {
    if (const JsonValue* error = root.Find("error")) {
        if (error->IsString() && !error->text.empty()) {
            return Utf8ToWide(error->text);
        }
        if (error->IsObject()) {
            if (const JsonValue* message = error->Find("message"); message != nullptr && message->IsString() && !message->text.empty()) {
                return Utf8ToWide(message->text);
            }
        }
    }
    if (const JsonValue* message = root.Find("message"); message != nullptr && message->IsString() && !message->text.empty()) {
        return Utf8ToWide(message->text);
    }
    return L"";
}

std::wstring ExtractTranslationText(const TranslateRequest& request, const JsonValue& root) {
    switch (request.protocol) {
    case LlmProtocol::Anthropic:
        return ExtractAnthropicText(root);
    case LlmProtocol::OpenAiResponses:
        return ExtractOpenAiResponsesText(root);
    case LlmProtocol::OpenAi:
    default:
        return ExtractOpenAiChatText(root);
    }
}

std::wstring TruncateForMessage(const std::string& text) {
    constexpr size_t kMaxSnippetChars = 300;
    std::wstring value = Utf8ToWide(text);
    // Collapse whitespace so an HTML error page stays on one status line.
    std::wstring collapsed;
    collapsed.reserve(value.size());
    bool lastWasSpace = false;
    for (const wchar_t ch : value) {
        const bool isSpace = ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
        if (isSpace) {
            if (!lastWasSpace && !collapsed.empty()) {
                collapsed.push_back(L' ');
            }
            lastWasSpace = true;
            continue;
        }
        collapsed.push_back(ch);
        lastWasSpace = false;
    }
    if (collapsed.size() > kMaxSnippetChars) {
        collapsed = collapsed.substr(0, kMaxSnippetChars) + L"...";
    }
    return collapsed;
}

std::wstring DescribeFailure(const JsonValue* root, DWORD statusCode, const std::string& rawResponse) {
    if (root != nullptr) {
        std::wstring message = ExtractErrorMessage(*root);
        if (!message.empty()) {
            return message;
        }
        if (const JsonValue* status = root->Find("status"); status != nullptr && status->IsString() &&
            status->text == "incomplete") {
            if (const JsonValue* details = root->Find("incomplete_details"); details != nullptr) {
                if (const JsonValue* reason = details->Find("reason"); reason != nullptr && reason->IsString()) {
                    return L"Translation response was incomplete (" + Utf8ToWide(reason->text) + L").";
                }
            }
            return L"Translation response was incomplete.";
        }
    }
    std::wstring message = statusCode >= 400
        ? L"Translation request failed with HTTP " + std::to_wstring(statusCode) + L"."
        : L"Translation response did not contain content.";

    const std::wstring snippet = TruncateForMessage(rawResponse);
    if (!TrimCopy(snippet).empty()) {
        message += L" Server response: ";
        message += snippet;
    }
    return message;
}

std::wstring SendTranslationRequest(const TranslateRequest& request) {
    ParsedUrl parsed;
    if (!ParseBaseUrl(request.baseUrl, request.protocol, parsed)) {
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

    std::wstring headers = BuildRequestHeaders(request);

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

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(requestHandle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX)) {
        statusCode = 0;
    }

    CloseOwnedHandle(requestHandle, &ActiveTranslationState::request);
    CloseOwnedHandle(connection, &ActiveTranslationState::connection);
    CloseOwnedHandle(session, &ActiveTranslationState::session);

    JsonValue root;
    const JsonValue* document = ParseJsonDocument(response, root) ? &root : nullptr;

    std::wstring content = document != nullptr ? ExtractTranslationText(request, *document) : L"";
    if (content.empty()) {
        const std::wstring message = DescribeFailure(document, statusCode, response);
        throw ApiError(WideToUtf8(message), static_cast<int>(statusCode));
    }

    return content;
}

// Some models reject a non-default temperature (Claude Haiku 5.5, OpenAI reasoning
// models). Retry once without the field instead of failing the translation, and
// remember that for the remaining chunks.
std::wstring SendTranslationWithTemperatureFallback(TranslateRequest request, bool& temperatureOmitted) {
    if (temperatureOmitted) {
        request.temperature = -1.0;
    }

    try {
        return SendTranslationRequest(request);
    } catch (const ApiError& error) {
        if (temperatureOmitted || !api_hints::ShouldRetryWithoutTemperature(error.statusCode(), error.what())) {
            throw;
        }
    }

    temperatureOmitted = true;
    request.temperature = -1.0;
    return SendTranslationRequest(request);
}

void PostError(HWND owner, const std::wstring& message) {
    auto* error = new TranslateError{message};
    if (!PostMessageW(owner, WM_APP_TRANSLATE_ERROR, 0, reinterpret_cast<LPARAM>(error))) {
        delete error;
    }
}

std::wstring TranslateWithChunking(const TranslateRequest& request, bool& temperatureOmitted) {
    const std::vector<std::wstring> chunks = BuildTranslationChunks(request.inputText);
    if (chunks.size() == 1) {
        return SendTranslationWithTemperatureFallback(request, temperatureOmitted);
    }

    std::wstring combined;
    combined.reserve(request.inputText.size() + (request.inputText.size() / 8));

    TranslateRequest chunkRequest = request;
    for (const std::wstring& chunk : chunks) {
        if (IsTranslationCancelled()) {
            throw std::runtime_error("Translation aborted.");
        }
        chunkRequest.inputText = chunk;
        combined += SendTranslationWithTemperatureFallback(chunkRequest, temperatureOmitted);
    }

    return combined;
}

}

void StartTranslateWorker(TranslateRequest request) {
    std::thread([request = std::move(request)]() mutable {
        try {
            BeginActiveTranslation();
            const bool needsApiKey = ResolveLlmAuthStyle(request.protocol, request.authStyle) != LlmAuthStyle::None;
            if (TrimCopy(request.baseUrl).empty() || TrimCopy(request.model).empty() ||
                (needsApiKey && TrimCopy(request.apiKey).empty())) {
                PostError(request.ownerHwnd, L"LLM configuration is incomplete.");
                EndActiveTranslation();
                return;
            }
            if (TrimCopy(request.inputText).empty()) {
                PostError(request.ownerHwnd, L"Input text is empty.");
                EndActiveTranslation();
                return;
            }

            bool temperatureOmitted = false;
            const std::wstring translated = TranslateWithChunking(request, temperatureOmitted);
            auto* result = new TranslateResult{TrimLeadingLineBreaks(translated), temperatureOmitted};
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
