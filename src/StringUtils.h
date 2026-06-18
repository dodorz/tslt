#ifndef TSLT_STRING_UTILS_H
#define TSLT_STRING_UTILS_H

#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

inline std::wstring TrimCopy(const std::wstring& value) {
    const wchar_t* whitespace = L" \t\r\n";
    const auto begin = value.find_first_not_of(whitespace);
    if (begin == std::wstring::npos) {
        return L"";
    }
    const auto end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return L"";
    }

    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

inline std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "";
    }

    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

inline std::wstring NormalizeEditControlLineEndings(const std::wstring& value) {
    std::wstring normalized;
    normalized.reserve(value.size() + 16);

    for (size_t i = 0; i < value.size(); ++i) {
        const wchar_t ch = value[i];
        if (ch == L'\r') {
            normalized.push_back(L'\r');
            if (i + 1 < value.size() && value[i + 1] == L'\n') {
                normalized.push_back(L'\n');
                ++i;
            } else {
                normalized.push_back(L'\n');
            }
            continue;
        }
        if (ch == L'\n') {
            normalized.push_back(L'\r');
            normalized.push_back(L'\n');
            continue;
        }
        normalized.push_back(ch);
    }

    return normalized;
}

inline std::string JsonEscapeUtf8(const std::wstring& value) {
    const std::string input = WideToUtf8(value);
    std::string output;
    output.reserve(input.size() + 16);

    for (unsigned char ch : input) {
        switch (ch) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20) {
                char buffer[7] = {};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                output += buffer;
            } else {
                output.push_back(static_cast<char>(ch));
            }
        }
    }

    return output;
}

inline std::wstring ExtractJsonStringField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    while (pos != std::string::npos) {
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos) {
            return L"";
        }
        ++pos;
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
            ++pos;
        }
        if (pos >= json.size() || json[pos] != '"') {
            pos = json.find(needle, pos);
            continue;
        }
        ++pos;

        std::string value;
        bool escaping = false;
        for (; pos < json.size(); ++pos) {
            const char ch = json[pos];
            if (escaping) {
                switch (ch) {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(ch); break;
                }
                escaping = false;
                continue;
            }
            if (ch == '\\') {
                escaping = true;
                continue;
            }
            if (ch == '"') {
                return Utf8ToWide(value);
            }
            value.push_back(ch);
        }
        return L"";
    }
    return L"";
}

inline std::wstring TrimLeadingLineBreaks(const std::wstring& value) {
    size_t start = 0;
    while (start < value.size() && (value[start] == L'\r' || value[start] == L'\n')) {
        ++start;
    }
    return value.substr(start);
}

inline bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline std::vector<wchar_t> ReadEditText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    return buffer;
}

#endif
