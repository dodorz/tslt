#ifndef TSLT_INI_FILE_H
#define TSLT_INI_FILE_H

#include <string>
#include <utility>
#include <vector>

// Minimal INI reader for tslt.ini.
//
// The Windows profile API (GetPrivateProfileString) only understands ';' comments at
// the start of a line and keeps a trailing '#' inside the value, so hand-written
// configs could not annotate values. This parser adds:
//   - '#' starts a comment anywhere on the line, unless it is inside double quotes;
//   - ';' still starts a comment at the start of a line (historical INI behaviour),
//     but is kept inside values so URLs and keys survive;
//   - a value wrapped in double quotes is used verbatim (minus the quotes), which is
//     the escape hatch for values that really contain '#'.
//
// Section and key lookup is case-insensitive like the profile API. The first
// definition of a duplicated key wins, and the first section with a given name wins.
namespace ini {

inline std::wstring TrimIniWhitespace(const std::wstring& value) {
    const wchar_t* whitespace = L" \t\r\n";
    const size_t begin = value.find_first_not_of(whitespace);
    if (begin == std::wstring::npos) {
        return L"";
    }
    const size_t end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

inline wchar_t FoldAsciiCase(wchar_t ch) {
    return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
}

inline bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (FoldAsciiCase(left[index]) != FoldAsciiCase(right[index])) {
            return false;
        }
    }
    return true;
}

// Strips a trailing comment from a raw value and removes one level of double quotes.
inline std::wstring NormalizeIniValue(const std::wstring& rawValue) {
    bool insideQuotes = false;
    size_t commentStart = std::wstring::npos;
    for (size_t index = 0; index < rawValue.size(); ++index) {
        const wchar_t ch = rawValue[index];
        if (ch == L'"') {
            insideQuotes = !insideQuotes;
            continue;
        }
        if (!insideQuotes && ch == L'#') {
            commentStart = index;
            break;
        }
    }

    std::wstring value = TrimIniWhitespace(
        commentStart == std::wstring::npos ? rawValue : rawValue.substr(0, commentStart));
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

class Document {
public:
    struct Entry {
        std::wstring key;
        std::wstring value;
    };

    void Clear() {
        sections_.clear();
        currentSection_ = kNoSection;
    }

    // Returns false only for an unreadable document; individual malformed lines are
    // skipped so one typo does not discard the whole configuration.
    bool Parse(const std::wstring& text) {
        Clear();

        size_t position = 0;
        // A UTF-8 BOM decoded to U+FEFF would otherwise hide the first section.
        if (!text.empty() && text.front() == L'\uFEFF') {
            position = 1;
        }
        while (true) {
            const size_t newline = text.find(L'\n', position);
            std::wstring line = newline == std::wstring::npos
                ? text.substr(position)
                : text.substr(position, newline - position);
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }
            ParseLine(line);

            if (newline == std::wstring::npos) {
                break;
            }
            position = newline + 1;
        }
        return true;
    }

    std::vector<std::wstring> SectionNames() const {
        std::vector<std::wstring> names;
        names.reserve(sections_.size());
        for (const auto& section : sections_) {
            names.push_back(section.first);
        }
        return names;
    }

    std::vector<Entry> Entries(const std::wstring& section) const {
        const size_t index = FindSection(section);
        if (index == kNoSection) {
            return {};
        }
        return sections_[index].second;
    }

    bool TryGet(const std::wstring& section, const std::wstring& key, std::wstring& value) const {
        for (const Entry& entry : Entries(section)) {
            if (EqualsIgnoreCase(entry.key, key)) {
                value = entry.value;
                return true;
            }
        }
        return false;
    }

    std::wstring Get(const std::wstring& section, const std::wstring& key, const std::wstring& defaultValue) const {
        std::wstring value;
        return TryGet(section, key, value) ? value : defaultValue;
    }

private:
    static constexpr size_t kNoSection = static_cast<size_t>(-1);

    size_t FindSection(const std::wstring& section) const {
        for (size_t index = 0; index < sections_.size(); ++index) {
            if (EqualsIgnoreCase(sections_[index].first, section)) {
                return index;
            }
        }
        return kNoSection;
    }

    void ParseLine(const std::wstring& rawLine) {
        const std::wstring line = TrimIniWhitespace(rawLine);
        if (line.empty()) {
            return;
        }
        // '#' and ';' both comment out a whole line.
        if (line.front() == L'#' || line.front() == L';') {
            return;
        }

        if (line.front() == L'[') {
            const size_t close = line.find(L']');
            if (close == std::wstring::npos) {
                return;
            }
            const std::wstring name = TrimIniWhitespace(line.substr(1, close - 1));
            if (name.empty()) {
                return;
            }
            sections_.emplace_back(name, std::vector<Entry>{});
            currentSection_ = sections_.size() - 1;
            return;
        }

        // Keys outside a section are ignored, matching the profile API.
        if (currentSection_ == kNoSection) {
            return;
        }

        const size_t equals = line.find(L'=');
        const std::wstring key = TrimIniWhitespace(equals == std::wstring::npos ? line : line.substr(0, equals));
        if (key.empty()) {
            return;
        }

        std::vector<Entry>& entries = sections_[currentSection_].second;
        for (const Entry& existing : entries) {
            if (EqualsIgnoreCase(existing.key, key)) {
                return;
            }
        }
        entries.push_back(Entry{key, equals == std::wstring::npos ? L"" : NormalizeIniValue(line.substr(equals + 1))});
    }

    std::vector<std::pair<std::wstring, std::vector<Entry>>> sections_;
    size_t currentSection_ = kNoSection;
};

}

#endif
