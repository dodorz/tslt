#ifndef TSLT_API_ERROR_HINTS_H
#define TSLT_API_ERROR_HINTS_H

#include <string>

// Pure helpers that classify provider error messages. Some models reject a
// request field they do not support (for example reasoning models and
// temperature); tslt answers that by retrying without the field instead of
// surfacing an error the user cannot act on.
namespace api_hints {

inline std::string LowercaseAsciiBytes(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        lowered.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
    }
    return lowered;
}

inline bool ContainsInsensitive(const std::string& haystack, const char* needle) {
    const std::string loweredNeedle = LowercaseAsciiBytes(needle);
    if (loweredNeedle.empty()) {
        return true;
    }
    return LowercaseAsciiBytes(haystack).find(loweredNeedle) != std::string::npos;
}

// True for "does not support non-default temperature", "temperature is not
// supported", "unsupported value for temperature", and similar phrasings.
inline bool RejectsTemperature(const std::string& errorMessage) {
    if (errorMessage.empty() || !ContainsInsensitive(errorMessage, "temperature")) {
        return false;
    }

    const char* const unsupportedPhrases[] = {
        "does not support",
        "doesn't support",
        "do not support",
        "not support",
        "unsupported",
        "not allowed",
        "invalid",
        "only the default",
        "non-default",
    };

    for (const char* phrase : unsupportedPhrases) {
        if (ContainsInsensitive(errorMessage, phrase)) {
            return true;
        }
    }
    return false;
}

// Only 4xx responses are retried; a 5xx or an auth failure means the request
// itself was not the problem.
inline bool ShouldRetryWithoutTemperature(int statusCode, const std::string& errorMessage) {
    if (statusCode < 400 || statusCode >= 500) {
        return false;
    }
    return RejectsTemperature(errorMessage);
}

}

#endif
