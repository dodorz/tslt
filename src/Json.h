#ifndef TSLT_JSON_H
#define TSLT_JSON_H

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

// Minimal read-only JSON parser.
// String values are stored as UTF-8 (escapes and \uXXXX are decoded), so this
// header stays independent of the Windows API and is easy to unit-test.
struct JsonValue {
    enum class Type {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object
    };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JsonValue> elements;
    std::vector<std::pair<std::string, JsonValue>> members;

    bool IsNull() const { return type == Type::Null; }
    bool IsBoolean() const { return type == Type::Boolean; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }

    size_t Size() const { return type == Type::Array ? elements.size() : 0; }

    const JsonValue* Find(const std::string& key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        for (const auto& member : members) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return nullptr;
    }

    const JsonValue* At(size_t index) const {
        if (type != Type::Array || index >= elements.size()) {
            return nullptr;
        }
        return &elements[index];
    }
};

namespace json_detail {

class Parser {
public:
    explicit Parser(const std::string& input)
        : input_(input) {
    }

    bool Parse(JsonValue& out) {
        SkipWhitespace();
        if (!ParseValue(out, 0)) {
            return false;
        }
        SkipWhitespace();
        return pos_ == input_.size();
    }

private:
    static constexpr int kMaxDepth = 64;

    const std::string& input_;
    size_t pos_ = 0;

    bool Eof() const { return pos_ >= input_.size(); }
    char Peek() const { return input_[pos_]; }

    void SkipWhitespace() {
        while (!Eof()) {
            const char ch = input_[pos_];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                break;
            }
            ++pos_;
        }
    }

    bool Consume(char expected) {
        if (Eof() || input_[pos_] != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    bool ConsumeLiteral(const char* literal) {
        size_t index = 0;
        while (literal[index] != '\0') {
            if (pos_ + index >= input_.size() || input_[pos_ + index] != literal[index]) {
                return false;
            }
            ++index;
        }
        pos_ += index;
        return true;
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > kMaxDepth) {
            return false;
        }
        SkipWhitespace();
        if (Eof()) {
            return false;
        }

        switch (Peek()) {
        case '{':
            return ParseObject(out, depth);
        case '[':
            return ParseArray(out, depth);
        case '"':
            out.type = JsonValue::Type::String;
            return ParseString(out.text);
        case 't':
            out.type = JsonValue::Type::Boolean;
            out.boolean = true;
            return ConsumeLiteral("true");
        case 'f':
            out.type = JsonValue::Type::Boolean;
            out.boolean = false;
            return ConsumeLiteral("false");
        case 'n':
            out.type = JsonValue::Type::Null;
            return ConsumeLiteral("null");
        default:
            return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Object;
        out.members.clear();
        if (!Consume('{')) {
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            return true;
        }

        while (true) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return false;
            }

            JsonValue value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.members.emplace_back(std::move(key), std::move(value));

            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
        }
    }

    bool ParseArray(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Array;
        out.elements.clear();
        if (!Consume('[')) {
            return false;
        }
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }

        while (true) {
            JsonValue value;
            if (!ParseValue(value, depth + 1)) {
                return false;
            }
            out.elements.push_back(std::move(value));

            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
        }
    }

    bool ParseString(std::string& out) {
        if (!Consume('"')) {
            return false;
        }
        out.clear();

        while (!Eof()) {
            const char ch = input_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (Eof()) {
                return false;
            }

            const char escape = input_[pos_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                unsigned int codeUnit = 0;
                if (!ParseHex4(codeUnit)) {
                    return false;
                }
                if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF && pos_ + 1 < input_.size() &&
                    input_[pos_] == '\\' && input_[pos_ + 1] == 'u') {
                    pos_ += 2;
                    unsigned int low = 0;
                    if (!ParseHex4(low)) {
                        return false;
                    }
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        const unsigned int codePoint = 0x10000u + ((codeUnit - 0xD800u) << 10) + (low - 0xDC00u);
                        AppendUtf8(codePoint, out);
                    } else {
                        AppendUtf8(codeUnit, out);
                        AppendUtf8(low, out);
                    }
                    break;
                }
                AppendUtf8(codeUnit, out);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseHex4(unsigned int& value) {
        if (pos_ + 4 > input_.size()) {
            return false;
        }
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const char ch = input_[pos_++];
            unsigned int digit = 0;
            if (ch >= '0' && ch <= '9') {
                digit = static_cast<unsigned int>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                digit = static_cast<unsigned int>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                digit = static_cast<unsigned int>(ch - 'A' + 10);
            } else {
                return false;
            }
            value = (value << 4) | digit;
        }
        return true;
    }

    static void AppendUtf8(unsigned int codePoint, std::string& out) {
        if (codePoint <= 0x7F) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    bool ParseNumber(JsonValue& out) {
        const size_t start = pos_;
        if (!Eof() && Peek() == '-') {
            ++pos_;
        }

        const size_t integerStart = pos_;
        while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) {
            ++pos_;
        }
        const size_t digits = pos_ - integerStart;
        // JSON forbids leading zeros, so "01" is not a valid number.
        const bool hasLeadingZero = digits > 1 && input_[integerStart] == '0';
        if (digits == 0 || hasLeadingZero) {
            pos_ = start;
            return false;
        }

        if (!Eof() && Peek() == '.') {
            ++pos_;
            size_t fractionDigits = 0;
            while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++pos_;
                ++fractionDigits;
            }
            if (fractionDigits == 0) {
                pos_ = start;
                return false;
            }
        }

        if (!Eof() && (Peek() == 'e' || Peek() == 'E')) {
            ++pos_;
            if (!Eof() && (Peek() == '+' || Peek() == '-')) {
                ++pos_;
            }
            size_t exponentDigits = 0;
            while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) {
                ++pos_;
                ++exponentDigits;
            }
            if (exponentDigits == 0) {
                pos_ = start;
                return false;
            }
        }

        out.type = JsonValue::Type::Number;
        out.number = std::strtod(input_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }
};

}

inline bool ParseJsonDocument(const std::string& text, JsonValue& out) {
    out = JsonValue{};
    json_detail::Parser parser(text);
    return parser.Parse(out);
}

#endif
