// Portable unit tests for the SSE parser and per-protocol delta extraction.
#include <cstdlib>
#include <iostream>
#include <string>

#include "SseStream.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

void CheckEquals(const std::string& actual, const std::string& expected, const std::string& message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " (expected \"" << expected << "\", got \"" << actual << "\")\n";
        ++g_failures;
    }
}

void TestParserBasics() {
    sse::Parser parser;
    parser.Feed("data: one\n\ndata: two\n\n");
    const auto events = parser.TakeEvents();
    Check(events.size() == 2, "two events parsed");
    if (events.size() == 2) {
        CheckEquals(events[0], "one", "first event");
        CheckEquals(events[1], "two", "second event");
    }

    // Split across arbitrary chunk boundaries.
    sse::Parser chunked;
    chunked.Feed("da");
    Check(chunked.TakeEvents().empty(), "partial line yields no event");
    chunked.Feed("ta: hel");
    chunked.Feed("lo\n");
    Check(chunked.TakeEvents().empty(), "event needs a blank line");
    chunked.Feed("\n");
    const auto chunkedEvents = chunked.TakeEvents();
    Check(chunkedEvents.size() == 1, "event completes across chunks");
    if (chunkedEvents.size() == 1) {
        CheckEquals(chunkedEvents[0], "hello", "reassembled payload");
    }

    // CRLF line endings, comments, event: fields and multi-line data.
    sse::Parser crlf;
    crlf.Feed(": keep-alive\r\n");
    crlf.Feed("event: content_block_delta\r\n");
    crlf.Feed("data: line1\r\ndata: line2\r\n\r\n");
    const auto crlfEvents = crlf.TakeEvents();
    Check(crlfEvents.size() == 1, "comment and event field are ignored");
    if (crlfEvents.size() == 1) {
        CheckEquals(crlfEvents[0], "line1\nline2", "multi-line data joined");
    }

    // Trailing event without the final blank line.
    sse::Parser trailing;
    trailing.Feed("data: last");
    trailing.Finish();
    const auto trailingEvents = trailing.TakeEvents();
    Check(trailingEvents.size() == 1, "unterminated trailing event is flushed");
    if (trailingEvents.size() == 1) {
        CheckEquals(trailingEvents[0], "last", "trailing payload");
    }

    Check(sse::IsStreamEnd("[DONE]"), "done marker detected");
    Check(!sse::IsStreamEnd("{}"), "json is not a done marker");
}

void TestOpenAiChatDeltas() {
    const sse::StreamPayload first = sse::Interpret(LlmProtocol::OpenAi,
        R"({"choices":[{"index":0,"delta":{"content":"你好"}}]})");
    Check(first.recognized, "chat chunk recognised");
    CheckEquals(first.textDelta, "你好", "chat content delta");
    Check(!first.done, "chat chunk is not terminal");

    const sse::StreamPayload reasoning = sse::Interpret(LlmProtocol::OpenAi,
        R"({"choices":[{"index":0,"delta":{"reasoning":"thinking"}}]})");
    CheckEquals(reasoning.textDelta, "", "reasoning deltas are not translated text");

    const sse::StreamPayload done = sse::Interpret(LlmProtocol::OpenAi,
        R"({"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]})");
    Check(done.done, "finish_reason ends the stream");

    const sse::StreamPayload error = sse::Interpret(LlmProtocol::OpenAi,
        R"({"error":{"message":"rate limited"}})");
    CheckEquals(error.error, "rate limited", "chat error is surfaced");
    Check(error.done, "error ends the stream");
}

void TestAnthropicDeltas() {
    const sse::StreamPayload delta = sse::Interpret(LlmProtocol::Anthropic,
        R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"你好，"}})");
    CheckEquals(delta.textDelta, "你好，", "anthropic text delta");

    const sse::StreamPayload start = sse::Interpret(LlmProtocol::Anthropic,
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})");
    CheckEquals(start.textDelta, "", "anthropic start has no delta");

    const sse::StreamPayload stop = sse::Interpret(LlmProtocol::Anthropic, R"({"type":"message_stop"})");
    Check(stop.done, "message_stop ends the stream");

    const sse::StreamPayload thinking = sse::Interpret(LlmProtocol::Anthropic,
        R"({"type":"content_block_delta","delta":{"type":"thinking_delta","thinking":"hmm"}})");
    CheckEquals(thinking.textDelta, "", "anthropic thinking delta is ignored");

    const sse::StreamPayload error = sse::Interpret(LlmProtocol::Anthropic,
        R"({"type":"error","error":{"type":"invalid_request_error","message":"bad key"}})");
    CheckEquals(error.error, "bad key", "anthropic error is surfaced");
}

void TestResponsesDeltas() {
    const sse::StreamPayload delta = sse::Interpret(LlmProtocol::OpenAiResponses,
        R"({"type":"response.output_text.delta","delta":"你好"})");
    CheckEquals(delta.textDelta, "你好", "responses text delta");

    const sse::StreamPayload reasoning = sse::Interpret(LlmProtocol::OpenAiResponses,
        R"({"type":"response.reasoning_summary_text.delta","delta":"thinking"})");
    CheckEquals(reasoning.textDelta, "", "responses reasoning delta is ignored");

    const sse::StreamPayload completed = sse::Interpret(LlmProtocol::OpenAiResponses,
        R"({"type":"response.completed","response":{"output":[{"type":"message","content":[{"type":"output_text","text":"你好，世界"}]}]}})");
    Check(completed.done, "response.completed ends the stream");
    CheckEquals(completed.fallbackText, "你好，世界", "completed event carries the final text");

    const sse::StreamPayload error = sse::Interpret(LlmProtocol::OpenAiResponses,
        R"({"type":"error","error":{"message":"overloaded"}})");
    CheckEquals(error.error, "overloaded", "responses error is surfaced");
}

void TestGarbage() {
    const sse::StreamPayload garbage = sse::Interpret(LlmProtocol::OpenAi, "not json");
    Check(!garbage.recognized, "garbage payload is not recognised");
    CheckEquals(garbage.textDelta, "", "garbage has no text");
    Check(garbage.error.empty(), "garbage is not an error");
}

}

int main() {
    TestParserBasics();
    TestOpenAiChatDeltas();
    TestAnthropicDeltas();
    TestResponsesDeltas();
    TestGarbage();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all sse checks passed\n";
    return EXIT_SUCCESS;
}
