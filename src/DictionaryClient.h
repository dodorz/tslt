#ifndef TSLT_DICTIONARY_CLIENT_H
#define TSLT_DICTIONARY_CLIENT_H

#include <string>
#include <windows.h>

#include "AppConfig.h"

struct DictionaryLookupRequest {
    HWND ownerHwnd = nullptr;
    DictionaryProvider provider = DictionaryProvider::DictCn;
    std::wstring word;
};

struct DictionaryLookupResult {
    std::wstring word;
    std::wstring phonetic;
    std::wstring definition;
    std::wstring examples;
};

struct DictionaryLookupError {
    std::wstring message;
};

// Error text used when the user aborts a lookup. MainWindow matches on it to skip the
// translate fallback, so the two sides must not drift apart.
inline constexpr wchar_t kDictionaryLookupAborted[] = L"Dictionary lookup aborted.";

void StartDictionaryLookupWorker(DictionaryLookupRequest request);
void AbortActiveDictionaryLookup();

#endif
