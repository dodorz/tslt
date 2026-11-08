// Portable unit tests for the WinHTTP error descriptions.
#include <cstdlib>
#include <iostream>
#include <string>

#include "WinHttpErrors.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

void TestDescriptions() {
    Check(winhttp_errors::Describe(winhttp_errors::kTimeout) == "timed out", "timeout description");
    Check(winhttp_errors::Describe(winhttp_errors::kNameNotResolved) == "host name could not be resolved",
        "dns description");
    Check(winhttp_errors::Describe(winhttp_errors::kCannotConnect) == "could not connect to the server",
        "connect description");
    Check(winhttp_errors::Describe(winhttp_errors::kSecureFailure) == "TLS/SSL handshake failed",
        "TLS description");
    Check(winhttp_errors::Describe(winhttp_errors::kInvalidServerResponse) == "the server returned an invalid response",
        "invalid response description");
    Check(winhttp_errors::Describe(winhttp_errors::kOperationCancelled) == "the operation was cancelled",
        "cancellation description");
    Check(!winhttp_errors::Describe(123456).empty(), "unknown codes still get a description");
}

void TestFailureMessages() {
    const std::string timeout = winhttp_errors::DescribeFailure("WinHttpReceiveResponse", winhttp_errors::kTimeout);
    Check(timeout.find("WinHttpReceiveResponse") != std::string::npos, "operation is named");
    Check(timeout.find("timed out") != std::string::npos, "reason is named");
    Check(timeout.find("12002") != std::string::npos, "error code is kept");

    const std::string dns = winhttp_errors::DescribeFailure("WinHttpConnect", winhttp_errors::kNameNotResolved);
    Check(dns.find("could not be resolved") != std::string::npos, "dns reason is named");

    Check(winhttp_errors::IsTimeout(winhttp_errors::kTimeout), "timeout code is detected");
    Check(!winhttp_errors::IsTimeout(winhttp_errors::kSecureFailure), "other codes are not timeouts");
    Check(timeout.find("Increase timeout") != std::string::npos, "timeout suggests raising the timeout");
    Check(dns.find("Increase timeout") == std::string::npos, "other failures do not suggest a timeout");

    const std::string noOperation = winhttp_errors::DescribeFailure(nullptr, winhttp_errors::kTimeout);
    Check(noOperation.find("HTTP request failed") == 0, "missing operation falls back to a generic name");
}

}

int main() {
    TestDescriptions();
    TestFailureMessages();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all winhttp error checks passed\n";
    return EXIT_SUCCESS;
}
