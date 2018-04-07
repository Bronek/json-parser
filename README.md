Very low footprint JSON parser written in a subset of C++ , forked from https://github.com/udp/json-parser

* BSD licensed with no dependencies (i.e. just drop the files into your project)
* Never recurses or allocates more memory than it needs
* Very simple API

Installing
----------

Just copy json.hpp and json.cpp into required location


API
---

    const json::value * json::parse (const char * json, size_t length);

    const json::value * json::parse (const json::settings & settings,
                                     const char * json,
                                     size_t length
                                     char * error);

Buffer `error` must be at least 128 characters long, otherwise buffer overflow may occur when reporting parsing errors

The `type` field of `json_value` is one of:

* `json_object` (see `u.object.length`, `u.object.values[x].name`, `u.object.values[x].value`)
* `json_array` (see `u.array.length`, `u.array.values`)
* `json_integer` (see `u.integer`)
* `json_double` (see `u.dbl`)
* `json_string` (see `u.string.ptr`, `u.string.length`)
* `json_boolean` (see `u.boolean`)
* `json_null`


Compile-Time Options
--------------------

    -DJSON_TRACK_SOURCE

Stores the source location (line and column number) inside each `json_value`.

This is useful for application-level error reporting.


Runtime Options
---------------

    settings.allow_comments = true;

Enables C-style `// line` and `/* block */` comments.

    size_t value_extra

The amount of space (if any) to allocate at the end of each `json_value`, in
order to give the application space to add metadata.

    void * (* mem_alloc) (size_t, int zero, void * user_data);
    void (* mem_free) (void *, void * user_data);

Custom allocator routines.  If NULL, the default `malloc` and `free` will be used.

The `user_data` pointer will be forwarded from `json_settings` to allow application
context to be passed.
