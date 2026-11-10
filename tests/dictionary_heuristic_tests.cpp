// Portable unit tests for the dictionary-vs-LLM thresholds.
#include <cstdlib>
#include <iostream>
#include <string>

#include "DictionaryHeuristic.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

// Cases confirmed against dict.cn/mini.php by probing it directly.
struct Case {
    const wchar_t* input;
    bool expectDictionary;
    const char* note;
};

void CheckCase(const Case& item, const dictionary_heuristic::Limits& limits) {
    const std::wstring trimmed = dictionary_heuristic::TrimLookupTerm(item.input);
    const bool dictionary = dictionary_heuristic::Classify(trimmed, limits) ==
        dictionary_heuristic::Verdict::UseDictionary;
    Check(dictionary == item.expectDictionary,
        std::string(item.note) + (item.expectDictionary ? " -> dictionary" : " -> LLM"));
}

void TestEnglishWords(const dictionary_heuristic::Limits& limits) {
    const Case cases[] = {
        {L"apple", true, "short word"},
        {L"computer", true, "word"},
        {L"internationalization", true, "20-char word hits dict.cn"},
        {L"uncharacteristically", true, "20-char adjective hits dict.cn"},
        {L"pneumonoultramicroscopicsilicovolcanoconiosis", false, "45-char monster word exceeds the bound"},
        {L"electroencephalographically", true, "27-char word is within the 32-char bound"},
        {L"applepie", true, "compound"},
        {L"123", false, "digits are not a headword"},
        {L"---", false, "punctuation is not a headword"},
    };
    for (const Case& item : cases) {
        CheckCase(item, limits);
    }
}

void TestEnglishPhrases(const dictionary_heuristic::Limits& limits) {
    const Case phrases[] = {
        {L"give up", true, "phrasal verb hits dict.cn"},
        {L"take care of", true, "3-word phrase hits"},
        {L"as a result of", true, "4-word phrase hits"},
        {L"on the other hand", true, "4-word idiom hits"},
        {L"in the middle of nowhere", true, "5-word idiom hits"},
        {L"the early bird catches the worm", true, "6-word proverb hits"},
        {L"every cloud has a silver lining", true, "6-word proverb hits"},
        {L"rome was not built in a day", false, "7 words exceeds the phrase bound"},
        {L"you can't judge a book by its cover", false, "8-word proverb misses dict.cn"},
        {L"let the cat out of the bag right now", false, "8 words exceeds the bound"},
        {L"a piece of cake for you", true, "6 words / 22 chars stays within bounds (dict.cn misses; the UI then falls back to the model)"},
    };
    for (const Case& item : phrases) {
        CheckCase(item, limits);
    }
}

void TestChinese(const dictionary_heuristic::Limits& limits) {
    const Case cases[] = {
        {L"好", true, "single character hits dict.cn"},
        {L"苹果", true, "2-character word"},
        {L"机器学习", true, "4-character term hits"},
        {L"人工智能", true, "4-character term hits"},
        {L"一举两得", true, "4-character idiom hits"},
        {L"中华人民共和国", true, "7-character proper noun hits"},
        {L"全国人民代表大会", true, "8-character noun hits"},
        {L"中国共产主义青年团", false, "9 characters exceeds the CJK bound"},
        // An unspaced 6-char CJK run is indistinguishable from a compound noun, so the
        // heuristic tries the dictionary; a miss falls back to the model.
        {L"今天天气很好", true, "6-char CJK run within the bound"},
        {L"深度学习与神经网络", false, "9 characters exceeds the CJK bound"},
        {L"我们明天一起去公园散步吧", false, "long sentence"},
    };
    for (const Case& item : cases) {
        CheckCase(item, limits);
    }
}

void TestTrailingPunctuation() {
    Check(dictionary_heuristic::TrimLookupTerm(L"apple.") == L"apple", "trailing '.' stripped");
    Check(dictionary_heuristic::TrimLookupTerm(L"apple,") == L"apple", "trailing ',' stripped");
    Check(dictionary_heuristic::TrimLookupTerm(L"  apple  \n") == L"apple", "whitespace stripped");
    Check(dictionary_heuristic::TrimLookupTerm(L"\u82f9\u679c\u3002") == L"\u82f9\u679c",
        "trailing CJK full stop stripped");
    Check(dictionary_heuristic::TrimLookupTerm(L"\u201capple\u201d") == L"apple", "smart quotes stripped");
    Check(dictionary_heuristic::TrimLookupTerm(L"(apple)") == L"apple", "parentheses stripped");
    Check(dictionary_heuristic::TrimLookupTerm(L"give up!") == L"give up", "phrase keeps its space");
    Check(dictionary_heuristic::TrimLookupTerm(L"...") == L"", "punctuation-only term is empty");

    dictionary_heuristic::Limits limits;
    const std::wstring trimmed = dictionary_heuristic::TrimLookupTerm(L"\u82f9\u679c\u3002");
    Check(dictionary_heuristic::Classify(trimmed, limits) == dictionary_heuristic::Verdict::UseDictionary,
        "苹果。 becomes a dictionary hit after trimming");
}

void TestAnalysisCounts() {
    const auto analysis = dictionary_heuristic::Analyze(L"hello world");
    Check(analysis.wordCount == 2, "two words counted");
    Check(analysis.characterCount == 10, "non-space characters counted");

    const auto chinese = dictionary_heuristic::Analyze(L"\u673a\u5668\u5b66\u4e60");
    Check(chinese.hasCjk, "CJK detected");
    Check(chinese.wordCount == 1, "Chinese input is one unspaced run");
    Check(chinese.cjkCount == 4, "four CJK code points");

    const auto emoji = dictionary_heuristic::Analyze(L"a\U0001F600b");
    Check(emoji.characterCount == 3, "a surrogate pair counts once");
}

void TestEmptyAndSeparators() {
    dictionary_heuristic::Limits limits;
    Check(dictionary_heuristic::Classify(L"", limits) == dictionary_heuristic::Verdict::UseLlm, "empty input -> LLM");
    Check(dictionary_heuristic::Classify(L"   ", limits) == dictionary_heuristic::Verdict::UseLlm, "spaces -> LLM");

    // Tabs and newlines separate words just like spaces.
    const auto analysis = dictionary_heuristic::Analyze(L"give\tup");
    Check(analysis.wordCount == 2, "tab separates words");
}

}

int main() {
    dictionary_heuristic::Limits limits;
    TestEnglishWords(limits);
    TestEnglishPhrases(limits);
    TestChinese(limits);
    TestTrailingPunctuation();
    TestAnalysisCounts();
    TestEmptyAndSeparators();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all dictionary heuristic checks passed\n";
    return EXIT_SUCCESS;
}
