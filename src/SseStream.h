#ifndef TSLT_SSE_STREAM_H
#define TSLT_SSE_STREAM_H

#include <cstddef>
#include <string>
#include <vector>

#include "Json.h"
#include "LlmProtocol.h"

// Minimal Server-Sent Events reader plus per-protocol delta extraction.
// Windows-free so the parsing rules can be unit tested on any host.
namespace sse {

// Splits an SSE byte stream into event payloads (`data:` lines joined by '\n').
// Fed with arbitrary chunk boundaries, so a line may arrive in several pieces.
class Parser {
public:
    void Feed(const std::string& chunk) {
        buffer_ += chunk;
        size_t pos = 0;
        while (true) {
            const size_t newline = buffer_.find('\n', pos);
            if (newline == std::string::npos) {
                break;
            }
            std::string line = buffer_.substr(pos, newline - pos);
            pos = newline + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            HandleLine(line);
        }
        buffer_.erase(0, pos);
    }

    // Flushes a trailing event that was not terminated by a blank line.
    void Finish() {
        if (!buffer_.empty()) {
            std::string line = buffer_;
            buffer_.clear();
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            HandleLine(line);
        }
        Dispatch();
    }

    std::vector<std::string> TakeEvents() {
        std::vector<std::string> events;
        events.swap(events_);
        return events;
    }

private:
    void HandleLine(const std::string& line) {
        if (line.empty()) {
            Dispatch();
            return;
        }
        if (line.front() == ':') {
            // Comment line, commonly used as a keep-alive.
            return;
        }

        const size_t colon = line.find(':');
        const std::string field = colon == std::string::npos ? line : line.substr(0, colon);
        // event:/id:/retry: carry no payload information that this client needs.
        if (field != "data") {
            return;
        }

        std::string value = colon == std::string::npos ? std::string() : line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') {
            value.erase(0, 1);
        }
        if (hasData_) {
            data_ += '\n';
        }
        data_ += value;
        hasData_ = true;
    }

    void Dispatch() {
        if (!hasData_) {
            return;
        }
        events_.push_back(data_);
        data_.clear();
        hasData_ = false;
    }

    std::string buffer_;
    std::string data_;
    bool hasData_ = false;
    std::vector<std::string> events_;
};

inline bool IsStreamEnd(const std::string& payload) {
    return payload == "[DONE]";
}

struct StreamPayload {
    std::string textDelta;
    std::string fallbackText;
    std::string error;
    bool done = false;
    bool recognized = false;
};

inline std::string ContentPartsText(const JsonValue* content) {
    if (content == nullptr) {
        return "";
    }
    if (content->IsString()) {
        return content->text;
    }
    if (!content->IsArray()) {
        return "";
    }

    std::string text;
    for (const JsonValue& part : content->elements) {
        if (part.IsString()) {
            text += part.text;
            continue;
        }
        if (!part.IsObject()) {
            continue;
        }
        if (const JsonValue* type = part.Find("type"); type != nullptr && type->IsString() &&
            type->text != "text" && type->text != "output_text") {
            continue;
        }
        if (const JsonValue* value = part.Find("text"); value != nullptr && value->IsString()) {
            text += value->text;
        }
    }
    return text;
}

inline std::string ErrorText(const JsonValue& root) {
    if (const JsonValue* error = root.Find("error")) {
        if (error->IsString() && !error->text.empty()) {
            return error->text;
        }
        if (error->IsObject()) {
            if (const JsonValue* message = error->Find("message"); message != nullptr && message->IsString()) {
                return message->text;
            }
        }
    }
    if (const JsonValue* message = root.Find("message"); message != nullptr && message->IsString()) {
        return message->text;
    }
    return "";
}

inline StreamPayload InterpretOpenAiChat(const JsonValue& root) {
    StreamPayload payload;
    payload.recognized = true;

    if (const std::string error = ErrorText(root); !error.empty()) {
        payload.error = error;
        payload.done = true;
        return payload;
    }

    const JsonValue* choices = root.Find("choices");
    if (choices == nullptr || !choices->IsArray() || choices->elements.empty()) {
        return payload;
    }

    const JsonValue& choice = choices->elements.front();
    if (const JsonValue* delta = choice.Find("delta")) {
        if (const JsonValue* content = delta->Find("content")) {
            payload.textDelta = ContentPartsText(content);
        }
    }
    if (const JsonValue* finish = choice.Find("finish_reason"); finish != nullptr && finish->IsString()) {
        payload.done = true;
    }
    return payload;
}

inline StreamPayload InterpretAnthropic(const JsonValue& root) {
    StreamPayload payload;
    payload.recognized = true;

    const JsonValue* type = root.Find("type");
    const std::string eventType = type != nullptr && type->IsString() ? type->text : std::string();

    if (eventType == "error") {
        payload.error = ErrorText(root);
        payload.done = true;
        return payload;
    }
    if (eventType == "content_block_delta") {
        if (const JsonValue* delta = root.Find("delta"); delta != nullptr) {
            if (const JsonValue* text = delta->Find("text"); text != nullptr && text->IsString()) {
                payload.textDelta = text->text;
            }
        }
        return payload;
    }
    if (eventType == "message_delta" || eventType == "message_stop") {
        payload.done = true;
    }
    return payload;
}

inline StreamPayload InterpretOpenAiResponses(const JsonValue& root) {
    StreamPayload payload;
    payload.recognized = true;

    const JsonValue* type = root.Find("type");
    const std::string eventType = type != nullptr && type->IsString() ? type->text : std::string();

    if (eventType == "error") {
        payload.error = ErrorText(root);
        payload.done = true;
        return payload;
    }
    if (eventType == "response.output_text.delta") {
        if (const JsonValue* delta = root.Find("delta"); delta != nullptr && delta->IsString()) {
            payload.textDelta = delta->text;
        }
        return payload;
    }
    if (eventType == "response.completed" || eventType == "response.incomplete") {
        payload.done = true;
        // Some gateways send an empty delta stream but include the finished
        // response in the terminal event, so keep it as a fallback.
        if (const JsonValue* response = root.Find("response"); response != nullptr) {
            const JsonValue* output = response->Find("output");
            if (output != nullptr && output->IsArray()) {
                for (const JsonValue& item : output->elements) {
                    if (!item.IsObject()) {
                        continue;
                    }
                    if (const JsonValue* itemType = item.Find("type"); itemType != nullptr && itemType->IsString() &&
                        itemType->text != "message") {
                        continue;
                    }
                    payload.fallbackText += ContentPartsText(item.Find("content"));
                }
            }
        }
    }
    return payload;
}

inline StreamPayload Interpret(LlmProtocol protocol, const std::string& payload) {
    JsonValue root;
    if (!ParseJsonDocument(payload, root) || !root.IsObject()) {
        return StreamPayload{};
    }

    switch (protocol) {
    case LlmProtocol::Anthropic:
        return InterpretAnthropic(root);
    case LlmProtocol::OpenAiResponses:
        return InterpretOpenAiResponses(root);
    case LlmProtocol::OpenAi:
    default:
        return InterpretOpenAiChat(root);
    }
}

}

#endif
