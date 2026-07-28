#include "config_key.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace warden {

namespace {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string TrimAscii(std::string value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

class JsonSyntaxValidator {
public:
    explicit JsonSyntaxValidator(const std::string& value) : value_(value) {}

    bool IsCompleteObject() {
        SkipWhitespace();
        if (position_ >= value_.size() || value_[position_] != '{') return false;
        if (!ParseObject()) return false;
        SkipWhitespace();
        return position_ == value_.size();
    }

private:
    static constexpr size_t MaxContainerDepth = 64;
    const std::string& value_;
    size_t position_ = 0;
    size_t depth_ = 0;

    static bool IsWhitespace(unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    }

    static bool IsDigit(unsigned char ch) {
        return ch >= '0' && ch <= '9';
    }

    static bool IsHex(unsigned char ch) {
        return IsDigit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    void SkipWhitespace() {
        while (position_ < value_.size() && IsWhitespace(static_cast<unsigned char>(value_[position_]))) ++position_;
    }

    bool Consume(char expected) {
        if (position_ >= value_.size() || value_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool ParseLiteral(const char* literal) {
        for (size_t i = 0; literal[i] != '\0'; ++i) {
            if (position_ >= value_.size() || value_[position_] != literal[i]) return false;
            ++position_;
        }
        return true;
    }

    bool ParseString() {
        if (!Consume('"')) return false;
        while (position_ < value_.size()) {
            const unsigned char ch = static_cast<unsigned char>(value_[position_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch != '\\') continue;
            if (position_ >= value_.size()) return false;
            const char escape = value_[position_++];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' || escape == 'n' || escape == 'r' || escape == 't') continue;
            if (escape != 'u') return false;
            for (int i = 0; i < 4; ++i) {
                if (position_ >= value_.size() || !IsHex(static_cast<unsigned char>(value_[position_++]))) return false;
            }
        }
        return false;
    }

    bool ParseNumber() {
        if (position_ < value_.size() && value_[position_] == '-') ++position_;
        if (position_ >= value_.size()) return false;
        if (value_[position_] == '0') {
            ++position_;
        } else {
            if (value_[position_] < '1' || value_[position_] > '9') return false;
            do { ++position_; } while (position_ < value_.size() && IsDigit(static_cast<unsigned char>(value_[position_])));
        }
        if (position_ < value_.size() && value_[position_] == '.') {
            ++position_;
            const auto fractionStart = position_;
            while (position_ < value_.size() && IsDigit(static_cast<unsigned char>(value_[position_]))) ++position_;
            if (position_ == fractionStart) return false;
        }
        if (position_ < value_.size() && (value_[position_] == 'e' || value_[position_] == 'E')) {
            ++position_;
            if (position_ < value_.size() && (value_[position_] == '+' || value_[position_] == '-')) ++position_;
            const auto exponentStart = position_;
            while (position_ < value_.size() && IsDigit(static_cast<unsigned char>(value_[position_]))) ++position_;
            if (position_ == exponentStart) return false;
        }
        return true;
    }

    bool ParseArray() {
        if (!Consume('[')) return false;
        if (depth_ >= MaxContainerDepth) return false;
        ++depth_;
        SkipWhitespace();
        if (Consume(']')) {
            --depth_;
            return true;
        }
        while (true) {
            if (!ParseValue()) {
                --depth_;
                return false;
            }
            SkipWhitespace();
            if (Consume(']')) {
                --depth_;
                return true;
            }
            if (!Consume(',')) {
                --depth_;
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseObject() {
        if (!Consume('{')) return false;
        if (depth_ >= MaxContainerDepth) return false;
        ++depth_;
        SkipWhitespace();
        if (Consume('}')) {
            --depth_;
            return true;
        }
        while (true) {
            if (!ParseString()) {
                --depth_;
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                --depth_;
                return false;
            }
            if (!ParseValue()) {
                --depth_;
                return false;
            }
            SkipWhitespace();
            if (Consume('}')) {
                --depth_;
                return true;
            }
            if (!Consume(',')) {
                --depth_;
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseValue() {
        SkipWhitespace();
        if (position_ >= value_.size()) return false;
        switch (value_[position_]) {
        case '{': return ParseObject();
        case '[': return ParseArray();
        case '"': return ParseString();
        case 't': return ParseLiteral("true");
        case 'f': return ParseLiteral("false");
        case 'n': return ParseLiteral("null");
        default: return value_[position_] == '-' || IsDigit(static_cast<unsigned char>(value_[position_])) ? ParseNumber() : false;
        }
    }
};

}

std::string SafeConfigKey(std::string value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;

    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::string CanonicalPluginConfigKey(std::string value) {
    const auto trimmed = TrimAscii(std::move(value));
    if (trimmed.empty()) return {};
    for (const auto ch : trimmed) {
        const auto unsignedCh = static_cast<unsigned char>(ch);
        if (!((unsignedCh >= 'a' && unsignedCh <= 'z') || (unsignedCh >= 'A' && unsignedCh <= 'Z') ||
            (unsignedCh >= '0' && unsignedCh <= '9') || ch == '-' || ch == '_')) {
            return {};
        }
    }
    return trimmed;
}

std::vector<std::string> LegacyPluginConfigAliases(const std::string&) {
    return {};
}
std::string PublicReleaseApiKeyOrEmpty(std::string value) {
    value = TrimAscii(std::move(value));
    // A release key must be a locally generated secret, not a short default.
    // The explicit marker is the only API-key placeholder emitted by the
    // public package.
    if (value.size() < 32 || value == "__SCUMNEDJIN_SET_LOCAL_API_KEY__") {
        return {};
    }
    return value;
}

bool IsValidJsonObject(const std::string& value) {
    return JsonSyntaxValidator(value).IsCompleteObject();
}

}
