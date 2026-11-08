// Portable unit tests for the tslt.ini parser (comment handling in particular).
#include <cstdlib>
#include <iostream>
#include <string>

#include "IniFile.h"

namespace {

int g_failures = 0;

void CheckEquals(const std::wstring& actual, const std::wstring& expected, const std::string& message) {
    if (actual != expected) {
        std::string a, e;
        for (wchar_t c : actual) a.push_back(c < 128 ? static_cast<char>(c) : '?');
        for (wchar_t c : expected) e.push_back(c < 128 ? static_cast<char>(c) : '?');
        std::cerr << "FAIL: " << message << " (expected \"" << e << "\", got \"" << a << "\")\n";
        ++g_failures;
    }
}

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

ini::Document Parse(const std::wstring& text) {
    ini::Document document;
    document.Parse(text);
    return document;
}

void TestInlineHashComments() {
    const auto document = Parse(
        L"[LLM.Qwen]\n"
        L"provider = Qwen   # the active provider\n"
        L"model = Qwen/Qwen3-8B#no-space-before-hash\n"
        L"stream = 1 # on\n");

    CheckEquals(document.Get(L"LLM.Qwen", L"provider", L""), L"Qwen", "comment after spaces is stripped");
    CheckEquals(document.Get(L"LLM.Qwen", L"model", L""), L"Qwen/Qwen3-8B", "# without a leading space starts a comment");
    CheckEquals(document.Get(L"LLM.Qwen", L"stream", L""), L"1", "comment after a value");
}

void TestSemicolonsAndUrls() {
    const auto document = Parse(
        L"[LLM.A]\n"
        L"api_key = sk-abc;def\n"
        L"base_url = https://example.com/v1?x=1;y=2\n"
        L"; whole line comment\n"
        L"# another whole line comment\n");

    CheckEquals(document.Get(L"LLM.A", L"api_key", L""), L"sk-abc;def", "semicolons inside values are kept");
    CheckEquals(document.Get(L"LLM.A", L"base_url", L""),
        L"https://example.com/v1?x=1;y=2", "semicolons in URLs are kept");
}

void TestQuotedValues() {
    const auto document = Parse(
        L"[LLM.A]\n"
        L"name = \"Qwen #1 (quoted)\"\n"
        L"model = \"model#with-hash\"\n"
        L"plain = \"quoted\"   # trailing comment outside quotes\n");

    CheckEquals(document.Get(L"LLM.A", L"name", L""), L"Qwen #1 (quoted)", "quoted value keeps the hash");
    CheckEquals(document.Get(L"LLM.A", L"model", L""), L"model#with-hash", "quoted model keeps the hash");
    CheckEquals(document.Get(L"LLM.A", L"plain", L""), L"quoted", "quotes are removed and comments still apply");
}

void TestSectionsAndKeys() {
    const auto document = Parse(
        L"orphan = ignored\n"
        L"[LLM.Qwen]   # inline section comment\n"
        L"name = Qwen\n"
        L"[Translate]\n"
        L"target_language = zh-CN\n"
        L"[llm.qwen]\n"
        L"name = DuplicateSection\n"
        L"[LLM.Qwen]\n"
        L"name = FirstWins\n");

    const auto names = document.SectionNames();
    Check(names.size() == 4, "sections are listed in file order");
    if (names.size() == 4) {
        CheckEquals(names[0], L"LLM.Qwen", "first section name");
        CheckEquals(names[1], L"Translate", "second section name");
    }

    CheckEquals(document.Get(L"LLM.Qwen", L"name", L""), L"Qwen", "first definition of a key wins");
    CheckEquals(document.Get(L"llm.qwen", L"NAME", L""), L"Qwen", "section and key lookup is case insensitive");
    CheckEquals(document.Get(L"Translate", L"target_language", L""), L"zh-CN", "second section value");
}

void TestDottedAndWhitespaceHandling() {
    const auto document = Parse(
        L"[LLM.DSK]\n"
        L"  base_url   =   https://api.deepseek.com   \n"
        L"empty =\n"
        L"noequals\n"
        L"[LLM.Claude]\n"
        L"max_tokens = 8192\n");

    CheckEquals(document.Get(L"LLM.DSK", L"base_url", L""), L"https://api.deepseek.com",
        "surrounding whitespace is trimmed");
    CheckEquals(document.Get(L"LLM.DSK", L"empty", L"fallback"), L"", "empty value is kept as empty");
    CheckEquals(document.Get(L"LLM.DSK", L"noequals", L""), L"", "a line without '=' becomes an empty value");
    CheckEquals(document.Get(L"LLM.Claude", L"max_tokens", L""), L"8192", "keys after a dotted section");
}

void TestCrlfAndBomLikeContent() {
    const auto document = Parse(L"[LLM.A]\r\nmodel = x\r\n");
    CheckEquals(document.Get(L"LLM.A", L"model", L""), L"x", "CRLF line endings are handled");

    const auto withBom = Parse(L"\xFEFF[LLM.A]\nmodel = y\n");
    CheckEquals(withBom.Get(L"LLM.A", L"model", L""), L"y", "a leading BOM does not break the first section");
}

void TestNormalizeValue() {
    CheckEquals(ini::NormalizeIniValue(L"  value  "), L"value", "value is trimmed");
    CheckEquals(ini::NormalizeIniValue(L"value # comment"), L"value", "trailing comment removed");
    CheckEquals(ini::NormalizeIniValue(L"\xff02value\xff02"), L"\xff02value\xff02", "non-ASCII untouched");
}

}

int main() {
    TestInlineHashComments();
    TestSemicolonsAndUrls();
    TestQuotedValues();
    TestSectionsAndKeys();
    TestDottedAndWhitespaceHandling();
    TestCrlfAndBomLikeContent();
    TestNormalizeValue();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all ini checks passed\n";
    return EXIT_SUCCESS;
}
