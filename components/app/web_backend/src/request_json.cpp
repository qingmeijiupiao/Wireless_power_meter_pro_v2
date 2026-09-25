/*
 * @version: 1.0
 * @LastEditors: qingmeijiupiao
 * @Description: Web request JSON field parser based on jsmn tokens
 * @Author: qingmeijiupiao
 * @LastEditTime: 2026-06-02
 */
#include "web_backend_internal.h"

#include <cstring>
#include <limits>

#include "jsmn.h"

namespace WebBackend {

namespace {

constexpr size_t JSON_TOKEN_COUNT = 64;

bool emit_byte(char byte, char* out, size_t out_size, const char* expected, size_t* decoded_size, bool* matches) {
    if (expected != nullptr && *matches && (expected[*decoded_size] == '\0' || expected[*decoded_size] != byte)) {
        *matches = false;
    }
    if (out != nullptr && out_size > 0 && *decoded_size < out_size - 1) {
        out[*decoded_size] = byte;
    }
    ++(*decoded_size);
    return true;
}

bool emit_codepoint(uint32_t codepoint, char* out, size_t out_size, const char* expected, size_t* decoded_size,
                    bool* matches) {
    if (codepoint <= 0x7F) {
        return emit_byte(static_cast<char>(codepoint), out, out_size, expected, decoded_size, matches);
    }
    if (codepoint <= 0x7FF) {
        return emit_byte(static_cast<char>(0xC0 | (codepoint >> 6)), out, out_size, expected, decoded_size, matches) &&
               emit_byte(static_cast<char>(0x80 | (codepoint & 0x3F)), out, out_size, expected, decoded_size, matches);
    }
    if (codepoint <= 0xFFFF) {
        return emit_byte(static_cast<char>(0xE0 | (codepoint >> 12)), out, out_size, expected, decoded_size, matches) &&
               emit_byte(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)), out, out_size, expected, decoded_size,
                         matches) &&
               emit_byte(static_cast<char>(0x80 | (codepoint & 0x3F)), out, out_size, expected, decoded_size, matches);
    }
    return emit_byte(static_cast<char>(0xF0 | (codepoint >> 18)), out, out_size, expected, decoded_size, matches) &&
           emit_byte(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)), out, out_size, expected, decoded_size,
                     matches) &&
           emit_byte(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)), out, out_size, expected, decoded_size,
                     matches) &&
           emit_byte(static_cast<char>(0x80 | (codepoint & 0x3F)), out, out_size, expected, decoded_size, matches);
}

bool read_hex_quad(const char* json, int offset, uint32_t* codepoint) {
    *codepoint = 0;
    for (int i = 0; i < 4; ++i) {
        const char digit   = json[offset + i];
        *codepoint       <<= 4;
        if (digit >= '0' && digit <= '9') {
            *codepoint += static_cast<uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            *codepoint += static_cast<uint32_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            *codepoint += static_cast<uint32_t>(digit - 'A' + 10);
        } else {
            return false;
        }
    }
    return true;
}

bool decode_string(const char* json, const jsmntok_t& token, char* out, size_t out_size,
                   const char* expected = nullptr) {
    if (token.type != JSMN_STRING || (out != nullptr && out_size == 0)) {
        return false;
    }

    size_t decoded_size = 0;
    bool   matches      = true;
    for (int offset = token.start; offset < token.end; ++offset) {
        const uint8_t byte = static_cast<uint8_t>(json[offset]);
        if (byte < 0x20) {
            return false;
        }
        if (byte != '\\') {
            emit_byte(static_cast<char>(byte), out, out_size, expected, &decoded_size, &matches);
            continue;
        }

        if (++offset >= token.end) {
            return false;
        }
        switch (json[offset]) {
        case '"':
        case '\\':
        case '/':
            emit_byte(json[offset], out, out_size, expected, &decoded_size, &matches);
            break;
        case 'b':
            emit_byte('\b', out, out_size, expected, &decoded_size, &matches);
            break;
        case 'f':
            emit_byte('\f', out, out_size, expected, &decoded_size, &matches);
            break;
        case 'n':
            emit_byte('\n', out, out_size, expected, &decoded_size, &matches);
            break;
        case 'r':
            emit_byte('\r', out, out_size, expected, &decoded_size, &matches);
            break;
        case 't':
            emit_byte('\t', out, out_size, expected, &decoded_size, &matches);
            break;
        case 'u': {
            if (offset + 4 >= token.end) {
                return false;
            }
            uint32_t codepoint = 0;
            if (!read_hex_quad(json, offset + 1, &codepoint)) {
                return false;
            }
            offset += 4;
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                if (offset + 6 >= token.end || json[offset + 1] != '\\' || json[offset + 2] != 'u') {
                    return false;
                }
                uint32_t low_surrogate = 0;
                if (!read_hex_quad(json, offset + 3, &low_surrogate) || low_surrogate < 0xDC00 ||
                    low_surrogate > 0xDFFF) {
                    return false;
                }
                codepoint  = 0x10000 + ((codepoint - 0xD800) << 10) + (low_surrogate - 0xDC00);
                offset    += 6;
            } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                return false;
            }
            emit_codepoint(codepoint, out, out_size, expected, &decoded_size, &matches);
            break;
        }
        default:
            return false;
        }
    }

    if (out != nullptr) {
        out[decoded_size < out_size ? decoded_size : out_size - 1] = '\0';
    }
    return expected == nullptr || (matches && expected[decoded_size] == '\0');
}

bool token_equals(const char* json, const jsmntok_t& token, const char* expected) {
    return expected != nullptr && decode_string(json, token, nullptr, 0, expected);
}

size_t next_token(const jsmntok_t* tokens, size_t token_count, size_t index) {
    const int end = tokens[index].end;
    ++index;
    while (index < token_count && tokens[index].start < end) {
        ++index;
    }
    return index;
}

bool token_equals_literal(const char* json, const jsmntok_t& token, const char* literal) {
    const size_t literal_size = strlen(literal);
    return token.type == JSMN_PRIMITIVE && token.end - token.start == static_cast<int>(literal_size) &&
           memcmp(json + token.start, literal, literal_size) == 0;
}

bool token_to_uint32(const char* json, const jsmntok_t& token, uint32_t* out) {
    if (token.type != JSMN_PRIMITIVE || token.start >= token.end) {
        return false;
    }
    uint32_t value = 0;
    for (int offset = token.start; offset < token.end; ++offset) {
        const char digit = json[offset];
        if (digit < '0' || digit > '9') {
            return false;
        }
        const uint32_t next_digit = static_cast<uint32_t>(digit - '0');
        if (value > (std::numeric_limits<uint32_t>::max() - next_digit) / 10) {
            return false;
        }
        value = value * 10 + next_digit;
    }
    *out = value;
    return true;
}

bool token_to_int32(const char* json, const jsmntok_t& token, int32_t* out) {
    if (token.type != JSMN_PRIMITIVE || token.start >= token.end) {
        return false;
    }
    int  offset   = token.start;
    bool negative = false;
    if (json[offset] == '-') {
        negative = true;
        ++offset;
    }
    if (offset >= token.end) {
        return false;
    }
    // 先按 int64 累加，再统一做范围检查，避免 INT32_MIN 取负溢出。
    int64_t value = 0;
    for (; offset < token.end; ++offset) {
        const char digit = json[offset];
        if (digit < '0' || digit > '9') {
            return false;
        }
        value = value * 10 + (digit - '0');
        if (value > static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1) {
            return false;
        }
    }
    if (negative) {
        value = -value;
    }
    if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *out = static_cast<int32_t>(value);
    return true;
}

void skip_whitespace(const char* json, size_t json_size, size_t* offset) {
    while (*offset < json_size) {
        const char byte = json[*offset];
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') {
            return;
        }
        ++(*offset);
    }
}

bool is_valid_number(const char* json, const jsmntok_t& token) {
    int offset = token.start;
    if (offset < token.end && json[offset] == '-') {
        ++offset;
    }
    if (offset >= token.end) {
        return false;
    }
    if (json[offset] == '0') {
        ++offset;
    } else {
        if (json[offset] < '1' || json[offset] > '9') {
            return false;
        }
        while (++offset < token.end && json[offset] >= '0' && json[offset] <= '9') {
        }
    }
    if (offset < token.end && json[offset] == '.') {
        const int fraction_start = ++offset;
        while (offset < token.end && json[offset] >= '0' && json[offset] <= '9') {
            ++offset;
        }
        if (offset == fraction_start) {
            return false;
        }
    }
    if (offset < token.end && (json[offset] == 'e' || json[offset] == 'E')) {
        ++offset;
        if (offset < token.end && (json[offset] == '+' || json[offset] == '-')) {
            ++offset;
        }
        const int exponent_start = offset;
        while (offset < token.end && json[offset] >= '0' && json[offset] <= '9') {
            ++offset;
        }
        if (offset == exponent_start) {
            return false;
        }
    }
    return offset == token.end;
}

bool validate_token(const char* json, size_t json_size, const jsmntok_t* tokens, size_t token_count, size_t* index,
                    size_t* offset) {
    skip_whitespace(json, json_size, offset);
    if (*index >= token_count) {
        return false;
    }

    const jsmntok_t& token = tokens[(*index)++];
    if (token.type == JSMN_STRING) {
        if (*offset != static_cast<size_t>(token.start - 1) || !decode_string(json, token, nullptr, 0)) {
            return false;
        }
        *offset = static_cast<size_t>(token.end + 1);
        return true;
    }
    if (token.type == JSMN_PRIMITIVE) {
        if (*offset != static_cast<size_t>(token.start) ||
            (!token_equals_literal(json, token, "true") && !token_equals_literal(json, token, "false") &&
             !token_equals_literal(json, token, "null") && !is_valid_number(json, token))) {
            return false;
        }
        *offset = static_cast<size_t>(token.end);
        return true;
    }

    const bool is_object = token.type == JSMN_OBJECT;
    if ((!is_object && token.type != JSMN_ARRAY) || *offset != static_cast<size_t>(token.start)) {
        return false;
    }
    ++(*offset);
    for (int child = 0; child < token.size; ++child) {
        if (child > 0) {
            skip_whitespace(json, json_size, offset);
            if (*offset >= json_size || json[(*offset)++] != ',') {
                return false;
            }
        }
        if (!validate_token(json, json_size, tokens, token_count, index, offset)) {
            return false;
        }
        if (is_object) {
            if (tokens[*index - 1].type != JSMN_STRING) {
                return false;
            }
            skip_whitespace(json, json_size, offset);
            if (*offset >= json_size || json[(*offset)++] != ':' ||
                !validate_token(json, json_size, tokens, token_count, index, offset)) {
                return false;
            }
        }
    }
    skip_whitespace(json, json_size, offset);
    const char closing_byte = is_object ? '}' : ']';
    if (*offset >= json_size || json[(*offset)++] != closing_byte || *offset != static_cast<size_t>(token.end)) {
        return false;
    }
    return true;
}

bool validate_json(const char* json, size_t json_size, const jsmntok_t* tokens, size_t token_count) {
    size_t index  = 0;
    size_t offset = 0;
    if (!validate_token(json, json_size, tokens, token_count, &index, &offset)) {
        return false;
    }
    skip_whitespace(json, json_size, &offset);
    return index == token_count && offset == json_size;
}

template <typename Reader> bool read_top_level_field(const char* json, const char* key, Reader reader) {
    if (json == nullptr || key == nullptr) {
        return false;
    }

    jsmn_parser parser                   = {};
    jsmntok_t   tokens[JSON_TOKEN_COUNT] = {};
    jsmn_init(&parser);
    const size_t json_size   = strlen(json);
    const int    token_count = jsmn_parse(&parser, json, json_size, tokens, JSON_TOKEN_COUNT);
    if (token_count < 1 || tokens[0].type != JSMN_OBJECT ||
        !validate_json(json, json_size, tokens, static_cast<size_t>(token_count))) {
        return false;
    }

    bool   found = false;
    size_t index = 1;
    while (index < static_cast<size_t>(token_count) && tokens[index].start < tokens[0].end) {
        const jsmntok_t& key_token = tokens[index++];
        if (key_token.type != JSMN_STRING || index >= static_cast<size_t>(token_count)) {
            return false;
        }
        const jsmntok_t& value_token = tokens[index];
        if (token_equals(json, key_token, key) && reader(value_token)) {
            found = true;
        }
        index = next_token(tokens, static_cast<size_t>(token_count), index);
    }
    return found;
}

} // namespace

bool json_get_string(const char* json, const char* key, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return false;
    }
    return read_top_level_field(
        json, key, [json, out, out_size](const jsmntok_t& token) { return decode_string(json, token, out, out_size); });
}

bool json_get_bool(const char* json, const char* key, bool* out) {
    if (out == nullptr) {
        return false;
    }
    return read_top_level_field(json, key, [json, out](const jsmntok_t& token) {
        if (token_equals_literal(json, token, "true")) {
            *out = true;
            return true;
        }
        if (token_equals_literal(json, token, "false")) {
            *out = false;
            return true;
        }
        return false;
    });
}

bool json_get_uint32(const char* json, const char* key, uint32_t* out) {
    return out != nullptr && read_top_level_field(json, key, [json, out](const jsmntok_t& token) {
               return token_to_uint32(json, token, out);
           });
}

bool json_get_int32(const char* json, const char* key, int32_t* out) {
    return out != nullptr && read_top_level_field(json, key, [json, out](const jsmntok_t& token) {
               return token_to_int32(json, token, out);
           });
}

bool json_has_key(const char* json, const char* key) {
    return read_top_level_field(json, key, [](const jsmntok_t&) { return true; });
}

} // namespace WebBackend
