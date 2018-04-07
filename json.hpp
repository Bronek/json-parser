
/* vim: set et ts=3 sw=3 sts=3 ft=c:
 *
 * Copyright (C) 2012, 2013, 2014 James McLaughlin et al.  All rights reserved.
 * https://github.com/udp/json-parser
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
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

#ifndef _JSON_HPP
#define _JSON_HPP

#include <cstdint>
#include <type_traits>

namespace json {

    struct settings {
        unsigned long max_memory;
        bool allow_comments;

        // Custom allocator support (leave null to use malloc/free)
        void *(*mem_alloc)(std::size_t, int zero, void *user_data);
        void (*mem_free)(void *, void *user_data);

        void *user_data;  // will be passed to mem_alloc and mem_free
        std::size_t value_extra;  //  how much extra space to allocate for values?
    };

    enum type {
        // json_none = 0 not used anymore
        json_object = 1,
        json_array,
        json_integer,
        json_double,
        json_string,
        json_boolean,
        json_null,
    };

    struct value;

    struct object_entry {
        const char *name;
        unsigned int name_length;
        const json::value *value;
    };
    static_assert(std::is_standard_layout_v<object_entry>);

    struct value {
        const value *parent;
        json::type type;

        union {
            bool boolean;
            int64_t integer;
            double dbl;

            struct {
                unsigned int length;
                const char *ptr; // null terminated
            } string;

            struct {
                unsigned int length;
                const object_entry *values;
            } object;

            struct {
                unsigned int length;
                const value *const *values;
            } array;
        } u;

        union {
            const void *reserved;
        } _reserved;

#ifdef JSON_TRACK_SOURCE
        // Location of the value in the source JSON
        unsigned int line, col;
#endif
    };
    static_assert(std::is_standard_layout_v<value>);

    const value *parse(const char *json, std::size_t length) noexcept;
    constexpr static int error_max = 128;
    const value *parse(const settings &settings, const char *json, std::size_t length, char *error) noexcept;

    void value_free(const value *) noexcept;
    void value_free(const settings &settings, const value *) noexcept;

} // namespace json

#endif
