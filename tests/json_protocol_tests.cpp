// Portable unit tests for the JSON parser and LLM protocol helpers.
// Both headers are Windows-free so these tests run on any host.
#include <cstdlib>
#include <iostream>
#include <string>

#include "Json.h"
#include "LlmProtocol.h"

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

void CheckEquals(const std::string& actual, const std::string& expected, const char* message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " (expected \"" << expected << "\", got \"" << actual << "\")\n";
        ++g_failures;
    }
}

const JsonValue* Child(const JsonValue* parent, const char* key) {
    return parent != nullptr ? parent->Find(key) : nullptr;
}

std::string Text(const JsonValue* value) {
    return value != nullptr && value->IsString() ? value->text : std::string("<missing>");
}

void TestJsonBasics() {
    JsonValue root;
    Check(ParseJsonDocument(R"({"a":1,"b":true,"c":null,"d":[1,2,3],"e":{"f":"g"}})", root), "object parses");
    Check(Child(&root, "a") != nullptr && Child(&root, "a")->IsNumber(), "number field");
    Check(Child(&root, "b") != nullptr && Child(&root, "b")->boolean, "boolean field");
    Check(Child(&root, "c") != nullptr && Child(&root, "c")->IsNull(), "null field");
    Check(Child(&root, "d") != nullptr && Child(&root, "d")->Size() == 3, "array field");
    CheckEquals(Text(Child(Child(&root, "e"), "f")), "g", "nested object");

    Check(!ParseJsonDocument("", root), "empty document rejected");
    Check(!ParseJsonDocument("{", root), "unterminated object rejected");
    Check(!ParseJsonDocument("{\"a\":1}trailing", root), "trailing garbage rejected");
    Check(!ParseJsonDocument("not json", root), "garbage rejected");

    Check(ParseJsonDocument(R"("plain string")", root) && Text(&root) == "plain string", "bare string document");
}

void TestJsonStringEscapes() {
    JsonValue root;
    Check(ParseJsonDocument(R"({"s":"line\nbreak \"quoted\" back\\slash"})", root), "escaped string parses");
    CheckEquals(Text(Child(&root, "s")), "line\nbreak \"quoted\" back\\slash", "escape decoding");

    Check(ParseJsonDocument(R"({"s":"\u4f60\u597d\u4e16\u754c"})", root), "unicode escapes parse");
    CheckEquals(Text(Child(&root, "s")), "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C", "unicode escape decoding");

    Check(ParseJsonDocument(R"({"s":"\ud83d\ude00"})", root), "surrogate pair parses");
    CheckEquals(Text(Child(&root, "s")), "\xF0\x9F\x98\x80", "surrogate pair decoding");

    Check(ParseJsonDocument(R"({"s":"你好，世界"})", root), "raw utf-8 parses");
    CheckEquals(Text(Child(&root, "s")), "你好，世界", "raw utf-8 preserved");
}

void TestChatCompletionsResponse() {
    JsonValue root;
    const char* body = R"({"choices":[{"message":{"role":"assistant","content":"你好，世界"},"finish_reason":"stop"}]})";
    Check(ParseJsonDocument(body, root), "chat completions parses");
    const JsonValue* choices = Child(&root, "choices");
    Check(choices != nullptr && choices->Size() == 1, "choices array");
    CheckEquals(Text(Child(Child(choices->At(0), "message"), "content")), "你好，世界", "chat content");

    // Content part arrays are used by some OpenAI-compatible servers.
    const char* parts = R"({"choices":[{"message":{"content":[{"type":"text","text":"a"},{"type":"text","text":"b"}]}}]})";
    Check(ParseJsonDocument(parts, root), "content parts parse");
    const JsonValue* content = Child(Child(Child(&root, "choices")->At(0), "message"), "content");
    Check(content != nullptr && content->Size() == 2, "content parts array");
}

void TestResponsesResponse() {
    JsonValue root;
    const char* body = R"({"status":"completed","output":[)"
                       R"({"type":"reasoning","summary":[]},)"
                       R"({"type":"message","role":"assistant","content":[{"type":"output_text","text":"你好，世界"}]}]})";
    Check(ParseJsonDocument(body, root), "responses document parses");
    const JsonValue* output = Child(&root, "output");
    Check(output != nullptr && output->Size() == 2, "output array");
    CheckEquals(Text(Child(output->At(1), "type")), "message", "message item type");
    CheckEquals(Text(Child(Child(output->At(1), "content")->At(0), "text")), "你好，世界", "output text");
}

void TestAnthropicResponse() {
    JsonValue root;
    const char* body = R"({"id":"msg_1","type":"message","role":"assistant","content":[)"
                       R"({"type":"text","text":"bonjour"},{"type":"text","text":" le monde"}],"stop_reason":"end_turn"})";
    Check(ParseJsonDocument(body, root), "anthropic document parses");
    const JsonValue* content = Child(&root, "content");
    Check(content != nullptr && content->Size() == 2, "anthropic content blocks");
    CheckEquals(Text(Child(content->At(0), "text")), "bonjour", "first text block");
}

void TestErrorResponse() {
    JsonValue root;
    Check(ParseJsonDocument(R"({"error":{"type":"invalid_request_error","message":"bad key"}})", root), "error parses");
    CheckEquals(Text(Child(Child(&root, "error"), "message")), "bad key", "error message");
}

void TestResponsesEventStreamFragments() {
    // Non-streaming responses are what tslt requests, but servers may still emit a
    // final JSON document with an empty output array when the model refuses.
    JsonValue root;
    Check(ParseJsonDocument(R"({"status":"incomplete","incomplete_details":{"reason":"max_output_tokens"},"output":[]})", root),
        "incomplete response parses");
    CheckEquals(Text(Child(&root, "status")), "incomplete", "incomplete status");
    CheckEquals(Text(Child(Child(&root, "incomplete_details"), "reason")), "max_output_tokens", "incomplete reason");
}

void TestProtocolParsing() {
    Check(ParseLlmProtocol(L"") == LlmProtocol::OpenAi, "empty protocol defaults to openai");
    Check(ParseLlmProtocol(L"openai") == LlmProtocol::OpenAi, "openai protocol");
    Check(ParseLlmProtocol(L"openai-chat-completions") == LlmProtocol::OpenAi, "openai alias");
    Check(ParseLlmProtocol(L"responses") == LlmProtocol::OpenAiResponses, "responses protocol");
    Check(ParseLlmProtocol(L"OpenAI_Responses") == LlmProtocol::OpenAiResponses, "responses alias");
    Check(ParseLlmProtocol(L"anthropic") == LlmProtocol::Anthropic, "anthropic protocol");
    Check(ParseLlmProtocol(L"Anthropic-Messages") == LlmProtocol::Anthropic, "anthropic alias");
    Check(ParseLlmProtocol(L"claude") == LlmProtocol::Anthropic, "claude alias");
    Check(ParseLlmProtocol(L"something-else") == LlmProtocol::OpenAi, "unknown protocol falls back");

    Check(ParseLlmAuthStyle(L"") == LlmAuthStyle::Auto, "auth auto by default");
    Check(ParseLlmAuthStyle(L"bearer") == LlmAuthStyle::Bearer, "auth bearer");
    Check(ParseLlmAuthStyle(L"x-api-key") == LlmAuthStyle::XApiKey, "auth x-api-key");
    Check(ParseLlmAuthStyle(L"api-key") == LlmAuthStyle::ApiKey, "auth api-key");
    Check(ParseLlmAuthStyle(L"none") == LlmAuthStyle::None, "auth none");

    Check(ResolveLlmAuthStyle(LlmProtocol::OpenAi, LlmAuthStyle::Auto) == LlmAuthStyle::Bearer, "openai auto auth");
    Check(ResolveLlmAuthStyle(LlmProtocol::OpenAiResponses, LlmAuthStyle::Auto) == LlmAuthStyle::Bearer, "responses auto auth");
    Check(ResolveLlmAuthStyle(LlmProtocol::Anthropic, LlmAuthStyle::Auto) == LlmAuthStyle::XApiKey, "anthropic auto auth");
    Check(ResolveLlmAuthStyle(LlmProtocol::Anthropic, LlmAuthStyle::None) == LlmAuthStyle::None, "explicit auth wins");
}

}

int main() {
    TestJsonBasics();
    TestJsonStringEscapes();
    TestChatCompletionsResponse();
    TestResponsesResponse();
    TestAnthropicResponse();
    TestErrorResponse();
    TestResponsesEventStreamFragments();
    TestProtocolParsing();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all json/protocol checks passed\n";
    return EXIT_SUCCESS;
}
