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

size_t CountWords(const std::wstring& text);
void StartDictionaryLookupWorker(DictionaryLookupRequest request);

#endif
