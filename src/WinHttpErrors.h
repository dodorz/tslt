#ifndef TSLT_WINHTTP_ERRORS_H
#define TSLT_WINHTTP_ERRORS_H

#include <string>

// WinHTTP error codes are stable, documented values, so they can be described
// without pulling in <winhttp.h>. Keeping this pure makes the mapping testable.
namespace winhttp_errors {

inline constexpr long kTimeout = 12002;
inline constexpr long kNameNotResolved = 12007;
inline constexpr long kCannotConnect = 12029;
inline constexpr long kConnectionError = 12030;
inline constexpr long kSecureFailure = 12175;
inline constexpr long kInvalidServerResponse = 12152;
inline constexpr long kConnectionReset = 12102;
inline constexpr long kOperationCancelled = 12017;
inline constexpr long kHeaderNotFound = 12150;
inline constexpr long kClientAuthCertNeeded = 12044;

inline std::string Describe(long error) {
    switch (error) {
    case 0:
        return "no error";
    case kTimeout:
        return "timed out";
    case kNameNotResolved:
        return "host name could not be resolved";
    case kCannotConnect:
        return "could not connect to the server";
    case kConnectionError:
        return "the connection was aborted";
    case kSecureFailure:
        return "TLS/SSL handshake failed";
    case kInvalidServerResponse:
        return "the server returned an invalid response";
    case kConnectionReset:
        return "the connection was reset by the server";
    case kOperationCancelled:
        return "the operation was cancelled";
    case kHeaderNotFound:
        return "the requested header was not found";
    case kClientAuthCertNeeded:
        return "the server requested a client certificate";
    default:
        break;
    }
    return "unexpected network error";
}

inline bool IsTimeout(long error) {
    return error == kTimeout;
}

// Example: "WinHttpReceiveResponse failed: timed out (12002)"
inline std::string DescribeFailure(const char* operation, long error) {
    std::string message = operation != nullptr ? operation : "HTTP request";
    message += " failed: ";
    message += Describe(error);
    message += " (";
    message += std::to_string(error);
    message += ")";
    if (IsTimeout(error)) {
        message += ". Increase timeout in the provider section if the model is slow.";
    }
    return message;
}

}

#endif
