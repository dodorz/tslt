#include "DictionaryClient.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <winhttp.h>

#include "Messages.h"
#include "StringUtils.h"

namespace {

struct ActiveLookupState {
    std::mutex mutex;
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
};

ActiveLookupState g_activeLookup;

void RegisterActiveHandles(HINTERNET session, HINTERNET connection, HINTERNET request) {
    std::lock_guard<std::mutex> lock(g_activeLookup.mutex);
    g_activeLookup.session = session;
    g_activeLookup.connection = connection;
    g_activeLookup.request = request;
}

void BeginActiveLookup() {
    std::lock_guard<std::mutex> lock(g_activeLookup.mutex);
    g_activeLookup.session = nullptr;
    g_activeLookup.connection = nullptr;
    g_activeLookup.request = nullptr;
}

void EndActiveLookup() {
    std::lock_guard<std::mutex> lock(g_activeLookup.mutex);
    g_activeLookup.session = nullptr;
    g_activeLookup.connection = nullptr;
    g_activeLookup.request = nullptr;
}

void CloseOwnedHandle(HINTERNET handle, HINTERNET ActiveLookupState::* member) {
    if (handle == nullptr) {
        return;
    }
    bool shouldClose = false;
    {
        std::lock_guard<std::mutex> lock(g_activeLookup.mutex);
        if (g_activeLookup.*member == handle) {
            g_activeLookup.*member = nullptr;
            shouldClose = true;
        }
    }
    if (shouldClose) {
        WinHttpCloseHandle(handle);
    }
}

std::wstring UrlEncode(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value);
    std::string encoded;
    encoded.reserve(utf8.size() * 3);
    for (unsigned char ch : utf8) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", ch);
            encoded += buffer;
        }
    }
    return Utf8ToWide(encoded);
}

std::wstring FetchUrl(const std::wstring& url) {
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);

    std::vector<wchar_t> host(256, L'\0');
    std::vector<wchar_t> path(4096, L'\0');
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUrlPath = path.data();
    components.dwUrlPathLength = static_cast<DWORD>(path.size());
    components.dwSchemeLength = 1;

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        throw std::runtime_error("Failed to parse URL.");
    }

    std::wstring hostStr(components.lpszHostName, components.dwHostNameLength);
    std::wstring pathStr(components.lpszUrlPath, components.dwUrlPathLength);
    INTERNET_PORT port = components.nPort;
    bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(L"tslt-dict/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        throw std::runtime_error("WinHttpOpen failed.");
    }
    RegisterActiveHandles(session, nullptr, nullptr);

    HINTERNET connection = WinHttpConnect(session, hostStr.c_str(), port, 0);
    if (connection == nullptr) {
        CloseOwnedHandle(session, &ActiveLookupState::session);
        throw std::runtime_error("WinHttpConnect failed.");
    }
    RegisterActiveHandles(session, connection, nullptr);

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET requestHandle = WinHttpOpenRequest(connection, L"GET", pathStr.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (requestHandle == nullptr) {
        CloseOwnedHandle(connection, &ActiveLookupState::connection);
        CloseOwnedHandle(session, &ActiveLookupState::session);
        throw std::runtime_error("WinHttpOpenRequest failed.");
    }
    RegisterActiveHandles(session, connection, requestHandle);

    if (!WinHttpSendRequest(requestHandle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        CloseOwnedHandle(requestHandle, &ActiveLookupState::request);
        CloseOwnedHandle(connection, &ActiveLookupState::connection);
        CloseOwnedHandle(session, &ActiveLookupState::session);
        throw std::runtime_error("WinHttpSendRequest failed.");
    }

    if (!WinHttpReceiveResponse(requestHandle, nullptr)) {
        CloseOwnedHandle(requestHandle, &ActiveLookupState::request);
        CloseOwnedHandle(connection, &ActiveLookupState::connection);
        CloseOwnedHandle(session, &ActiveLookupState::session);
        throw std::runtime_error("WinHttpReceiveResponse failed.");
    }

    std::string response;
    DWORD available = 0;
    do {
        if (!WinHttpQueryDataAvailable(requestHandle, &available)) {
            CloseOwnedHandle(requestHandle, &ActiveLookupState::request);
            CloseOwnedHandle(connection, &ActiveLookupState::connection);
            CloseOwnedHandle(session, &ActiveLookupState::session);
            throw std::runtime_error("WinHttpQueryDataAvailable failed.");
        }
        if (available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD downloaded = 0;
        if (!WinHttpReadData(requestHandle, chunk.data(), available, &downloaded)) {
            CloseOwnedHandle(requestHandle, &ActiveLookupState::request);
            CloseOwnedHandle(connection, &ActiveLookupState::connection);
            CloseOwnedHandle(session, &ActiveLookupState::session);
            throw std::runtime_error("WinHttpReadData failed.");
        }
        chunk.resize(downloaded);
        response += chunk;
    } while (available > 0);

    CloseOwnedHandle(requestHandle, &ActiveLookupState::request);
    CloseOwnedHandle(connection, &ActiveLookupState::connection);
    CloseOwnedHandle(session, &ActiveLookupState::session);

    return Utf8ToWide(response);
}

bool IsLineBreakTag(const std::wstring& tagName) {
    static const wchar_t* const kLineBreakTags[] = {
        L"br", L"div", L"p", L"li", L"ul", L"ol", L"dl", L"dt", L"dd",
        L"tr", L"td", L"th", L"table", L"h1", L"h2", L"h3", L"h4", L"h5", L"h6", L"hr",
    };
    for (const wchar_t* tag : kLineBreakTags) {
        if (tagName == tag) {
            return true;
        }
    }
    return false;
}

std::wstring DecodeHtmlEntities(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        const wchar_t ch = text[index];
        if (ch != L'&') {
            result.push_back(ch);
            continue;
        }

        const size_t semicolon = text.find(L';', index + 1);
        if (semicolon == std::wstring::npos || semicolon - index > 10) {
            result.push_back(ch);
            continue;
        }

        const std::wstring entity = text.substr(index + 1, semicolon - index - 1);
        unsigned long decodedCode = 0;
        if (entity == L"nbsp") {
            decodedCode = L' ';
        } else if (entity == L"amp") {
            decodedCode = L'&';
        } else if (entity == L"lt") {
            decodedCode = L'<';
        } else if (entity == L"gt") {
            decodedCode = L'>';
        } else if (entity == L"quot") {
            decodedCode = L'"';
        } else if (entity == L"apos") {
            decodedCode = L'\'';
        } else if (entity.size() > 1 && entity[0] == L'#') {
            const wchar_t* digits = entity.c_str() + 1;
            int base = 10;
            if (*digits == L'x' || *digits == L'X') {
                base = 16;
                ++digits;
            }
            wchar_t* end = nullptr;
            const unsigned long code = std::wcstoul(digits, &end, base);
            if (end != nullptr && *end == L'\0' && code > 0 && code <= 0x10FFFF) {
                decodedCode = code;
            }
        }

        if (decodedCode == 0) {
            result.push_back(ch);
            continue;
        }

        if (decodedCode > 0xFFFF) {
            const unsigned long supplementary = decodedCode - 0x10000;
            result.push_back(static_cast<wchar_t>(0xD800 + (supplementary >> 10)));
            result.push_back(static_cast<wchar_t>(0xDC00 + (supplementary & 0x3FF)));
        } else {
            result.push_back(static_cast<wchar_t>(decodedCode));
        }
        index = semicolon;
    }
    return result;
}

std::wstring NormalizeText(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    bool pendingSpace = false;
    bool pendingLineBreak = false;
    for (wchar_t ch : text) {
        if (ch == L' ' || ch == L'\t' || ch == L'\v' || ch == L'\f' || ch == 0x00A0) {
            if (!result.empty() && !pendingLineBreak) {
                pendingSpace = true;
            }
            continue;
        }
        if (ch == L'\r' || ch == L'\n') {
            pendingSpace = false;
            if (!result.empty()) {
                pendingLineBreak = true;
            }
            continue;
        }
        if (pendingLineBreak) {
            result.push_back(L'\r');
            result.push_back(L'\n');
            pendingLineBreak = false;
        } else if (pendingSpace) {
            result.push_back(L' ');
        }
        pendingSpace = false;
        result.push_back(ch);
    }
    return result;
}

std::wstring StripHtmlTags(const std::wstring& html) {
    std::wstring withoutTags;
    withoutTags.reserve(html.size() + 16);

    size_t index = 0;
    while (index < html.size()) {
        const wchar_t ch = html[index];
        if (ch != L'<') {
            withoutTags.push_back(ch);
            ++index;
            continue;
        }

        const size_t tagEnd = html.find(L'>', index + 1);
        if (tagEnd == std::wstring::npos) {
            break;
        }

        std::wstring tagName;
        for (size_t i = index + 1; i < tagEnd; ++i) {
            const wchar_t tagChar = html[i];
            if (tagChar == L' ' || tagChar == L'\t' || tagChar == L'\r' || tagChar == L'\n'
                || tagChar == L'/' || tagChar == L'!') {
                if (!tagName.empty()) {
                    break;
                }
                continue;
            }
            tagName.push_back(static_cast<wchar_t>(std::towlower(tagChar)));
        }

        if (IsLineBreakTag(tagName)) {
            withoutTags.push_back(L'\n');
        }
        index = tagEnd + 1;
    }

    return NormalizeText(DecodeHtmlEntities(withoutTags));
}

std::wstring ExtractBetween(const std::wstring& html, const std::wstring& startTag, const std::wstring& endTag) {
    size_t startPos = html.find(startTag);
    if (startPos == std::wstring::npos) {
        return L"";
    }
    startPos += startTag.size();
    size_t endPos = html.find(endTag, startPos);
    if (endPos == std::wstring::npos) {
        return html.substr(startPos);
    }
    return html.substr(startPos, endPos - startPos);
}

DictionaryLookupResult ParseDictCn(const std::wstring& html, const std::wstring& word) {
    DictionaryLookupResult result;
    result.word = word;

    std::wstring phonetic = ExtractBetween(html, L"<span class='p'>", L"</span>");
    result.phonetic = StripHtmlTags(phonetic);

    std::wstring definition = ExtractBetween(html, L"<div id=\"e\">", L"</div>");
    result.definition = StripHtmlTags(definition);

    std::wstring examples = ExtractBetween(html, L"<div id=\"s\">", L"</div>");
    result.examples = StripHtmlTags(examples);

    return result;
}

std::wstring ExtractJsonStringValue(const std::wstring& json, const std::wstring& key) {
    std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) {
        return L"";
    }
    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) {
        return L"";
    }
    ++pos;
    while (pos < json.size() && json[pos] == L' ') {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != L'"') {
        return L"";
    }
    ++pos;
    size_t end = pos;
    while (end < json.size()) {
        if (json[end] == L'\\') {
            end += 2;
            continue;
        }
        if (json[end] == L'"') {
            break;
        }
        ++end;
    }
    return json.substr(pos, end - pos);
}

DictionaryLookupResult ParseYoudao(const std::wstring& json, const std::wstring& word) {
    DictionaryLookupResult result;
    result.word = word;

    size_t explainPos = json.find(L"\"explain\":\"");
    if (explainPos != std::wstring::npos) {
        size_t valStart = explainPos + 11;
        size_t valEnd = valStart;
        while (valEnd < json.size()) {
            if (json[valEnd] == L'\\') {
                valEnd += 2;
                continue;
            }
            if (json[valEnd] == L'"') break;
            ++valEnd;
        }
        result.definition = json.substr(valStart, valEnd - valStart);
    }

    return result;
}

DictionaryLookupResult LookupDictCn(const std::wstring& word) {
    std::wstring url = L"https://dict.cn/mini.php?q=" + UrlEncode(word);
    std::wstring html = FetchUrl(url);
    return ParseDictCn(html, word);
}

DictionaryLookupResult LookupYoudao(const std::wstring& word) {
    std::wstring url = L"https://dict.youdao.com/suggest?q=" + UrlEncode(word) + L"&num=1&doctype=json";
    std::wstring json = FetchUrl(url);
    return ParseYoudao(json, word);
}

std::wstring FormatDictionaryResult(const DictionaryLookupResult& result) {
    std::wstring output;
    output += result.word;
    if (!result.phonetic.empty()) {
        output += L" " + result.phonetic;
    }
    output += L"\r\n\r\n";
    if (!result.definition.empty()) {
        output += result.definition;
    }
    if (!result.examples.empty()) {
        output += L"\r\n\r\n" + result.examples;
    }
    return output;
}

void PostError(HWND owner, const std::wstring& message) {
    auto* error = new DictionaryLookupError{message};
    if (!PostMessageW(owner, WM_APP_DICT_LOOKUP_ERROR, 0, reinterpret_cast<LPARAM>(error))) {
        delete error;
    }
}

void PostResult(HWND owner, DictionaryLookupResult* result) {
    if (!PostMessageW(owner, WM_APP_DICT_LOOKUP_DONE, 0, reinterpret_cast<LPARAM>(result))) {
        delete result;
    }
}

}

size_t CountWords(const std::wstring& text) {
    size_t count = 0;
    bool inWord = false;
    for (wchar_t ch : text) {
        if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
            inWord = false;
        } else if (!inWord) {
            inWord = true;
            ++count;
        }
    }
    return count;
}

void StartDictionaryLookupWorker(DictionaryLookupRequest request) {
    std::thread([request = std::move(request)]() mutable {
        try {
            BeginActiveLookup();
            if (request.word.empty()) {
                PostError(request.ownerHwnd, L"Input text is empty.");
                EndActiveLookup();
                return;
            }

            DictionaryLookupResult* result = nullptr;
            switch (request.provider) {
            case DictionaryProvider::DictCn: {
                auto lookupResult = LookupDictCn(request.word);
                result = new DictionaryLookupResult{std::move(lookupResult)};
                break;
            }
            case DictionaryProvider::Youdao: {
                auto lookupResult = LookupYoudao(request.word);
                result = new DictionaryLookupResult{std::move(lookupResult)};
                break;
            }
            default:
                PostError(request.ownerHwnd, L"Unknown dictionary provider.");
                EndActiveLookup();
                return;
            }

            EndActiveLookup();
            PostResult(request.ownerHwnd, result);
        } catch (const std::exception& ex) {
            EndActiveLookup();
            PostError(request.ownerHwnd, Utf8ToWide(ex.what()));
        } catch (...) {
            EndActiveLookup();
            PostError(request.ownerHwnd, L"Unexpected dictionary lookup error.");
        }
    }).detach();
}
