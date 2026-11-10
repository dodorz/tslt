#ifndef TSLT_DICTIONARY_HEURISTIC_H
#define TSLT_DICTIONARY_HEURISTIC_H

#include <string>

// Decides whether the current input is worth an online dictionary lookup, and which
// text to send. Windows-free so the thresholds can be unit tested.
//
// The defaults come from probing dict.cn/mini.php directly:
//   - single English words resolve from short entries up to ~45 characters, so a
//     generous bound is used to avoid pointless network calls for identifiers;
//   - English phrases resolve up to about six words and ~35 characters
//     ("every cloud has a silver lining" = 6 words / 31 chars hits,
//      "rome was not built in a day" = 7 words / 27 chars misses);
//   - Chinese entries resolve up to ~8 characters in practice, and commas or
//     other CJK punctuation mark a sentence boundary;
//   - trailing punctuation always misses ("苹果。" and "apple." both return nothing),
//     so the lookup term is stripped before the request.
//
// A miss is still possible inside these bounds (for example "a piece of cake for
// you" resolves only as a rewrite), which is why MainWindow falls back to the model
// when the dictionary returns no definition.
namespace dictionary_heuristic {

struct Limits {
    // Longest single Latin word treated as a dictionary lookup.
    size_t maxSingleWordChars = 32;
    // Longest multi-word phrase, in words and in characters.
    size_t maxPhraseWords = 6;
    size_t maxPhraseChars = 35;
    // Longest run of CJK text. CJK has no word separators, so the bound is in code
    // points rather than words.
    size_t maxCjkChars = 8;
};

enum class Verdict {
    // Not a plausible dictionary term: send it to the language model.
    UseLlm,
    // Looks like a dictionary term: try the dictionary first.
    UseDictionary
};

inline bool IsCjkLetter(wchar_t ch) {
    const unsigned int codePoint = static_cast<unsigned int>(ch);
    return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
        (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
        (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
        (codePoint >= 0x3040 && codePoint <= 0x30FF) ||  // Hiragana/Katakana
        (codePoint >= 0xAC00 && codePoint <= 0xD7AF) ||  // Hangul
        (codePoint >= 0xFF66 && codePoint <= 0xFF9F) ||
        (codePoint >= 0x20000 && codePoint <= 0x2FA1F) ||
        (codePoint >= 0x30000 && codePoint <= 0x323AF);
}

// CJK punctuation such as ，。！？ marks a sentence boundary, which never resolves as
// a single dictionary entry.
inline bool IsCjkPunctuation(wchar_t ch) {
    const unsigned int codePoint = static_cast<unsigned int>(ch);
    return (codePoint >= 0x3000 && codePoint <= 0x303F) ||
        (codePoint >= 0xFF01 && codePoint <= 0xFF20) ||
        (codePoint >= 0xFF3B && codePoint <= 0xFF40) ||
        (codePoint >= 0xFF5B && codePoint <= 0xFF65) ||
        codePoint == 0x2018 || codePoint == 0x2019 ||
        codePoint == 0x201C || codePoint == 0x201D ||
        codePoint == 0x2026 || codePoint == 0x2014;
}

inline bool IsLatinLetter(wchar_t ch) {
    const unsigned int codePoint = static_cast<unsigned int>(ch);
    if ((codePoint >= U'A' && codePoint <= U'Z') || (codePoint >= U'a' && codePoint <= U'z')) {
        return true;
    }
    // Latin-1 supplement and Latin Extended-A/B, excluding the math/symbol slots.
    if (codePoint >= 0xC0 && codePoint <= 0xFF) {
        return codePoint != 0xD7 && codePoint != 0xF7;
    }
    if (codePoint >= 0x100 && codePoint <= 0x24F) {
        return true;
    }
    // Greek and Cyrillic are alphabetic but not Latin; they still behave like words.
    return (codePoint >= 0x370 && codePoint <= 0x3FF) || (codePoint >= 0x400 && codePoint <= 0x4FF);
}

inline bool IsSpaceOrSeparator(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == L'\v' || ch == L'\f';
}

inline bool IsTrailingPunctuation(wchar_t ch) {
    switch (ch) {
    case L'.': case L',': case L';': case L':': case L'!': case L'?':
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF1B:  // ；
    case 0xFF1A:  // ：
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case 0x2026:  // …
    case 0x3001:  // 、
    case L'"': case L'\'': case L')': case L']': case L'}':
    case 0x201D:  // ”
    case 0x2019:  // ’
    case 0x300D:  // 」
    case 0x300F:  // 〗
    case 0xFF09:  // ）
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
        return true;
    default:
        return false;
    }
}

inline bool IsLeadingPunctuation(wchar_t ch) {
    switch (ch) {
    case L'"': case L'\'': case L'(': case L'[': case L'{':
    case 0x201C:  // “
    case 0x2018:  // ‘
    case 0x300C:  // 「
    case 0x300E:  // 〖
    case 0x300A:  // 《
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
        return true;
    default:
        return false;
    }
}

// Strips whitespace and wrapping/trailing punctuation. dict.cn returns nothing for
// "apple." or "苹果。", so the request must not carry the punctuation.
inline std::wstring TrimLookupTerm(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsSpaceOrSeparator(text[begin])) {
        ++begin;
    }
    while (end > begin && IsSpaceOrSeparator(text[end - 1])) {
        --end;
    }
    while (begin < end && IsLeadingPunctuation(text[begin])) {
        ++begin;
    }
    while (end > begin && (IsTrailingPunctuation(text[end - 1]) || IsSpaceOrSeparator(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

struct Analysis {
    size_t wordCount = 0;
    size_t characterCount = 0;   // non-separator code points
    size_t cjkCount = 0;
    size_t letterCount = 0;      // alphabetic code points of any script
    bool hasCjk = false;
};

inline Analysis Analyze(const std::wstring& text) {
    Analysis analysis;
    bool inWord = false;
    for (size_t index = 0; index < text.size(); ++index) {
        const wchar_t ch = text[index];

        const bool separator = IsSpaceOrSeparator(ch) || IsCjkPunctuation(ch);
        if (separator) {
            inWord = false;
            continue;
        }
        if (!inWord) {
            inWord = true;
            ++analysis.wordCount;
        }

        // Count a surrogate pair once.
        if (ch >= 0xD800 && ch <= 0xDBFF && index + 1 < text.size()) {
            const wchar_t next = text[index + 1];
            if (next >= 0xDC00 && next <= 0xDFFF) {
                ++index;
                ++analysis.characterCount;
                analysis.hasCjk = true;
                ++analysis.cjkCount;
                ++analysis.letterCount;
                continue;
            }
        }

        ++analysis.characterCount;
        if (IsCjkLetter(ch)) {
            analysis.hasCjk = true;
            ++analysis.cjkCount;
            ++analysis.letterCount;
        } else if (IsLatinLetter(ch)) {
            ++analysis.letterCount;
        }
    }
    return analysis;
}

inline Verdict Classify(const std::wstring& trimmedTerm, const Limits& limits) {
    const Analysis analysis = Analyze(trimmedTerm);

    // Nothing alphabetic means there is no headword to look up.
    if (analysis.letterCount == 0) {
        return Verdict::UseLlm;
    }

    // Chinese/Japanese/Korean: bounded by characters, and a CJK comma already splits
    // it into several "words", which is a sentence rather than a headword.
    if (analysis.hasCjk) {
        if (analysis.wordCount > 1) {
            return Verdict::UseLlm;
        }
        if (analysis.characterCount > limits.maxCjkChars) {
            return Verdict::UseLlm;
        }
        return Verdict::UseDictionary;
    }

    // Single Latin word: compounds are common, so allow a generous bound.
    if (analysis.wordCount <= 1) {
        return analysis.characterCount <= limits.maxSingleWordChars
            ? Verdict::UseDictionary
            : Verdict::UseLlm;
    }

    // Multi-word: idioms and phrasal verbs resolve only while they stay short.
    if (analysis.wordCount > limits.maxPhraseWords) {
        return Verdict::UseLlm;
    }
    if (analysis.characterCount > limits.maxPhraseChars) {
        return Verdict::UseLlm;
    }
    return Verdict::UseDictionary;
}

}

#endif
