// Portable unit tests for the provider error classification helpers.
#include <cstdlib>
#include <iostream>
#include <string>

#include "ApiErrorHints.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

void TestRejectsTemperature() {
    Check(api_hints::RejectsTemperature("claude-haiku-5-5 does not support non-default temperature"),
        "claude rejection is recognised");
    Check(api_hints::RejectsTemperature("temperature is not supported for this model"),
        "not supported phrasing is recognised");
    Check(api_hints::RejectsTemperature("Unsupported value: 'temperature' does not support 0.2"),
        "openai reasoning phrasing is recognised");
    Check(api_hints::RejectsTemperature("Invalid 'temperature': only the default (1) is supported"),
        "default-only phrasing is recognised");
    Check(api_hints::RejectsTemperature("TEMPERATURE is not allowed"),
        "matching is case insensitive");

    Check(!api_hints::RejectsTemperature(""), "empty message is not a rejection");
    Check(!api_hints::RejectsTemperature("messages: text content blocks must be non-empty"),
        "unrelated error is not a rejection");
    Check(!api_hints::RejectsTemperature("temperature backend unavailable"),
        "message without an unsupported phrase is not a rejection");
    Check(!api_hints::ShouldRetryWithoutTemperature(500, "temperature is not supported"),
        "5xx responses are not retried");
    Check(!api_hints::ShouldRetryWithoutTemperature(0, "temperature is not supported"),
        "missing status is not retried");
    Check(!api_hints::ShouldRetryWithoutTemperature(400, "invalid api key"),
        "auth errors are not retried");
    Check(api_hints::ShouldRetryWithoutTemperature(400, "temperature is not supported"),
        "4xx temperature rejection is retried");
    Check(api_hints::ShouldRetryWithoutTemperature(422, "unsupported temperature"),
        "4xx unprocessable rejection is retried");
}

}

int main() {
    TestRejectsTemperature();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all api error hint checks passed\n";
    return EXIT_SUCCESS;
}
