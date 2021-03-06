/* vim: set et ts=4 sw=4 sts=4 ft=c:
 *
 * Copyright (C) 2012, 2013, 2014 James McLaughlin et al.  All rights reserved.
 * https://github.com/udp/json-parser
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <limits>

namespace {
    struct json_value;

    struct object_entry {
        char *name;
        unsigned int name_length;
        json_value *value;
    };
    static_assert(std::is_standard_layout_v<object_entry>);

    struct json_value {
        json_value *parent;
        json::type type;

        union {
            bool boolean;
            int64_t integer;
            double dbl;

            struct {
                unsigned int length;
                char *ptr; // null terminated
            } string;

            struct {
                unsigned int length;
                object_entry *values;
            } object;

            struct {
                unsigned int length;
                json_value **values;
            } array;
        } u;

        union {
            void *object_mem;
            json_value *next_alloc;
        } _reserved;

#ifdef JSON_TRACK_SOURCE
        // Location of the value in the source JSON
        unsigned int line, col;
#endif
    };
    static_assert(std::is_standard_layout_v<json_value>);
    static_assert(sizeof(json_value) == sizeof(json::value));
    // no assert available to enforce that these two types are layout compatible

    constexpr unsigned char hex_value(char c) noexcept {
        if (c >= '0' && c <= '9')
            return c - '0';

        switch (c) {
            case 'a': case 'A': return 0x0A;
            case 'b': case 'B': return 0x0B;
            case 'c': case 'C': return 0x0C;
            case 'd': case 'D': return 0x0D;
            case 'e': case 'E': return 0x0E;
            case 'f': case 'F': return 0x0F;
            default:
                return 0xFF;
        }
    }

    struct json_state {
        unsigned long used_memory;

        unsigned int uint_max;
        unsigned long ulong_max;

        json::settings settings;
        int first_pass;

        const char *ptr;
        unsigned int cur_line, cur_col;
    };

    void *default_alloc(size_t size, int zero, void *) noexcept {
        return zero ? std::calloc(1, size) : std::malloc(size);
    }

    void default_free(void *ptr, void *) noexcept {
        std::free(ptr);
    }

    void *json_alloc(json_state *state, unsigned long size, bool zero) noexcept {
        if ((state->ulong_max - state->used_memory) < size)
            return nullptr;

        if (state->settings.max_memory
            && (state->used_memory += size) > state->settings.max_memory) {
            return nullptr;
        }

        return state->settings.mem_alloc(size, zero, state->settings.user_data);
    }

    bool new_value(json_state *state,
                   json_value **top, json_value **root, json_value **alloc,
                   json::type type) noexcept
    {
        json_value *value;
        int values_size;

        if (!state->first_pass) {
            value = *top = *alloc;
            *alloc = (*alloc)->_reserved.next_alloc;

            if (!*root)
                *root = value;

            switch (value->type) {
                case json::json_array:
                    if (value->u.array.length == 0)
                        break;

                    if (!(value->u.array.values = (json_value **) json_alloc
                            (state, value->u.array.length * sizeof(json_value *), false))) {
                        return false;
                    }

                    value->u.array.length = 0;
                    break;

                case json::json_object:
                    if (value->u.object.length == 0)
                        break;

                    values_size = sizeof(*value->u.object.values) * value->u.object.length;

                    if (!(value->u.object.values = (object_entry *) json_alloc
                            (state, values_size + ((unsigned long) value->u.object.values), false))) {
                        return false;
                    }

                    value->_reserved.object_mem =
                            (*(char **) &value->u.object.values) + values_size;
                    value->u.object.length = 0;
                    break;

                case json::json_string:
                    if (!(value->u.string.ptr = (char *) json_alloc
                            (state, (value->u.string.length + 1) * sizeof(char), false))) {
                        return false;
                    }

                    value->u.string.length = 0;
                    break;

                default:
                    break;
            }

            return true;
        }

        if (!(value = (json_value *) json_alloc
                (state, sizeof(value) + state->settings.value_extra, true))) {
            return false;
        }

        if (!*root)
            *root = value;

        value->type = type;
        value->parent = *top;

#ifdef JSON_TRACK_SOURCE
        value->line = state->cur_line;
        value->col = state->cur_col;
#endif
        if (*alloc)
            (*alloc)->_reserved.next_alloc = value;

        *alloc = *top = value;
        return true;
    }

    constexpr static long
            flag_next = 1 << 0,
            flag_reproc = 1 << 1,
            flag_need_comma = 1 << 2,
            flag_seek_value = 1 << 3,
            flag_escaped = 1 << 4,
            flag_string = 1 << 5,
            flag_need_colon = 1 << 6,
            flag_done = 1 << 7,
            flag_num_negative = 1 << 8,
            flag_num_zero = 1 << 9,
            flag_num_e = 1 << 10,
            flag_num_e_got_sign = 1 << 11,
            flag_num_e_negative = 1 << 12,
            flag_line_comment = 1 << 13,
            flag_block_comment = 1 << 14;
}

#define WHITESPACE \
    case '\n': ++ state.cur_line;  state.cur_col = 0; \
    case ' ': case '\t': case '\r'

#define STRING_ADD(b)  \
    do { if (!state.first_pass) string [string_length] = b;  ++ string_length; } while (0)

#define LINE_AND_COL \
    state.cur_line, state.cur_col

const json::value * json::parse(const json::settings & settings,
                                const char * json,
                                size_t length,
                                char * error_buf) noexcept
{
    // Skip UTF-8 BOM
    if (length >= 3 && ((unsigned char) json[0]) == 0xEF
        && ((unsigned char) json[1]) == 0xBB
        && ((unsigned char) json[2]) == 0xBF) {
        json += 3;
        length -= 3;
    }

    char error[json::error_max];
    error[0] = '\0';
    const char *const end = (json + length);

    json_state state = {0};
    state.settings = settings;

    if (!state.settings.mem_alloc)
        state.settings.mem_alloc = default_alloc;

    if (!state.settings.mem_free)
        state.settings.mem_free = default_free;

    // limit of how much can be added before next check
    state.uint_max = std::numeric_limits<decltype(state.uint_max)>::max() - 8;
    state.ulong_max = std::numeric_limits<decltype(state.ulong_max)>::max() - 8;

    json_value *top, *root, *alloc = nullptr;
    for (state.first_pass = 1; state.first_pass >= 0; --state.first_pass) {
        top = root = nullptr;
        long flags = flag_seek_value;
        state.cur_line = 1;
        char *string = nullptr;
        unsigned int string_length = 0;
        long num_digits = 0, num_e = 0;
        int64_t num_fraction = 0;

        for (state.ptr = json;; ++state.ptr) {
            char b = (state.ptr == end ? 0 : *state.ptr);

            if (flags & flag_string) {
                if (!b) {
                    std::sprintf(error,
                                 "Unexpected EOF in string (at %d:%d)",
                                 LINE_AND_COL);
                    goto e_failed;
                }

                if (string_length > state.uint_max)
                    goto e_overflow;

                if (flags & flag_escaped) {
                    flags &= ~flag_escaped;
                    unsigned char uc_b1, uc_b2, uc_b3, uc_b4;
                    uint32_t uchar;

                    switch (b) {
                        case 'b': STRING_ADD('\b'); break;
                        case 'f': STRING_ADD('\f'); break;
                        case 'n': STRING_ADD('\n'); break;
                        case 'r': STRING_ADD('\r'); break;
                        case 't': STRING_ADD('\t'); break;
                        case 'u':
                            if (end - state.ptr <= 4 ||
                                (uc_b1 = hex_value(*++state.ptr)) == 0xFF ||
                                (uc_b2 = hex_value(*++state.ptr)) == 0xFF ||
                                (uc_b3 = hex_value(*++state.ptr)) == 0xFF ||
                                (uc_b4 = hex_value(*++state.ptr)) == 0xFF) {
                                std::sprintf(error,
                                             "Invalid character value `%c` (at %d:%d)", b,
                                             LINE_AND_COL);
                                goto e_failed;
                            }

                            uc_b1 = (uc_b1 << 4) | uc_b2;
                            uc_b2 = (uc_b3 << 4) | uc_b4;
                            uchar = (uc_b1 << 8) | uc_b2;

                            if ((uchar & 0xF800) == 0xD800) {
                                if (end - state.ptr <= 6 ||
                                    (*++state.ptr) != '\\' ||
                                    (*++state.ptr) != 'u' ||
                                    (uc_b1 = hex_value(*++state.ptr)) == 0xFF ||
                                    (uc_b2 = hex_value(*++state.ptr)) == 0xFF ||
                                    (uc_b3 = hex_value(*++state.ptr)) == 0xFF ||
                                    (uc_b4 = hex_value(*++state.ptr)) == 0xFF) {
                                    std::sprintf(error,
                                                 "Invalid character value `%c` (at %d:%d)", b,
                                                 LINE_AND_COL);
                                    goto e_failed;
                                }

                                uc_b1 = (uc_b1 << 4) | uc_b2;
                                uc_b2 = (uc_b3 << 4) | uc_b4;
                                const uint32_t uchar2 = (uc_b1 << 8) | uc_b2;
                                uchar = 0x010000 | ((uchar & 0x3FF) << 10) | (uchar2 & 0x3FF);
                            }

                            if (uchar <= 0x7F) {
                                STRING_ADD((char) uchar);
                                break;
                            }

                            if (uchar <= 0x7FF) {
                                if (state.first_pass)
                                    string_length += 2;
                                else {
                                    string[string_length++] = 0xC0 | (uchar >> 6);
                                    string[string_length++] = 0x80 | (uchar & 0x3F);
                                }

                                break;
                            }

                            if (uchar <= 0xFFFF) {
                                if (state.first_pass)
                                    string_length += 3;
                                else {
                                    string[string_length++] = 0xE0 | (uchar >> 12);
                                    string[string_length++] = 0x80 | ((uchar >> 6) & 0x3F);
                                    string[string_length++] = 0x80 | (uchar & 0x3F);
                                }

                                break;
                            }

                            if (state.first_pass)
                                string_length += 4;
                            else {
                                string[string_length++] = 0xF0 | (uchar >> 18);
                                string[string_length++] = 0x80 | ((uchar >> 12) & 0x3F);
                                string[string_length++] = 0x80 | ((uchar >> 6) & 0x3F);
                                string[string_length++] = 0x80 | (uchar & 0x3F);
                            }

                            break;

                        default:
                            STRING_ADD(b);
                    }

                    continue;
                }

                if (b == '\\') {
                    flags |= flag_escaped;
                    continue;
                }

                if (b == '"') {
                    if (!state.first_pass)
                        string[string_length] = 0;

                    flags &= ~flag_string;
                    string = nullptr;

                    switch (top->type) {
                        case json_string:
                            top->u.string.length = string_length;
                            flags |= flag_next;
                            break;

                        case json_object:
                            if (state.first_pass)
                                (*(char **) &top->u.object.values) += string_length + 1;
                            else {
                                top->u.object.values[top->u.object.length].name
                                        = (char *) top->_reserved.object_mem;
                                top->u.object.values[top->u.object.length].name_length
                                        = string_length;
                                (*(char **) &top->_reserved.object_mem) += string_length + 1;
                            }

                            flags |= flag_seek_value | flag_need_colon;
                            continue;

                        default:
                            break;
                    }
                } else {
                    STRING_ADD(b);
                    continue;
                }
            }

            if (state.settings.allow_comments) {
                if (flags & (flag_line_comment | flag_block_comment)) {
                    if (flags & flag_line_comment) {
                        if (b == '\r' || b == '\n' || !b) {
                            flags &= ~flag_line_comment;
                            --state.ptr;  // so null can be reproc'd
                        }

                        continue;
                    }

                    if (flags & flag_block_comment) {
                        if (!b) {
                            std::sprintf(error,
                                         "%d:%d: Unexpected EOF in block comment",
                                         LINE_AND_COL);
                            goto e_failed;
                        }

                        if (b == '*' && state.ptr < (end - 1) && state.ptr[1] == '/') {
                            flags &= ~flag_block_comment;
                            ++state.ptr;  // skip closing sequence
                        }

                        continue;
                    }
                } else if (b == '/') {
                    if (!(flags & (flag_seek_value | flag_done)) && top->type != json_object) {
                        std::sprintf(error,
                                     "%d:%d: Comment not allowed here",
                                     LINE_AND_COL);
                        goto e_failed;
                    }

                    if (++state.ptr == end) {
                        std::sprintf(error,
                                     "%d:%d: EOF unexpected",
                                     LINE_AND_COL);
                        goto e_failed;
                    }

                    switch (b = *state.ptr) {
                        case '/':
                            flags |= flag_line_comment;
                            continue;

                        case '*':
                            flags |= flag_block_comment;
                            continue;

                        default:
                            std::sprintf(error,
                                         "%d:%d: Unexpected `%c` in comment opening sequence",
                                         LINE_AND_COL, b);
                            goto e_failed;
                    }
                }
            }

            if (flags & flag_done) {
                if (!b)
                    break;

                switch (b) {
                    WHITESPACE:
                        continue;

                    default:
                        std::sprintf(error,
                                     "%d:%d: Trailing garbage: `%c`",
                                     LINE_AND_COL, b);
                        goto e_failed;
                }
            }

            if (flags & flag_seek_value) {
                switch (b) {
                    WHITESPACE:
                        continue;

                    case ']':
                        if (top && top->type == json_array)
                            flags = (flags & ~(flag_need_comma | flag_seek_value)) | flag_next;
                        else {
                            std::sprintf(error,
                                         "%d:%d: Unexpected ]",
                                         LINE_AND_COL);
                            goto e_failed;
                        }

                        break;

                    default:
                        if (flags & flag_need_comma) {
                            if (b == ',') {
                                flags &= ~flag_need_comma;
                                continue;
                            } else {
                                std::sprintf(error,
                                             "%d:%d: Expected , before %c",
                                             LINE_AND_COL, b);
                                goto e_failed;
                            }
                        }

                        if (flags & flag_need_colon) {
                            if (b == ':') {
                                flags &= ~flag_need_colon;
                                continue;
                            } else {
                                std::sprintf(error,
                                             "%d:%d: Expected : before %c",
                                             LINE_AND_COL, b);
                                goto e_failed;
                            }
                        }

                        flags &= ~flag_seek_value;

                        switch (b) {
                            case '{':
                                if (!new_value(&state, &top, &root, &alloc, json_object))
                                    goto e_alloc_failure;

                                continue;

                            case '[':
                                if (!new_value(&state, &top, &root, &alloc, json_array))
                                    goto e_alloc_failure;

                                flags |= flag_seek_value;
                                continue;

                            case '"':
                                if (!new_value(&state, &top, &root, &alloc, json_string))
                                    goto e_alloc_failure;

                                flags |= flag_string;
                                string = top->u.string.ptr;
                                string_length = 0;
                                continue;

                            case 't':
                                if ((end - state.ptr) < 3 ||
                                    *(++state.ptr) != 'r' ||
                                    *(++state.ptr) != 'u' ||
                                    *(++state.ptr) != 'e') {
                                    goto e_unknown_value;
                                }

                                if (!new_value(&state, &top, &root, &alloc, json_boolean))
                                    goto e_alloc_failure;

                                top->u.boolean = true;
                                flags |= flag_next;
                                break;

                            case 'f':
                                if ((end - state.ptr) < 4 ||
                                    *(++state.ptr) != 'a' ||
                                    *(++state.ptr) != 'l' ||
                                    *(++state.ptr) != 's' ||
                                    *(++state.ptr) != 'e') {
                                    goto e_unknown_value;
                                }

                                if (!new_value(&state, &top, &root, &alloc, json_boolean))
                                    goto e_alloc_failure;

                                flags |= flag_next;
                                break;

                            case 'n':
                                if ((end - state.ptr) < 3 ||
                                    *(++state.ptr) != 'u' ||
                                    *(++state.ptr) != 'l' ||
                                    *(++state.ptr) != 'l') {
                                    goto e_unknown_value;
                                }

                                if (!new_value(&state, &top, &root, &alloc, json_null))
                                    goto e_alloc_failure;

                                flags |= flag_next;
                                break;

                            default:
                                if (std::isdigit(b) || b == '-') {
                                    if (!new_value(&state, &top, &root, &alloc, json_integer))
                                        goto e_alloc_failure;

                                    if (!state.first_pass) {
                                        while (std::isdigit(b) ||
                                               b == '+' ||
                                               b == '-' ||
                                               b == 'e' ||
                                               b == 'E' ||
                                               b == '.') {
                                            if ((++state.ptr) == end) {
                                                b = 0;
                                                break;
                                            }

                                            b = *state.ptr;
                                        }

                                        flags |= flag_next | flag_reproc;
                                        break;
                                    }

                                    flags &= ~(flag_num_negative | flag_num_e |
                                               flag_num_e_got_sign | flag_num_e_negative |
                                               flag_num_zero);

                                    num_digits = 0;
                                    num_fraction = 0;
                                    num_e = 0;

                                    if (b != '-') {
                                        flags |= flag_reproc;
                                        break;
                                    }

                                    flags |= flag_num_negative;
                                    continue;
                                } else {
                                    std::sprintf(error,
                                                 "%d:%d: Unexpected %c when seeking value",
                                                 LINE_AND_COL, b);
                                    goto e_failed;
                                }
                        }
                }
            } else {
                switch (top->type) {
                    case json_object:
                        switch (b) {
                            WHITESPACE:
                                continue;

                            case '"':
                                if (flags & flag_need_comma) {
                                    std::sprintf(error,
                                                 "%d:%d: Expected , before \"",
                                                 LINE_AND_COL);
                                    goto e_failed;
                                }

                                flags |= flag_string;
                                string = (char *) top->_reserved.object_mem;
                                string_length = 0;
                                break;

                            case '}':
                                flags = (flags & ~flag_need_comma) | flag_next;
                                break;

                            case ',':
                                if (flags & flag_need_comma) {
                                    flags &= ~flag_need_comma;
                                    break;
                                }

                            default:
                                std::sprintf(error,
                                             "%d:%d: Unexpected `%c` in object",
                                             LINE_AND_COL, b);
                                goto e_failed;
                        }


                        break;

                    case json_integer:
                    case json_double:
                        if (std::isdigit(b)) {
                            ++num_digits;

                            if (top->type == json_integer || flags & flag_num_e) {
                                if (!(flags & flag_num_e)) {
                                    if (flags & flag_num_zero) {
                                        std::sprintf(error,
                                                     "%d:%d: Unexpected `0` before `%c`",
                                                     LINE_AND_COL, b);
                                        goto e_failed;
                                    }

                                    if (num_digits == 1 && b == '0')
                                        flags |= flag_num_zero;
                                } else {
                                    flags |= flag_num_e_got_sign;
                                    num_e = (num_e * 10) + (b - '0');
                                    continue;
                                }

                                top->u.integer = (top->u.integer * 10) + (b - '0');
                                continue;
                            }

                            num_fraction = (num_fraction * 10) + (b - '0');
                            continue;
                        }

                        if (b == '+' || b == '-') {
                            if ((flags & flag_num_e) && !(flags & flag_num_e_got_sign)) {
                                flags |= flag_num_e_got_sign;

                                if (b == '-')
                                    flags |= flag_num_e_negative;
                                continue;
                            }
                        } else if (b == '.' && top->type == json_integer) {
                            if (!num_digits) {
                                std::sprintf(error,
                                             "%d:%d: Expected digit before `.`",
                                             LINE_AND_COL);
                                goto e_failed;
                            }

                            top->type = json_double;
                            top->u.dbl = (double) top->u.integer;
                            num_digits = 0;
                            continue;
                        }

                        if (!(flags & flag_num_e)) {
                            if (top->type == json_double) {
                                if (!num_digits) {
                                    std::sprintf(error,
                                                 "%d:%d: Expected digit after `.`",
                                                 LINE_AND_COL);
                                    goto e_failed;
                                }

                                top->u.dbl +=
                                        ((double) num_fraction) /
                                        (std::pow(10.0, (double) num_digits));
                            }

                            if (b == 'e' || b == 'E') {
                                flags |= flag_num_e;
                                if (top->type == json_integer) {
                                    top->type = json_double;
                                    top->u.dbl = (double) top->u.integer;
                                }

                                num_digits = 0;
                                flags &= ~flag_num_zero;
                                continue;
                            }
                        } else {
                            if (!num_digits) {
                                std::sprintf(error,
                                             "%d:%d: Expected digit after `e`",
                                             LINE_AND_COL);
                                goto e_failed;
                            }

                            top->u.dbl *= std::pow(10.0, (double)
                                    (flags & flag_num_e_negative ? -num_e : num_e));
                        }

                        if (flags & flag_num_negative) {
                            if (top->type == json_integer)
                                top->u.integer = -top->u.integer;
                            else
                                top->u.dbl = -top->u.dbl;
                        }

                        flags |= flag_next | flag_reproc;
                        break;

                    default:
                        break;
                }
            }

            if (flags & flag_reproc) {
                flags &= ~flag_reproc;
                --state.ptr;
            }

            if (flags & flag_next) {
                flags = (flags & ~flag_next) | flag_need_comma;

                if (!top->parent) {
                    // root value done
                    flags |= flag_done;
                    continue;
                }

                if (top->parent->type == json_array)
                    flags |= flag_seek_value;

                if (!state.first_pass) {
                    json_value *parent = top->parent;

                    switch (parent->type) {
                        case json_object:
                            parent->u.object.values
                            [parent->u.object.length].value = top;
                            break;

                        case json_array:
                            parent->u.array.values
                            [parent->u.array.length] = top;
                            break;

                        default:
                            break;
                    }
                }

                if ((++top->parent->u.array.length) > state.uint_max)
                    goto e_overflow;

                top = top->parent;
                continue;
            }
        }

        alloc = root;
    }

    return reinterpret_cast<json::value*>(root);

    e_unknown_value:
    std::sprintf(error, "%d:%d: Unknown value", LINE_AND_COL);
    goto e_failed;

    e_alloc_failure:
    std::strcpy(error, "Memory allocation failure");
    goto e_failed;

    e_overflow:
    std::sprintf(error, "%d:%d: Too long (caught overflow)", LINE_AND_COL);
    goto e_failed;

    e_failed:
    if (error_buf) {
        if (*error)
            std::strcpy(error_buf, error);
        else
            std::strcpy(error_buf, "Unknown error");
    }

    if (state.first_pass)
        alloc = root;

    while (alloc) {
        top = alloc->_reserved.next_alloc;
        state.settings.mem_free(alloc, state.settings.user_data);
        alloc = top;
    }

    if (!state.first_pass)
        json::value_free(state.settings, reinterpret_cast<json::value*>(root));

    return nullptr;
}

const json::value * json::parse(const char * json, size_t length) noexcept {
    const json::settings settings = { 0 };
    return json::parse(settings, json, length, 0);
}

void json::value_free(const json::settings & settings, const json::value * val) noexcept
{
    if (!val)
        return;

    json_value* value = reinterpret_cast<json_value*>(const_cast<json::value*>(val));
    value->parent = nullptr;
    json_value *cur_value;
    while (value) {
        switch (value->type) {
            case json_array:
                if (!value->u.array.length) {
                    settings.mem_free(value->u.array.values, settings.user_data);
                    break;
                }

                value = value->u.array.values[--value->u.array.length];
                continue;

            case json_object:
                if (!value->u.object.length) {
                    settings.mem_free(value->u.object.values, settings.user_data);
                    break;
                }

                value = value->u.object.values[--value->u.object.length].value;
                continue;

            case json_string:
                settings.mem_free(value->u.string.ptr, settings.user_data);
                break;

            default:
                break;
        }

        cur_value = value;
        value = value->parent;
        settings.mem_free(cur_value, settings.user_data);
    }
}

void json::value_free(const json::value * value) noexcept {
    json::settings settings = {0};
    settings.mem_free = default_free;
    json::value_free(settings, value);
}
