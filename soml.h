/*
soml.h: a reader and writer for SOML, a config format for humans.

The format is described in the SOML specification. This file is the whole library. In exactly one C file, define `SOML_IMPLEMENTATION` before you include it:

	#define SOML_IMPLEMENTATION
	#include "soml.h"

Every other file includes it without the define. Define `SOML_STATIC` as well to make every function `static`, for a library that is private to one file.

The implementation needs C23 (GCC 14 or later, or Clang 19 or later), including 128-bit `_BitInt` support on the target. Clang on Windows with the MSVC target does not link the compiler-rt builtins that 128-bit division needs, so link with `-rtlib=compiler-rt`. The declarations also compile as C99 or later and as C++17 or later, so code built with another standard, or as C++, can include this file and link the implementation from a C23 file.

Reading:

	soml_error error;
	soml_document *document = soml_parse(text, length, nullptr, &error);

	if (document == nullptr) {
		fprintf(stderr, "%s at line %zu, column %zu\n", error.message, error.line, error.column);
		return;
	}

	int64_t port;

	if (soml_get_int(soml_object_get(soml_object_get(soml_document_root(document), "postgres"), "port"), &port)) {
		…
	}

	soml_document_free(document);

Writing:

	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_object(document);
	soml_object_add(document, root, "name", 4, soml_new_string(document, "api", 3));
	soml_object_add(document, root, "replicas", 8, soml_new_int(document, 3));
	soml_document_set_root(document, root);

	soml_string text;
	soml_error error;

	if (soml_write(document, &text, &error)) {
		fwrite(text.data, 1, text.length, stdout);
	}

	soml_document_free(document);

The license is at the end of this file.
*/

#ifndef SOML_H
#define SOML_H

#include <stddef.h>
#include <stdint.h>

// The declarations also work from C99 to C17, so that files built with an older standard can use a library built as C23. The checks for C23 use `202000L`, because GCC 14 sets `__STDC_VERSION__` to that value for `-std=c23` instead of `202311L`.
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202000L)
	#define SOML_NODISCARD [[nodiscard]]
#else
	#include <stdbool.h>
	#define SOML_NODISCARD
#endif

// With Clang's -fbounds-safety, the pointers in the declarations carry their bounds, so a caller's buffers are checked. The implementation itself does not support the flag.
#if defined(__has_feature) && !defined(__cplusplus)
	#if __has_feature(bounds_safety)
		#define SOML__HAS_BOUNDS_SAFETY 1
	#endif
#endif

#ifdef SOML__HAS_BOUNDS_SAFETY
	#include <ptrcheck.h>
	__ptrcheck_abi_assume_single()
	#define SOML_COUNTED_BY(count) __counted_by(count)
	#define SOML_COUNTED_BY_OR_NULL(count) __counted_by_or_null(count)
	#define SOML_SIZED_BY_OR_NULL(size) __sized_by_or_null(size)
#else
	#define SOML_COUNTED_BY(count)
	#define SOML_COUNTED_BY_OR_NULL(count)
	#define SOML_SIZED_BY_OR_NULL(size)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SOMLDEF
	#ifdef SOML_STATIC
		#define SOMLDEF [[maybe_unused]] static
	#else
		#define SOMLDEF extern
	#endif
#endif

// Annotations that let the compiler and its static analyzer check a caller: that a document is freed once with `soml_document_free` and not used after, that a pointer has as many bytes as the length that comes with it, and that a key is NUL-terminated. None of them changes the generated code. They are left out where the implementation is compiled, because GCC's specialized copies of a function lose its annotations and give false warnings there.
#if !defined(SOML_IMPLEMENTATION) && defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
	// Only the form that names the deallocator. The bare `malloc` attribute would also promise that the document holds no pointers to existing memory, which is false, because it holds the allocator's context.
	#define SOML__FREED_WITH_DOCUMENT_FREE __attribute__((malloc(soml_document_free, 1)))
	#define SOML__READS(pointer, size) __attribute__((access(read_only, pointer, size)))
	#define SOML__NUL_TERMINATED(index) __attribute__((null_terminated_string_arg(index)))
#else
	#define SOML__FREED_WITH_DOCUMENT_FREE
	#define SOML__READS(pointer, size)
	#define SOML__NUL_TERMINATED(index)
#endif

// For Clang's static analyzer, from Clang 21, as older versions track ownership attributes only for `malloc`. `soml_parse` is left out, because the analyzer then treats its `error` output as never written.
#if !defined(SOML_IMPLEMENTATION) && defined(__clang__)
	#define SOML__RETURNS_DOCUMENT __attribute__((ownership_returns(soml_document)))
	#define SOML__TAKES_DOCUMENT __attribute__((ownership_takes(soml_document, 1)))
#else
	#define SOML__RETURNS_DOCUMENT
	#define SOML__TAKES_DOCUMENT
#endif

/**
A parsed or built document. It owns every value, string, and key in it, and `soml_document_free` releases all of them at once.

Separate documents can be used on separate threads. One document can be read from several threads at once, with the accessors, `soml_write_with_callback`, and `soml_write_canonical_with_callback`, as long as no thread changes it. The builder functions, `soml_write`, and `soml_write_canonical`, which store their text in the document, change it. The callback writers can allocate temporary memory with the document's allocator, to sort the keys of a built object, so that allocator must be thread-safe too, which the default one is.
*/
typedef struct soml_document soml_document;

/**
A value in a document. It is valid until its document is freed.
*/
typedef struct soml_value soml_value;

/**
The type of a value.
*/
typedef enum soml_type {
	/**
	Not a value: what `soml_type_of` returns for `nullptr`.
	*/
	SOML_TYPE_NONE,
	SOML_TYPE_STRING,
	SOML_TYPE_INT,
	SOML_TYPE_FLOAT,
	SOML_TYPE_BOOL,
	SOML_TYPE_NULL,
	SOML_TYPE_INSTANT,
	SOML_TYPE_DURATION,
	SOML_TYPE_ARRAY,
	SOML_TYPE_OBJECT,
} soml_type;

/**
What went wrong.
*/
typedef enum soml_error_code {
	SOML_ERROR_NONE,

	/**
	The text is not a valid SOML document.
	*/
	SOML_ERROR_SYNTAX,

	/**
	The document or value is nested more than 100 levels deep, which the specification does not allow.
	*/
	SOML_ERROR_TOO_DEEP,

	/**
	The allocator returned `nullptr`.
	*/
	SOML_ERROR_OUT_OF_MEMORY,

	/**
	A value cannot be written, because SOML cannot represent it: NaN, a string that is not valid UTF-8 or that holds a carriage return, an instant outside the years 0001 to 9999, a duplicate key, or a root that is not an object or an array.
	*/
	SOML_ERROR_INVALID_VALUE,

	/**
	The write callback returned `false`, or a caller-owned output buffer was too small.
	*/
	SOML_ERROR_WRITE,

	/**
	A function got an argument it cannot use, such as `nullptr` text with a nonzero length, a `nullptr` value for the builder, a value from another document, or a value that is already in a container.
	*/
	SOML_ERROR_INVALID_ARGUMENT,
} soml_error_code;

/**
Where and why reading or writing failed.

`message` is a static string, so it never needs to be freed. `offset` is the byte offset of the problem in the text, and `line` and `column` are 1-based, with the column counted in Unicode scalar values. A `SOML_ERROR_SYNTAX` or `SOML_ERROR_TOO_DEEP` from `soml_parse` always has a position. Other errors may have a line and column of 0.
*/
typedef struct soml_error {
	soml_error_code code;
	const char *message;
	size_t offset;
	size_t line;
	size_t column;
} soml_error;

/**
A string or a key: UTF-8 bytes and their length. The bytes are followed by a NUL, but a string can also hold U+0000 (written `\u{0}`), so use `length`.
*/
typedef struct soml_string {
	// The bounds include the NUL after the bytes, so that a caller can read it, and `data` is `nullptr` for a missing key.
	const char *SOML_COUNTED_BY_OR_NULL(length + 1) data;
	size_t length;
} soml_string;

/**
An object member, in the document's member order. Its key and value belong to the document.
*/
typedef struct soml_member {
	soml_string key;
	soml_value *value;
} soml_member;

/**
A borrowed array range. `data` is read-only, but its values can be used with the builder. Reacquire the view after any builder call on the array, and do not use it after the document is freed.
*/
typedef struct soml_array_view {
	soml_value *const *SOML_COUNTED_BY_OR_NULL(count) data;
	size_t count;
} soml_array_view;

/**
A borrowed object range, in member order. Reacquire the view after any builder call on the object, and do not use it after the document is freed.
*/
typedef struct soml_object_view {
	const soml_member *SOML_COUNTED_BY_OR_NULL(count) data;
	size_t count;
} soml_object_view;

/**
An instant, in UTC: seconds since 1970-01-01T00:00:00Z and nanoseconds from 0 to 999,999,999.

The range is 0001-01-01T00:00:00Z to 9999-12-31T23:59:59.999999999Z.
*/
typedef struct soml_instant {
	int64_t seconds;
	int32_t nanoseconds;
} soml_instant;

/**
Memory for a document.

`reallocate` works like `realloc`, with the old size passed too: it gets `nullptr` and an old size of 0 to allocate, and a new size of 0 to free, when it returns `nullptr`. `context` is passed through. With `nullptr` instead of an allocator, or a `reallocate` of `nullptr`, the library uses `malloc`, `realloc`, and `free`.

The allocator is copied into the document, so the struct itself need not stay valid. Its callback and context must stay valid until the document is freed. It must return memory aligned for any type, as `malloc` does, and a failed reallocation must leave the original allocation unchanged.
*/
typedef struct soml_allocator {
	void *SOML_SIZED_BY_OR_NULL(new_size) (*reallocate)(void *context, void *SOML_SIZED_BY_OR_NULL(old_size) pointer, size_t old_size, size_t new_size);
	void *context;
} soml_allocator;

// Declared before the functions that return a document, because their annotation for GCC names it.

/**
Free a document and everything in it. Accepts `nullptr`.
*/
SOMLDEF SOML__TAKES_DOCUMENT void soml_document_free(soml_document *document);

/**
Parse a document.

`text` is UTF-8 and does not need a NUL terminator. It can be freed once this function returns, because the document copies what it keeps. `allocator` can be `nullptr`, and so can `error`.

Returns `nullptr` and fills in `error` when the text is invalid, `text` is `nullptr` with a nonzero length, or allocation fails. Checks every specification rule, including duplicate keys, numeric and temporal ranges, and the nesting limit of 100 levels.
*/
SOML_NODISCARD SOMLDEF SOML__FREED_WITH_DOCUMENT_FREE SOML__READS(1, 2) soml_document *soml_parse(const char *SOML_COUNTED_BY(length) text, size_t length, const soml_allocator *allocator, soml_error *error);

/**
Create an empty document, to build values in and write.

Returns `nullptr` when the allocator fails.
*/
SOML_NODISCARD SOMLDEF SOML__FREED_WITH_DOCUMENT_FREE SOML__RETURNS_DOCUMENT soml_document *soml_document_create(const soml_allocator *allocator);

/**
The root value: an object or an array for a parsed document, or whatever `soml_document_set_root` set. `nullptr` for `nullptr`.
*/
SOML_NODISCARD SOMLDEF soml_value *soml_document_root(const soml_document *document);

/**
The first builder failure, without writing the document. Returns `false` and leaves `output` unchanged when there is no builder failure or either argument is `nullptr`. Writer failures are retriable and are not stored here.
*/
SOML_NODISCARD SOMLDEF bool soml_document_get_error(const soml_document *document, soml_error *output);

/**
The bytes currently retained by the document, including its own allocation, arena chunks, and stored writer output. Excludes temporary parser/writer memory and allocator overhead. Returns 0 for `nullptr`, or `SIZE_MAX` if the sum does not fit.
*/
SOML_NODISCARD SOMLDEF size_t soml_document_allocated_bytes(const soml_document *document);

/**
The type of a value, or `SOML_TYPE_NONE` for `nullptr`.
*/
SOML_NODISCARD SOMLDEF soml_type soml_type_of(const soml_value *value);

/**
The value of a string. Returns `false`, and leaves `output` unchanged, when `value` is `nullptr` or not a string. The same holds for the other getters, which also return `false` when `output` is `nullptr`. Use `soml_type_of` to check only the type.
*/
SOML_NODISCARD SOMLDEF bool soml_get_string(const soml_value *value, soml_string *output);

/**
The value of an int. A float is not an int, even when it has no fraction.
*/
SOML_NODISCARD SOMLDEF bool soml_get_int(const soml_value *value, int64_t *output);

/**
The value of a float: finite, `INFINITY`, or `-INFINITY`. An int is not a float.
*/
SOML_NODISCARD SOMLDEF bool soml_get_float(const soml_value *value, double *output);

/**
The value of a bool.
*/
SOML_NODISCARD SOMLDEF bool soml_get_bool(const soml_value *value, bool *output);

/**
The value of an instant.
*/
SOML_NODISCARD SOMLDEF bool soml_get_instant(const soml_value *value, soml_instant *output);

/**
The value of a duration, in nanoseconds.
*/
SOML_NODISCARD SOMLDEF bool soml_get_duration(const soml_value *value, int64_t *nanoseconds);

/**
Whether the value is `null`. Returns `false` for `nullptr`, which is not a value.
*/
SOML_NODISCARD SOMLDEF bool soml_is_null(const soml_value *value);

/**
The number of items in an array or members in an object, and 0 for anything else.
*/
SOML_NODISCARD SOMLDEF size_t soml_count(const soml_value *value);

/**
An item of an array, or `nullptr` when `value` is not an array or `index` is out of range.
*/
SOML_NODISCARD SOMLDEF soml_value *soml_array_at(const soml_value *value, size_t index);

/**
Borrow all array items for iteration without one accessor call per item. Returns `{nullptr, 0}` for `nullptr` or a value that is not an array.
*/
SOML_NODISCARD SOMLDEF soml_array_view soml_array_items(const soml_value *array);

/**
Borrow all object members in their original order. Returns `{nullptr, 0}` for `nullptr` or a value that is not an object.
*/
SOML_NODISCARD SOMLDEF soml_object_view soml_object_members(const soml_value *object);

/**
The value of the member with this key, or `nullptr` when there is none or `object` is not an object. The key is NUL-terminated. Use `soml_object_get_with_length` for a key that holds U+0000.

Lookup is a binary search for an unchanged parsed object. Adding members can make it a linear scan. For an object built with `soml_object_add`, it stays a binary search while the keys are added in sorted order, and becomes a linear scan otherwise, until `soml_object_index` builds an index. Replacements and removals preserve an index. When a built object has a key twice, which `soml_write` refuses, lookup finds the first.
*/
SOML_NODISCARD SOMLDEF SOML__NUL_TERMINATED(2) soml_value *soml_object_get(const soml_value *object, const char *key);

/**
Like `soml_object_get`, with a key of `length` bytes.
*/
SOML_NODISCARD SOMLDEF SOML__READS(2, 3) soml_value *soml_object_get_with_length(const soml_value *object, const char *SOML_COUNTED_BY(length) key, size_t length);

/**
The key of a member. Members are in the order they appear in the source, or in the order they were added.

Returns `{nullptr, 0}` when `object` is not an object or `index` is out of range.
*/
SOML_NODISCARD SOMLDEF soml_string soml_object_key_at(const soml_value *object, size_t index);

/**
The value of a member, in the same order as `soml_object_key_at`, or `nullptr` when `object` is not an object or `index` is out of range.
*/
SOML_NODISCARD SOMLDEF soml_value *soml_object_value_at(const soml_value *object, size_t index);

/**
New values for building a document. Each belongs to `document`, and can only be added to containers in that document: the builder refuses a value from another document, because that document could be freed first.

They return `nullptr` when the allocator fails or `document` is `nullptr`, and `soml_new_string` also when `data` is `nullptr` but `length` is not 0. Every builder function accepts `nullptr` values and then fails, so a builder can be checked once, at the end.

The document records the first failure of any builder function, and `soml_write` then fails with it, even when the caller handled the failure. So a document whose building failed cannot be written by mistake with a member missing.

A string is copied. Whether it is valid UTF-8, whether a float is NaN, and whether an instant is in range is checked by `soml_write`. Negative zero is stored as zero.
*/
SOMLDEF SOML__READS(2, 3) soml_value *soml_new_string(soml_document *document, const char *SOML_COUNTED_BY(length) data, size_t length);
SOMLDEF soml_value *soml_new_int(soml_document *document, int64_t value);
SOMLDEF soml_value *soml_new_float(soml_document *document, double value);
SOMLDEF soml_value *soml_new_bool(soml_document *document, bool value);
SOMLDEF soml_value *soml_new_null(soml_document *document);
SOMLDEF soml_value *soml_new_instant(soml_document *document, soml_instant value);
SOMLDEF soml_value *soml_new_duration(soml_document *document, int64_t nanoseconds);
SOMLDEF soml_value *soml_new_array(soml_document *document);
SOMLDEF soml_value *soml_new_object(soml_document *document);

/**
Deep-copy a value into `document`, including its strings, keys, and descendants. The result is unattached, so it can be added to any container in the destination document. The source may belong to the same document. Member order and any existing lookup index are preserved.

Returns `nullptr` and records a builder failure for a null source, allocation failure, or nesting deeper than 100 container levels. Scalar representability is checked when writing, just as for the other builder functions. Partial copies stay owned by the destination until it is freed; the source is unchanged.
*/
SOML_NODISCARD SOMLDEF soml_value *soml_clone_value(soml_document *document, const soml_value *value);

/**
Create an independent copy of a valid document using `allocator`, which may be `nullptr`. Preserves member order and lookup indexes without serializing text. The source can be freed immediately after success.

Returns `nullptr` and fills `error` when the source cannot be written or allocation fails. Accepts a null `error`. Temporary duplicate checking uses the source allocator; retained copies use the destination allocator.
*/
SOML_NODISCARD SOMLDEF SOML__FREED_WITH_DOCUMENT_FREE soml_document *soml_document_clone(const soml_document *source, const soml_allocator *allocator, soml_error *error);

/**
Reserve room for at least `capacity` array items without changing the count. Existing items remain unchanged on failure. Returns `false` and records a builder failure for an invalid container, a container from another document, size overflow, or allocation failure.
*/
SOMLDEF bool soml_array_reserve(soml_document *document, soml_value *array, size_t capacity);

/**
Like `soml_array_reserve`, for object members. Keeps the member order and lookup index.
*/
SOMLDEF bool soml_object_reserve(soml_document *document, soml_value *object, size_t capacity);

/**
Insert an item before `index`, from 0 through the array's count. The count itself appends. Uses the same ownership and failure rules as `soml_array_append`.
*/
SOMLDEF bool soml_array_insert(soml_document *document, soml_value *array, size_t index, soml_value *item);

/**
Replace an existing array item. The previous item becomes unattached and may be added elsewhere in its document. Setting the same item again succeeds. An index past the end records a builder failure; insertion uses `soml_array_insert`.
*/
SOMLDEF bool soml_array_set(soml_document *document, soml_value *array, size_t index, soml_value *item);

/**
Remove an existing array item, shifting later items left. The removed value becomes unattached. An index past the end records a builder failure.
*/
SOMLDEF bool soml_array_remove(soml_document *document, soml_value *array, size_t index);

/**
Remove and return the last array item, which becomes unattached. Returns `nullptr` for an empty array without recording a failure; an invalid container records a builder failure.
*/
SOML_NODISCARD SOMLDEF soml_value *soml_array_pop(soml_document *document, soml_value *array);

/**
Remove every array item in one pass, making the values unattached and retaining the reserved capacity for reuse.
*/
SOMLDEF bool soml_array_clear(soml_document *document, soml_value *array);

/**
Add a missing object member or replace its value in place. A replacement keeps its key and member position, and makes the previous value unattached. Setting the same value again succeeds. Uses the same key, ownership, and failure rules as `soml_object_add`.

The key is found as `soml_object_get` finds it, so each call is a linear scan in an object with unsorted keys and no index, and setting many keys takes quadratic time. A call that adds a key can invalidate an index, as any addition can.
*/
SOMLDEF SOML__READS(3, 4) bool soml_object_set(soml_document *document, soml_value *object, const char *SOML_COUNTED_BY(key_length) key, size_t key_length, soml_value *value);

/**
Remove the first member with this key, or succeed without changing anything when it is missing. Keeps the remaining member order and lookup index, and makes the removed value unattached.

The key is found as `soml_object_get` finds it. A removal moves the members after it and rewrites an index, so it can take time linear in the member count.
*/
SOMLDEF SOML__READS(3, 4) bool soml_object_remove(soml_document *document, soml_value *object, const char *SOML_COUNTED_BY(key_length) key, size_t key_length);

/**
Remove and return the first member's value for this key, making it unattached. Returns `nullptr` for a missing key without recording a failure; invalid arguments record a builder failure. It costs the same as `soml_object_remove`.
*/
SOML_NODISCARD SOMLDEF SOML__READS(3, 4) soml_value *soml_object_take(soml_document *document, soml_value *object, const char *SOML_COUNTED_BY(key_length) key, size_t key_length);

/**
Remove every object member in one pass, making the values unattached and retaining the reserved capacity for reuse. Resets the lookup index to an empty sorted object.
*/
SOMLDEF bool soml_object_clear(soml_document *document, soml_value *object);

/**
Append an item to an array. Returns `false` when an argument is `nullptr`, `array` is not an array, `array` or `item` belongs to another document, `item` is already in a container or is the root, or the allocator fails.

A value can be in one place only, as in a tree: a value in several places would be written once for each, which grows exponentially with nesting.

The builder does not check ancestors, so a value added to itself or to one of its own descendants makes a cycle. Nothing outside the cycle can reach it, so it is never written, but `soml_clone_value` of a value in it fails with `SOML_ERROR_TOO_DEEP` and records a builder failure.
*/
SOMLDEF bool soml_array_append(soml_document *document, soml_value *array, soml_value *item);

/**
Add a member to an object. The key is copied.

This does not check for a duplicate key, because a check on every add makes building a large object quadratic. `soml_write` reports one. Returns `false` when an argument is `nullptr` (a key can be `nullptr` with a length of 0), `object` is not an object, `object` or `value` belongs to another document, `value` is already in a container or is the root, or the allocator fails. A value added to itself or to one of its own descendants makes a cycle, as described for `soml_array_append`.
*/
SOMLDEF SOML__READS(3, 4) bool soml_object_add(soml_document *document, soml_value *object, const char *SOML_COUNTED_BY(key_length) key, size_t key_length, soml_value *value);

/**
Set the root of a document, which must be an object or an array when it is written. Returns `false` when `root` is `nullptr`, belongs to another document, or is already in a container. The previous root, if any, can then be added to a container.
*/
SOMLDEF bool soml_document_set_root(soml_document *document, soml_value *root);

/**
Build an object's key index for logarithmic lookup after unsorted insertion. Checks duplicate keys and records a builder failure if duplicates or allocation failure prevent indexing. Does not change member order. Later additions may invalidate the index; replacements and removals keep it usable.

Each call that builds an index allocates it in the document, which keeps it until the document is freed. So indexing after each addition uses quadratic memory: index an object once after a batch of additions.
*/
SOMLDEF bool soml_object_index(soml_document *document, soml_value *object);

/**
Check that a document can be written, without formatting or storing output. Uses the same type, UTF-8, temporal, duplicate-key, and depth rules as the writers. Accepts `nullptr` for `error`; validation itself records no builder failure. Invalid values can be repaired and checked again, but a persistent builder failure still prevents writing. Unindexed objects can require temporary sorting memory.
*/
SOML_NODISCARD SOMLDEF bool soml_validate(const soml_document *document, soml_error *error);

/**
Measure the written text's length, excluding its NUL terminator, without retaining the text. Canonical and member-order writing have the same length. On failure, `length` is unchanged; `error` can be `nullptr`.
*/
SOML_NODISCARD SOMLDEF bool soml_measure(const soml_document *document, size_t *length, soml_error *error);

/**
Write into caller-owned memory, including a NUL terminator. `capacity` includes the terminator. A buffer that is too small fails with `SOML_ERROR_WRITE`; its contents may contain a partial prefix, and `length` remains unchanged. On success, `length` excludes the NUL. `buffer` must not overlap document-owned memory. This does not change the document or retain output. `nullptr` with zero capacity is allowed and reports insufficient space for a valid document.
*/
SOML_NODISCARD SOMLDEF bool soml_write_to_buffer(const soml_document *document, char *SOML_COUNTED_BY(capacity) buffer, size_t capacity, size_t *length, soml_error *error);

/**
Like `soml_write_to_buffer`, with the members sorted by key.
*/
SOML_NODISCARD SOMLDEF bool soml_write_canonical_to_buffer(const soml_document *document, char *SOML_COUNTED_BY(capacity) buffer, size_t capacity, size_t *length, soml_error *error);

/**
Write a document for people to read. Members keep their order in the document, as `soml_object_key_at` lists them: the order of the source for a parsed object, and the order they were added for a built one. Every other rule of canonical form is followed, so the output is canonical form when the members of every object are already sorted by key. Use `soml_write_canonical` for exact canonical form.

The text is stored in the document, so it is freed with it, and ends with one line feed. It is followed by a NUL that is not part of the length.

Returns `false` and fills in `error` when `document` or `output` is `nullptr`, a builder function failed earlier, a value cannot be represented (see `SOML_ERROR_INVALID_VALUE`), nesting exceeds 100 levels (`SOML_ERROR_TOO_DEEP`), or allocation fails. `error` can be `nullptr`.

On failure, `output` is unchanged. A write failure does not record a builder failure; an allocation failure during writing can be retried. Earlier successful output remains valid until the document is freed, including after later writes or builder calls.
*/
SOML_NODISCARD SOMLDEF bool soml_write(soml_document *document, soml_string *output, soml_error *error);

/**
Like `soml_write`, but in exact canonical form, with the members of every object sorted by key. Two documents with the same value give the same bytes, so the text can be hashed, signed, or compared.
*/
SOML_NODISCARD SOMLDEF bool soml_write_canonical(soml_document *document, soml_string *output, soml_error *error);

/**
Write a document like `soml_write`, to a callback, such as one that writes to a file. The callback gets the text in pieces, and returns `false` to stop with `SOML_ERROR_WRITE`.

It fails for the same builder, value, depth, and allocation errors as `soml_write`, when `document` or `write` is `nullptr`, or when the callback returns `false`. On failure, the callback may already have received part of the text. That part can itself be a valid, shorter document without the later members, for example when a later string holds a carriage return. So to save a file, write to a temporary file and rename it only after success. `error` can be `nullptr`.

The callback's bytes are not guaranteed to be NUL-terminated and are valid only during that callback invocation. Consume them by length, and copy them if they must be kept.

The callback must not change or free the document: the text is written while the callback runs.
*/
SOML_NODISCARD SOMLDEF bool soml_write_with_callback(const soml_document *document, bool (*write)(void *context, const char *SOML_COUNTED_BY(length) bytes, size_t length), void *context, soml_error *error);

/**
Like `soml_write_with_callback`, but in exact canonical form, as `soml_write_canonical` writes it.
*/
SOML_NODISCARD SOMLDEF bool soml_write_canonical_with_callback(const soml_document *document, bool (*write)(void *context, const char *SOML_COUNTED_BY(length) bytes, size_t length), void *context, soml_error *error);

#ifdef __cplusplus
}
#endif

#endif // SOML_H

// The implementation does not use C23 digit separators, such as `1'000`. GCC reads the lines of a skipped `#if` block too, and before C23, a `'` begins a character literal, so a caller that includes only the declarations in C99 or C17 would get a warning for each.
#if defined(SOML_IMPLEMENTATION) && !defined(SOML_IMPLEMENTATION_INCLUDED)
#define SOML_IMPLEMENTATION_INCLUDED

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L
	#error "The SOML implementation needs C23, such as GCC 14 or Clang 19 with -std=c23. The declarations alone also work in C99 or later and in C++17 or later."
#endif

#ifdef SOML__HAS_BOUNDS_SAFETY
	#error "The SOML implementation does not support -fbounds-safety. Build the file that defines SOML_IMPLEMENTATION without it. Files that only include the declarations can use it."
#endif

#include <float.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

// Under AddressSanitizer, unused arena space and gaps around allocations are marked as not addressable to help detect underruns and overruns.
#if defined(__has_feature)
	#if __has_feature(address_sanitizer)
		#define SOML__HAS_ADDRESS_SANITIZER 1
	#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
	#define SOML__HAS_ADDRESS_SANITIZER 1
#endif

#ifdef SOML__HAS_ADDRESS_SANITIZER
	#include <sanitizer/asan_interface.h>
	#define SOML__POISON(address, size) ASAN_POISON_MEMORY_REGION(address, size)
	#define SOML__UNPOISON(address, size) ASAN_UNPOISON_MEMORY_REGION(address, size)
#else
	#define SOML__POISON(address, size) ((void)(address), (void)(size))
	#define SOML__UNPOISON(address, size) ((void)(address), (void)(size))
#endif

static_assert(CHAR_BIT == 8, "SOML needs eight-bit bytes");
static_assert('0' == 0x30 && 'A' == 0x41 && 'a' == 0x61 && '\n' == 0x0A, "SOML needs an ASCII-compatible execution character set");

// The nesting limit from the specification. It is exact: 100 levels are valid, and 101 are not.
constexpr size_t soml__maximum_depth = 100;
constexpr size_t soml__minimum_chunk_size = 4096;
constexpr size_t soml__maximum_chunk_size = (size_t)1 << 24;
constexpr uint64_t soml__byte_high_bits = 0x8080808080808080;
constexpr uint64_t soml__byte_ones = 0x0101010101010101;
constexpr uint64_t soml__float_sign_bit = (uint64_t)1 << 63;
constexpr uint64_t soml__float_infinity_bits = 0x7FF0000000000000;
constexpr uint64_t soml__nanoseconds_per_second = 1000000000;
constexpr uint64_t soml__nanoseconds_per_minute = 60 * soml__nanoseconds_per_second;
constexpr uint64_t soml__nanoseconds_per_hour = 60 * soml__nanoseconds_per_minute;

// Keeps a function that has a large local, such as a high-precision decimal, out of the recursive parser and writer, so that every level of nesting costs a small frame. Without it, inlining made each level of the parser take about 1 KB of stack, and 100 levels overflowed a 64 KB stack. It is also on functions without a large local that the recursive parser calls, such as most error diagnoses, for the same reason.
#if defined(__GNUC__) || defined(__clang__)
	#define SOML__NOINLINE [[gnu::noinline]]
#else
	#define SOML__NOINLINE
#endif

// The digits a high-precision decimal holds. 800 is more than the 767 significant digits that deciding the rounding of any binary64 value needs.
constexpr int soml__decimal_capacity = 800;

constexpr int64_t soml__minimum_instant_seconds = -62135596800; // 0001-01-01T00:00:00Z
constexpr int64_t soml__maximum_instant_seconds = 253402300799; // 9999-12-31T23:59:59Z

typedef soml_member soml__member;

struct soml_value {
	soml_type type;
	// Whether the value is in a container or is the root. A value can be in one place only, because a value in several places would be written once for each, which grows exponentially with nesting.
	bool is_attached;
	// For an object: whether members can be read in key order using `as.object.sorted`, or their own order when that pointer is nullptr. It is here rather than in the union, where it would make every value 8 bytes larger.
	bool is_sorted;
	// The document that owns the value, so that the builder can refuse a value from another document, which could be freed first.
	const soml_document *document;

	union {
		soml_string string;
		int64_t integer;
		double number;
		bool boolean;
		soml_instant instant;
		int64_t duration;

		struct {
			soml_value **items;
			size_t count;
			size_t capacity;
		} array;

		struct {
			soml__member *members;
			// The member positions in key order, valid while `is_sorted` is true; nullptr means the members are already in key order.
			size_t *sorted;
			size_t count;
			size_t capacity;
		} object;
	} as;
};

typedef struct soml__chunk {
	struct soml__chunk *next;
	size_t size;
	max_align_t data[];
} soml__chunk;

struct soml_document {
	soml_allocator allocator;
	soml__chunk *chunks;
	size_t chunk_used;
	size_t next_chunk_size;
	soml_value *root;
	// The first builder failure, which `soml_write` reports.
	soml_error_code failure;
	const char *failure_message;
};

static const char soml__out_of_memory_message[] = "Out of memory";

// MARK: Memory

[[nodiscard]] static void *soml__default_reallocate(void *context, void *pointer, size_t old_size, size_t new_size) {
	(void)context;
	(void)old_size;

	if (new_size == 0) {
		free(pointer);
		return nullptr;
	}

	return realloc(pointer, new_size);
}

[[nodiscard]] static soml_allocator soml__resolve_allocator(const soml_allocator *allocator) {
	if (allocator == nullptr || allocator->reallocate == nullptr) {
		return (soml_allocator){.reallocate = soml__default_reallocate, .context = nullptr};
	}

	return *allocator;
}

/*
The capacity to grow to, doubling from at least `minimum` until it holds `needed` elements, and its size in bytes. Returns `false` when the size overflows.
*/
[[nodiscard]] static bool soml__next_capacity(size_t capacity, size_t needed, size_t minimum, size_t element_size, size_t *new_capacity, size_t *new_size) {
	size_t grown_capacity = capacity < minimum ? minimum : capacity;

	while (grown_capacity < needed) {
		if (ckd_mul(&grown_capacity, grown_capacity, 2)) {
			return false;
		}
	}

	size_t size;

	if (ckd_mul(&size, grown_capacity, element_size)) {
		return false;
	}

	*new_capacity = grown_capacity;
	*new_size = size;
	return true;
}

/*
Grows a buffer of `*capacity` elements so it holds at least `needed`. Returns the new buffer, or `nullptr` when the size overflows or the allocator fails, in which case the old buffer is unchanged.
*/
[[nodiscard]] static void *soml__grow(const soml_allocator *allocator, void *buffer, size_t *capacity, size_t element_size, size_t needed) {
	if (needed <= *capacity) {
		return buffer;
	}

	size_t new_capacity;
	size_t new_size;

	if (!soml__next_capacity(*capacity, needed, 16, element_size, &new_capacity, &new_size)) {
		return nullptr;
	}

	size_t old_size = *capacity * element_size;
	void *grown = allocator->reallocate(allocator->context, buffer, old_size, new_size);

	if (grown != nullptr) {
		*capacity = new_capacity;
	}

	return grown;
}

static void soml__release(const soml_allocator *allocator, void *buffer, size_t size) {
	if (buffer != nullptr) {
		allocator->reallocate(allocator->context, buffer, size, 0);
	}
}

[[nodiscard]] static void *soml__allocate_scratch(const soml_allocator *allocator, size_t count, size_t element_size) {
	size_t size;

	if (count == 0 || ckd_mul(&size, count, element_size)) {
		return nullptr;
	}

	return allocator->reallocate(allocator->context, nullptr, 0, size);
}

static void soml__release_scratch(const soml_allocator *allocator, void *buffer, size_t count, size_t element_size) {
	soml__release(allocator, buffer, count * element_size);
}

/*
Bump allocation from the document's chunks, aligned to `alignment`, a power of two. Nothing is freed until the document is.
*/
[[nodiscard]] static void *soml__allocate_aligned(soml_document *document, size_t size, size_t alignment) {
	if (size == 0) {
		size = 1;
	}

	size_t reserved_size = size;
	size_t prefix_size = 0;

	#ifdef SOML__HAS_ADDRESS_SANITIZER
	// ASan tracks eight-byte granules. Leave a leading redzone, including before the first allocation in a chunk, and a trailing redzone that a later allocation cannot expose.
	alignment = alignment < 8 ? 8 : alignment;
	prefix_size = alignment;

	if (ckd_add(&reserved_size, size, prefix_size) || ckd_add(&reserved_size, reserved_size, 8)) {
		return nullptr;
	}
	#endif

	soml__chunk *chunk = document->chunks;
	size_t start = 0;

	// The data of a chunk is aligned for any type, so an offset that is a multiple of the alignment is too.
	bool has_room = chunk != nullptr && !ckd_add(&start, document->chunk_used, alignment - 1);
	start &= ~(alignment - 1);
	has_room = has_room && start <= chunk->size && reserved_size <= chunk->size - start;

	if (has_room) {
		document->chunk_used = start + reserved_size;
		unsigned char *data = (unsigned char *)chunk->data + start + prefix_size;
		SOML__UNPOISON(data, size);
		return data;
	}

	// An allocation larger than a chunk gets a chunk of its own, linked after the current one, so the current one keeps its free space.
	bool is_large = reserved_size > document->next_chunk_size;
	size_t data_size = is_large ? reserved_size : document->next_chunk_size;
	size_t total;

	if (ckd_add(&total, data_size, sizeof(soml__chunk))) {
		return nullptr;
	}

	soml__chunk *new_chunk = document->allocator.reallocate(document->allocator.context, nullptr, 0, total);

	if (new_chunk == nullptr) {
		return nullptr;
	}

	new_chunk->size = data_size;
	unsigned char *data = (unsigned char *)new_chunk->data + prefix_size;
	SOML__POISON(new_chunk->data, data_size);
	SOML__UNPOISON(data, size);

	if (is_large && chunk != nullptr) {
		new_chunk->next = chunk->next;
		chunk->next = new_chunk;
		return data;
	}

	new_chunk->next = chunk;
	document->chunks = new_chunk;
	document->chunk_used = reserved_size;

	if (document->next_chunk_size < soml__maximum_chunk_size) {
		document->next_chunk_size *= 2;
	}

	return data;
}

// The arena holds values, members, sorted indexes, and pointers, which need no more alignment than a value. Using that, rather than `max_align_t`, saves memory where `max_align_t` is 16 bytes.
static_assert(alignof(soml__member) <= alignof(soml_value) && alignof(size_t) <= alignof(soml_value) && alignof(soml_value *) <= alignof(soml_value));

/*
Memory for a value, or an array of values, members, indexes, or pointers.
*/
[[nodiscard]] static void *soml__allocate(soml_document *document, size_t size) {
	return soml__allocate_aligned(document, size, alignof(soml_value));
}

/*
Memory for bytes, such as a string, which need no alignment.
*/
[[nodiscard]] static unsigned char *soml__allocate_bytes(soml_document *document, size_t size) {
	return soml__allocate_aligned(document, size, 1);
}

/*
An array of `count` elements in the document, with the size checked for overflow.
*/
[[nodiscard]] static void *soml__allocate_array(soml_document *document, size_t count, size_t element_size) {
	size_t size;

	if (ckd_mul(&size, count, element_size)) {
		return nullptr;
	}

	return soml__allocate(document, size);
}

[[nodiscard]] static soml_document *soml__create_document(const soml_allocator *allocator) {
	auto resolved = soml__resolve_allocator(allocator);
	soml_document *document = resolved.reallocate(resolved.context, nullptr, 0, sizeof(*document));

	if (document == nullptr) {
		return nullptr;
	}

	*document = (soml_document){
		.allocator = resolved,
		.next_chunk_size = soml__minimum_chunk_size,
	};

	return document;
}

SOMLDEF void soml_document_free(soml_document *document) {
	if (document == nullptr) {
		return;
	}

	soml__chunk *chunk = document->chunks;

	while (chunk != nullptr) {
		soml__chunk *next = chunk->next;
		// A custom allocator may reuse the memory, so it gets it back addressable.
		SOML__UNPOISON(chunk->data, chunk->size);
		soml__release(&document->allocator, chunk, sizeof(*chunk) + chunk->size);
		chunk = next;
	}

	auto allocator = document->allocator;
	soml__release(&allocator, document, sizeof(*document));
}

/*
A new value in the document, a copy of `contents` that belongs to the document.
*/
[[nodiscard]] static soml_value *soml__new_value(soml_document *document, soml_value contents) {
	soml_value *value = soml__allocate(document, sizeof(*value));

	if (value != nullptr) {
		*value = contents;
		value->document = document;
	}

	return value;
}

/*
A copy of `length` bytes in the document, followed by a NUL.
*/
[[nodiscard]] static char *soml__copy_bytes(soml_document *document, const char *data, size_t length) {
	size_t size;

	if (ckd_add(&size, length, 1)) {
		return nullptr;
	}

	char *copy = (char *)soml__allocate_bytes(document, size);

	if (copy == nullptr) {
		return nullptr;
	}

	// `data` can be `nullptr` when `length` is 0, which the builder allows, and `memcpy` must not get `nullptr`, even for 0 bytes.
	if (length > 0) {
		memcpy(copy, data, length);
	}

	copy[length] = '\0';
	return copy;
}

// MARK: Characters

[[nodiscard]] static bool soml__is_digit(int character) {
	return character >= '0' && character <= '9';
}

[[nodiscard]] static bool soml__is_letter(int character) {
	return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
}

[[nodiscard]] static bool soml__equals_ignoring_case(const unsigned char *text, size_t length, soml_string word) {
	size_t word_length = word.length;

	if (length != word_length) {
		return false;
	}

	for (size_t index = 0; index < length; index++) {
		if ((text[index] | 0x20) != (unsigned char)word.data[index]) {
			return false;
		}
	}

	return true;
}

[[nodiscard]] static bool soml__is_bare_key_character(int character) {
	return soml__is_letter(character) || soml__is_digit(character) || character == '_' || character == '-';
}

[[nodiscard]] static bool soml__is_uppercase_hex_digit(int character) {
	return soml__is_digit(character) || (character >= 'A' && character <= 'F');
}

/*
The value of a lowercase hexadecimal digit, as used in `\u{…}`, or -1.
*/
[[nodiscard]] static int soml__lowercase_hex_value(int character) {
	if (soml__is_digit(character)) {
		return character - '0';
	}

	return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
}

/*
The radix that a prefix letter names, as in `0x`, `0o`, and `0b`, or 0.
*/
[[nodiscard]] static int soml__radix_of(int letter) {
	switch (letter) {
		case 'x': {
			return 16;
		}

		case 'o': {
			return 8;
		}

		case 'b': {
			return 2;
		}

		default: {
			return 0;
		}
	}
}

/*
The index of the first byte in text[index, end) that is not a space or a tab, or `end`.
*/
[[nodiscard]] static size_t soml__skip_blanks(const unsigned char *text, size_t index, size_t end) {
	while (index < end && (text[index] == ' ' || text[index] == '\t')) {
		index++;
	}

	return index;
}

/*
A high bit for each byte of `word` equal to `byte`, independent of byte order. Setting every high bit before subtraction prevents a borrow between bytes.
*/
[[nodiscard]] static uint64_t soml__equal_byte_mask(uint64_t word, unsigned char byte) {
	uint64_t different = word ^ soml__byte_ones * byte;
	return ~((different | soml__byte_high_bits) - soml__byte_ones) & ~different & soml__byte_high_bits;
}

/*
Whether every byte is an ASCII bare-key character. Setting each high bit before subtraction keeps the range checks independent across bytes and byte orders.
*/
[[nodiscard]] static bool soml__is_bare_key_word(uint64_t word) {
	if ((word & soml__byte_high_bits) != 0) {
		return false;
	}

	uint64_t lower = word | soml__byte_ones * 0x20;
	uint64_t letters = ((lower | soml__byte_high_bits) - soml__byte_ones * 'a') & ((soml__byte_ones * 'z' | soml__byte_high_bits) - lower);
	uint64_t digits = ((word | soml__byte_high_bits) - soml__byte_ones * '0') & ((soml__byte_ones * '9' | soml__byte_high_bits) - word);
	uint64_t allowed = letters | digits | soml__equal_byte_mask(word, '_') | soml__equal_byte_mask(word, '-');
	return (allowed & soml__byte_high_bits) == soml__byte_high_bits;
}

/*
The index after a run of bare-key characters from `index`.
*/
[[nodiscard]] static size_t soml__skip_bare_key(const unsigned char *text, size_t length, size_t index) {
	if (index >= length) {
		return index;
	}

	while (length - index >= sizeof(uint64_t)) {
		uint64_t word;
		memcpy(&word, text + index, sizeof(word));

		if (!soml__is_bare_key_word(word)) {
			break;
		}

		index += sizeof(word);
	}

	while (index < length && soml__is_bare_key_character(text[index])) {
		index++;
	}

	return index;
}

/*
The index after a run of digits and underscores from `index`, in any order, for describing a number that does not follow the grammar.
*/
[[nodiscard]] static size_t soml__skip_digits_and_underscores(const unsigned char *text, size_t length, size_t index) {
	while (index < length && (soml__is_digit(text[index]) || text[index] == '_')) {
		index++;
	}

	return index;
}

[[nodiscard]] static bool soml__is_radix_digit(int character, int radix) {
	switch (radix) {
		case 16: {
			return soml__is_uppercase_hex_digit(character);
		}

		case 8: {
			return character >= '0' && character <= '7';
		}

		case 2: {
			return character == '0' || character == '1';
		}

		default: {
			return soml__is_digit(character);
		}
	}
}

/*
What may directly follow a scalar value: whitespace, a separator, a closing bracket, or the start of a comment.
*/
[[nodiscard]] static bool soml__is_value_end(int character) {
	switch (character) {
		case ' ':
		case '\t':
		case '\n':
		case ',':
		case ']':
		case '}':
		case '#':
		case '/': {
			return true;
		}

		default: {
			return false;
		}
	}
}

/*
A number, an instant, or a duration is read as one maximal run of these characters, and then checked as a whole.
*/
[[nodiscard]] static bool soml__is_number_character(int character) {
	return soml__is_letter(character) || soml__is_digit(character) || character == '_' || character == '.' || character == ':' || character == '+' || character == '-';
}

/*
Whether a C0 control character other than tab and line feed, or DEL, which may not appear raw anywhere.
*/
[[nodiscard]] static bool soml__is_control(int character) {
	return (character < 0x20 && character != '\t' && character != '\n') || character == 0x7F;
}

/*
The length of the valid UTF-8 sequence at the start of `bytes`, or 0 when it is not valid. Overlong forms, surrogates, and values above U+10FFFF are not valid. When it is not valid only because the text ends too soon, `is_truncated` is set.
*/
[[nodiscard]] static size_t soml__utf8_sequence_length(const unsigned char *bytes, size_t available, bool *is_truncated) {
	if (available == 0) {
		if (is_truncated != nullptr) {
			*is_truncated = true;
		}

		return 0;
	}

	unsigned char first = bytes[0];

	if (first < 0x80) {
		return 1;
	}

	// The length from the first byte, and the range of the second byte, which rules out overlong forms, surrogates, and values above U+10FFFF.
	size_t length;
	unsigned char low = 0x80;
	unsigned char high = 0xBF;

	if (first >= 0xC2 && first <= 0xDF) {
		length = 2;
	} else if (first >= 0xE0 && first <= 0xEF) {
		length = 3;

		if (first == 0xE0) {
			low = 0xA0;
		} else if (first == 0xED) {
			high = 0x9F;
		}
	} else if (first >= 0xF0 && first <= 0xF4) {
		length = 4;

		if (first == 0xF0) {
			low = 0x90;
		} else if (first == 0xF4) {
			high = 0x8F;
		}
	} else {
		return 0;
	}

	size_t present = available < length ? available : length;

	for (size_t index = 1; index < present; index++) {
		if (bytes[index] < low || bytes[index] > high) {
			return 0;
		}

		low = 0x80;
		high = 0xBF;
	}

	if (available < length) {
		if (is_truncated != nullptr) {
			*is_truncated = true;
		}

		return 0;
	}

	return length;
}

[[nodiscard]] static size_t soml__encode_utf8(uint32_t scalar, unsigned char *output) {
	if (scalar < 0x80) {
		output[0] = (unsigned char)scalar;
		return 1;
	}

	if (scalar < 0x800) {
		output[0] = (unsigned char)(0xC0 | (scalar >> 6));
		output[1] = (unsigned char)(0x80 | (scalar & 0x3F));
		return 2;
	}

	if (scalar < 0x10000) {
		output[0] = (unsigned char)(0xE0 | (scalar >> 12));
		output[1] = (unsigned char)(0x80 | ((scalar >> 6) & 0x3F));
		output[2] = (unsigned char)(0x80 | (scalar & 0x3F));
		return 3;
	}

	output[0] = (unsigned char)(0xF0 | (scalar >> 18));
	output[1] = (unsigned char)(0x80 | ((scalar >> 12) & 0x3F));
	output[2] = (unsigned char)(0x80 | ((scalar >> 6) & 0x3F));
	output[3] = (unsigned char)(0x80 | (scalar & 0x3F));
	return 4;
}

/*
The scalar value of a valid UTF-8 sequence of `length` bytes.
*/
[[nodiscard]] static uint32_t soml__decode_utf8(const unsigned char *bytes, size_t length) {
	uint32_t scalar = length == 1 ? bytes[0] : bytes[0] & (0x7Fu >> length);

	for (size_t index = 1; index < length; index++) {
		scalar = (scalar << 6) | (bytes[index] & 0x3Fu);
	}

	return scalar;
}

/*
Whether a scalar value of U+00A0 or above is whitespace in Unicode (the White_Space property).
*/
[[nodiscard]] static bool soml__is_unicode_space(uint32_t scalar) {
	return scalar == 0xA0 || scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200A) || scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
}

[[nodiscard]] static int soml__compare_strings(soml_string left, soml_string right) {
	size_t length = left.length < right.length ? left.length : right.length;
	// A key to look up can be `nullptr` with a length of 0, and `memcmp` must not get `nullptr`, even for 0 bytes.
	int result = length == 0 ? 0 : memcmp(left.data, right.data, length);

	if (result != 0) {
		return result;
	}

	return (left.length > right.length) - (left.length < right.length);
}

/*
A stable bottom-up merge sort of `count` indexes, ordered by `compare`. `temporary` has room for `count` indexes.
*/
static void soml__merge_sort(size_t *items, size_t *temporary, size_t count, int (*compare)(const void *context, size_t left, size_t right), const void *context) {
	size_t *source = items;
	size_t *destination = temporary;

	// Past half of `count`, the width goes to `count`, which ends the loop, rather than doubling, which could overflow.
	for (size_t width = 1; width < count; width = width > count / 2 ? count : width * 2) {
		size_t left = 0;

		while (left < count) {
			size_t middle = left + (width < count - left ? width : count - left);
			size_t right = middle + (width < count - middle ? width : count - middle);
			if (middle == right || compare(context, source[middle - 1], source[middle]) <= 0) {
				memcpy(destination + left, source + left, (right - left) * sizeof(*source));
				left = right;
				continue;
			}

			size_t first = left;
			size_t second = middle;
			size_t output = left;

			while (first < middle && second < right) {
				if (compare(context, source[second], source[first]) < 0) {
					destination[output++] = source[second++];
				} else {
					destination[output++] = source[first++];
				}
			}

			if (first < middle) {
				memcpy(destination + output, source + first, (middle - first) * sizeof(*source));
			} else if (second < right) {
				memcpy(destination + output, source + second, (right - second) * sizeof(*source));
			}

			left = right;
		}

		size_t *swap = source;
		source = destination;
		destination = swap;
	}

	if (source != items) {
		memcpy(items, source, count * sizeof(*items));
	}
}

// MARK: High-precision decimal

/*
A decimal number with up to 800 significant digits, used to convert between decimal text and binary64 exactly. The algorithm is the one in Go's strconv/decimal.go and strconv/ftoa.go (BSD-3-Clause, notice at the end of this file).

The value is 0.digits × 10^point. The digits are values 0 to 9, with no trailing zeros. `truncated` records that nonzero digits past the capacity were dropped, so that a value that looks exactly halfway is known to be above it.
*/
typedef struct soml__decimal {
	unsigned char digits[soml__decimal_capacity];
	int count;
	int point;
	bool truncated;
} soml__decimal;

typedef unsigned _BitInt(128) soml__wide_unsigned;

// The largest shift at once, so that two decimal digits can be processed together: the running value stays below 100 × 2^57, which fits in 64 bits.
constexpr unsigned soml__maximum_shift = 57;

// The layout of binary64: 52 stored mantissa bits, an 11-bit exponent, and its bias.
constexpr int soml__mantissa_bits = 52;
constexpr int soml__exponent_bits = 11;
constexpr int soml__exponent_bias = -1023;
static_assert(FLT_RADIX == 2 && DBL_MANT_DIG == soml__mantissa_bits + 1 && DBL_MAX_EXP == -soml__exponent_bias + 1 && sizeof(double) == sizeof(uint64_t), "SOML needs `double` to be IEEE 754 binary64");

static void soml__decimal_trim(soml__decimal *decimal) {
	while (decimal->count > 0 && decimal->digits[decimal->count - 1] == 0) {
		decimal->count--;
	}

	if (decimal->count == 0) {
		decimal->point = 0;
	}
}

static void soml__decimal_assign(soml__decimal *decimal, uint64_t value) {
	unsigned char reversed[20];
	int count = 0;

	while (value > 0) {
		reversed[count++] = (unsigned char)(value % 10);
		value /= 10;
	}

	decimal->count = count;
	decimal->truncated = false;

	for (int index = 0; index < count; index++) {
		decimal->digits[index] = reversed[count - 1 - index];
	}

	decimal->point = count;
	soml__decimal_trim(decimal);
}

/*
Divides by 2^shift.
*/
static void soml__decimal_right_shift(soml__decimal *decimal, unsigned shift) {
	int read = 0;
	int write = 0;
	uint64_t value = 0;

	for (; (value >> shift) == 0; read++) {
		if (read >= decimal->count) {
			if (value == 0) {
				decimal->count = 0;
				return;
			}

			while ((value >> shift) == 0) {
				value *= 10;
				read++;
			}

			break;
		}

		value = value * 10 + decimal->digits[read];
	}

	decimal->point -= read - 1;
	uint64_t mask = ((uint64_t)1 << shift) - 1;

	for (; read + 1 < decimal->count; read += 2) {
		value = value * 10 + decimal->digits[read];
		unsigned pair = (unsigned)(value >> shift);
		value &= mask;
		decimal->digits[write++] = (unsigned char)(pair / 10);
		decimal->digits[write++] = (unsigned char)(pair % 10);
		value = value * 10 + decimal->digits[read + 1];
	}

	for (; read < decimal->count; read++) {
		uint64_t digit = value >> shift;
		value &= mask;
		decimal->digits[write++] = (unsigned char)digit;
		value = value * 10 + decimal->digits[read];
	}

	while (value > 0) {
		uint64_t digit = value >> shift;
		value &= mask;

		if (write < soml__decimal_capacity) {
			decimal->digits[write++] = (unsigned char)digit;
		} else if (digit > 0) {
			decimal->truncated = true;
		}

		value *= 10;
	}

	decimal->count = write;
	soml__decimal_trim(decimal);
}

/*
Multiplies by 2^shift. The digits are written from the right into a buffer with room for the new leading digits, then copied back.
*/
static void soml__decimal_left_shift(soml__decimal *decimal, unsigned shift) {
	// 2^57 has 18 digits, so the product has at most 18 more digits.
	constexpr int extra = 18;
	unsigned char buffer[soml__decimal_capacity + extra];
	int write = decimal->count + extra;
	uint64_t value = 0;

	int read = decimal->count - 1;

	for (; read >= 1; read -= 2) {
		value += (uint64_t)(decimal->digits[read - 1] * 10 + decimal->digits[read]) << shift;
		unsigned pair = (unsigned)(value % 100);
		buffer[--write] = (unsigned char)(pair % 10);
		buffer[--write] = (unsigned char)(pair / 10);
		value /= 100;
	}

	if (read == 0) {
		value += (uint64_t)decimal->digits[read] << shift;
		buffer[--write] = (unsigned char)(value % 10);
		value /= 10;
	}

	while (value > 0) {
		buffer[--write] = (unsigned char)(value % 10);
		value /= 10;
	}

	int new_count = decimal->count + extra - write;
	decimal->point += new_count - decimal->count;

	if (new_count > soml__decimal_capacity) {
		for (int index = soml__decimal_capacity; index < new_count; index++) {
			if (buffer[write + index] != 0) {
				decimal->truncated = true;
				break;
			}
		}

		new_count = soml__decimal_capacity;
	}

	memcpy(decimal->digits, buffer + write, (size_t)new_count);
	decimal->count = new_count;
	soml__decimal_trim(decimal);
}

/*
Multiplies by 2^shift, or divides by 2^-shift.
*/
static void soml__decimal_shift(soml__decimal *decimal, int shift) {
	if (decimal->count == 0) {
		return;
	}

	while (shift > (int)soml__maximum_shift) {
		soml__decimal_left_shift(decimal, soml__maximum_shift);
		shift -= (int)soml__maximum_shift;
	}

	while (shift < -(int)soml__maximum_shift) {
		soml__decimal_right_shift(decimal, soml__maximum_shift);
		shift += (int)soml__maximum_shift;
	}

	if (shift > 0) {
		soml__decimal_left_shift(decimal, (unsigned)shift);
	} else if (shift < 0) {
		soml__decimal_right_shift(decimal, (unsigned)-shift);
	}
}

/*
Whether rounding to `count` digits rounds up: to nearest, and to an even digit when exactly halfway.
*/
[[nodiscard]] static bool soml__decimal_should_round_up(const soml__decimal *decimal, int count) {
	if (count < 0 || count >= decimal->count) {
		return false;
	}

	if (decimal->digits[count] == 5 && count + 1 == decimal->count) {
		// Dropped digits make it more than halfway.
		if (decimal->truncated) {
			return true;
		}

		return count > 0 && (decimal->digits[count - 1] & 1) != 0;
	}

	return decimal->digits[count] >= 5;
}

static void soml__decimal_round_down(soml__decimal *decimal, int count) {
	if (count < 0 || count >= decimal->count) {
		return;
	}

	decimal->count = count;
	soml__decimal_trim(decimal);
}

static void soml__decimal_round_up(soml__decimal *decimal, int count) {
	if (count < 0 || count >= decimal->count) {
		return;
	}

	for (int index = count - 1; index >= 0; index--) {
		if (decimal->digits[index] < 9) {
			decimal->digits[index]++;
			decimal->count = index + 1;
			return;
		}
	}

	// Every digit was a 9.
	decimal->digits[0] = 1;
	decimal->count = 1;
	decimal->point++;
}

static void soml__decimal_round(soml__decimal *decimal, int count) {
	if (soml__decimal_should_round_up(decimal, count)) {
		soml__decimal_round_up(decimal, count);
	} else {
		soml__decimal_round_down(decimal, count);
	}
}

/*
The integer part, rounded to nearest with ties to even. Only called with a value below 2^64.
*/
[[nodiscard]] static uint64_t soml__decimal_rounded_integer(const soml__decimal *decimal) {
	if (decimal->point > 20) {
		return UINT64_MAX;
	}

	uint64_t value = 0;
	int index = 0;

	for (; index < decimal->point && index < decimal->count; index++) {
		value = value * 10 + decimal->digits[index];
	}

	for (; index < decimal->point; index++) {
		value *= 10;
	}

	if (soml__decimal_should_round_up(decimal, decimal->point)) {
		value++;
	}

	return value;
}

/*
The shift that moves a decimal whose point is `distance` digits from 0 toward [0.5, 1) without stepping past it. Near it, the shift is the power of two at or below 10^distance. Far from it, a step is the largest shift, which cannot step past it: at a decimal point of 19 or more the value is at least 10^18, more than 2^57, so after the step it is still at least 0.5, and at -19 or less it is below 10^-19, so after the step it is still below 0.5.
*/
[[nodiscard]] static int soml__scale_shift(int distance) {
	static constexpr int powers[] = {1, 3, 6, 9, 13, 16, 19, 23, 26};

	if (distance >= 19) {
		return (int)soml__maximum_shift;
	}

	return distance < (int)(sizeof(powers) / sizeof(powers[0])) ? powers[distance] : 27;
}

/*
The binary64 value nearest to the decimal, with ties to even. Returns `false` when it is too large to be finite. The decimal is changed.
*/
[[nodiscard]] static bool soml__decimal_to_double(soml__decimal *decimal, double *output) {
	uint64_t mantissa = 0;
	int exponent = 0;

	// Below 10^-330, the value is far below half the smallest subnormal, about 2.5 × 10^-324, so it is zero. Above 10^310, it is far above the largest finite value, about 1.8 × 10^308.
	if (decimal->count == 0 || decimal->point < -330) {
		exponent = soml__exponent_bias;
	} else {
		if (decimal->point > 310) {
			return false;
		}

		// Scale by powers of two until the value is in [0.5, 1).
		while (decimal->point > 0) {
			int shift = soml__scale_shift(decimal->point);
			soml__decimal_shift(decimal, -shift);
			exponent += shift;
		}

		while (decimal->point < 0 || (decimal->point == 0 && decimal->digits[0] < 5)) {
			int shift = soml__scale_shift(-decimal->point);
			soml__decimal_shift(decimal, shift);
			exponent -= shift;
		}

		// The range is [0.5, 1), but binary64 has [1, 2).
		exponent--;

		// Below the smallest normal exponent, the value is subnormal.
		if (exponent < soml__exponent_bias + 1) {
			int shift = soml__exponent_bias + 1 - exponent;
			soml__decimal_shift(decimal, -shift);
			exponent += shift;
		}

		if (exponent - soml__exponent_bias >= (1 << soml__exponent_bits) - 1) {
			return false;
		}

		soml__decimal_shift(decimal, 1 + soml__mantissa_bits);
		mantissa = soml__decimal_rounded_integer(decimal);

		// Rounding can carry into a new bit.
		if (mantissa == (uint64_t)2 << soml__mantissa_bits) {
			mantissa >>= 1;
			exponent++;

			if (exponent - soml__exponent_bias >= (1 << soml__exponent_bits) - 1) {
				return false;
			}
		}

		if ((mantissa & ((uint64_t)1 << soml__mantissa_bits)) == 0) {
			exponent = soml__exponent_bias;
		}
	}

	uint64_t bits = mantissa & (((uint64_t)1 << soml__mantissa_bits) - 1);
	bits |= (uint64_t)((exponent - soml__exponent_bias) & ((1 << soml__exponent_bits) - 1)) << soml__mantissa_bits;
	double value;
	memcpy(&value, &bits, sizeof(value));
	*output = value;
	return true;
}

/*
Sets the decimal to the shortest digits that read back as the finite, positive binary64 value with these bits, choosing the closest, and then an even last digit, among the shortest.
*/
static void soml__decimal_shortest(soml__decimal *decimal, uint64_t bits) {
	int exponent = (int)((bits >> soml__mantissa_bits) & 0x7FF);
	uint64_t mantissa = bits & (((uint64_t)1 << soml__mantissa_bits) - 1);

	if (exponent == 0) {
		exponent++;
	} else {
		mantissa |= (uint64_t)1 << soml__mantissa_bits;
	}

	exponent += soml__exponent_bias;

	soml__decimal_assign(decimal, mantissa);
	soml__decimal_shift(decimal, exponent - soml__mantissa_bits);

	if (mantissa == 0) {
		decimal->count = 0;
		return;
	}

	// The digits are already shortest when the next shorter number is farther away than the gap to a neighbor.
	int minimum_exponent = soml__exponent_bias + 1;

	// 332 / 100 is just below log2(10), so this compares 10^(point - count) with 2^(exponent - 52), the gap, in integers.
	if (exponent > minimum_exponent && 332 * (decimal->point - decimal->count) >= 100 * (exponent - soml__mantissa_bits)) {
		return;
	}

	// Halfway to the next value above.
	soml__decimal upper;
	soml__decimal_assign(&upper, mantissa * 2 + 1);
	soml__decimal_shift(&upper, exponent - soml__mantissa_bits - 1);

	// Halfway to the next value below, which has a smaller gap when the mantissa is a power of two.
	uint64_t lower_mantissa;
	int lower_exponent;

	if (mantissa > (uint64_t)1 << soml__mantissa_bits || exponent == minimum_exponent) {
		lower_mantissa = mantissa - 1;
		lower_exponent = exponent;
	} else {
		lower_mantissa = mantissa * 2 - 1;
		lower_exponent = exponent - 1;
	}

	soml__decimal lower;
	soml__decimal_assign(&lower, lower_mantissa * 2 + 1);
	soml__decimal_shift(&lower, lower_exponent - soml__mantissa_bits - 1);

	// A bound reads back as this value only when the mantissa is even, because of ties to even.
	bool inclusive = (mantissa & 1) == 0;

	// 0: the digits of the value and the upper bound are equal so far. 1: they differ by one, followed only by 9s in the value and 0s in the bound. 2: rounding up stays below the upper bound.
	int upper_delta = 0;

	for (int upper_index = 0;; upper_index++) {
		int middle_index = upper_index - upper.point + decimal->point;

		if (middle_index >= decimal->count) {
			break;
		}

		int lower_index = upper_index - upper.point + lower.point;
		int lower_digit = lower_index >= 0 && lower_index < lower.count ? lower.digits[lower_index] : 0;
		int middle_digit = middle_index >= 0 ? decimal->digits[middle_index] : 0;
		int upper_digit = upper_index < upper.count ? upper.digits[upper_index] : 0;

		bool can_round_down = lower_digit != middle_digit || (inclusive && lower_index + 1 == lower.count);

		if (upper_delta == 0 && middle_digit + 1 < upper_digit) {
			upper_delta = 2;
		} else if (upper_delta == 0 && middle_digit != upper_digit) {
			upper_delta = 1;
		} else if (upper_delta == 1 && (middle_digit != 9 || upper_digit != 0)) {
			upper_delta = 2;
		}

		bool can_round_up = upper_delta > 0 && (inclusive || upper_delta > 1 || upper_index + 1 < upper.count);

		if (can_round_down && can_round_up) {
			soml__decimal_round(decimal, middle_index + 1);
			return;
		}

		if (can_round_down) {
			soml__decimal_round_down(decimal, middle_index + 1);
			return;
		}

		if (can_round_up) {
			soml__decimal_round_up(decimal, middle_index + 1);
			return;
		}
	}
}

/*
Finds the shortest decimal of a normal binary64 value whose denominator fits in 65 bits. Integer interval comparisons make the result independent of floating-point rounding modes. Other magnitudes use the high-precision decimal path.
*/
[[nodiscard]] static bool soml__decimal_shortest_small(soml__decimal *decimal, uint64_t bits) {
	int exponent = (int)(bits >> soml__mantissa_bits) + soml__exponent_bias - soml__mantissa_bits;

	if (exponent < -63 || exponent >= 0) {
		return false;
	}

	uint64_t mantissa = (bits & (((uint64_t)1 << soml__mantissa_bits) - 1)) | ((uint64_t)1 << soml__mantissa_bits);
	unsigned shift = (unsigned)(2 - exponent);
	soml__wide_unsigned denominator = (soml__wide_unsigned)1 << shift;
	soml__wide_unsigned middle = (soml__wide_unsigned)mantissa * 4;
	soml__wide_unsigned lower = middle - (mantissa == (uint64_t)1 << soml__mantissa_bits ? 1 : 2);
	soml__wide_unsigned upper = middle + 2;
	soml__wide_unsigned scale = 1;
	bool inclusive = (mantissa & 1) == 0;

	for (int precision = 0; precision <= 19; precision++) {
		soml__wide_unsigned numerator = middle * scale;
		soml__wide_unsigned rounded = numerator >> shift;
		soml__wide_unsigned remainder = numerator & (denominator - 1);

		if (remainder > denominator / 2 || (remainder == denominator / 2 && (rounded & 1) != 0)) {
			rounded++;
		}

		soml__wide_unsigned candidate = rounded * denominator;
		soml__wide_unsigned minimum = lower * scale;
		soml__wide_unsigned maximum = upper * scale;

		if ((candidate > minimum || (inclusive && candidate == minimum)) && (candidate < maximum || (inclusive && candidate == maximum))) {
			if (rounded > UINT64_MAX) {
				return false;
			}

			soml__decimal_assign(decimal, (uint64_t)rounded);
			decimal->point -= precision;
			return true;
		}

		scale *= 10;
	}

	return false;
}

[[nodiscard]] static size_t soml__format_unsigned(uint64_t value, char *output) {
	char reversed[20];
	size_t count = 0;

	do {
		reversed[count++] = (char)('0' + value % 10);
		value /= 10;
	} while (value > 0);

	for (size_t index = 0; index < count; index++) {
		output[index] = reversed[count - 1 - index];
	}

	return count;
}

/*
Writes a float in canonical form (rule 11) and returns the length. The buffer holds at least 32 bytes.
*/
[[nodiscard]] SOML__NOINLINE static size_t soml__format_float(double value, char *output) {
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	bool is_negative = (bits >> 63) != 0;
	uint64_t magnitude = bits & ~soml__float_sign_bit;
	size_t length = 0;

	if (magnitude == 0) {
		memcpy(output, "0.0", 3);
		return 3;
	}

	if (is_negative) {
		output[length++] = '-';
	}

	// NaN never gets here, because `soml__write_value` refuses it first.
	if (magnitude == soml__float_infinity_bits) {
		memcpy(output + length, "infinity", 8);
		return length + 8;
	}

	soml__decimal decimal;
	double absolute = __builtin_fabs(value);

	// A whole number below 2^53 is exact, and its neighbors are at most 1 away, so its own digits are the shortest that read back.
	// Check the range in bits: converting a large value to `uint64_t` is undefined, and a floating-point comparison can mistake a subnormal for zero when the thread flushes subnormals.
	if (magnitude >= 0x3FF0000000000000 && magnitude < 0x4340000000000000 && (double)(uint64_t)absolute == absolute) {
		soml__decimal_assign(&decimal, (uint64_t)absolute);
	} else if (!soml__decimal_shortest_small(&decimal, magnitude)) {
		soml__decimal_shortest(&decimal, magnitude);
	}

	int count = decimal.count;
	int point = decimal.point;

	// Steps 3 to 6 of canonical form rule 11, where k is `count` and n is `point`.
	if (count <= point && point <= 21) {
		for (int index = 0; index < count; index++) {
			output[length++] = (char)('0' + decimal.digits[index]);
		}

		memset(output + length, '0', (size_t)(point - count));
		length += (size_t)(point - count);

		output[length++] = '.';
		output[length++] = '0';
	} else if (0 < point && point < count) {
		for (int index = 0; index < count; index++) {
			if (index == point) {
				output[length++] = '.';
			}

			output[length++] = (char)('0' + decimal.digits[index]);
		}
	} else if (-6 < point && point <= 0) {
		output[length++] = '0';
		output[length++] = '.';

		memset(output + length, '0', (size_t)-point);
		length += (size_t)-point;

		for (int index = 0; index < count; index++) {
			output[length++] = (char)('0' + decimal.digits[index]);
		}
	} else {
		output[length++] = (char)('0' + decimal.digits[0]);

		if (count > 1) {
			output[length++] = '.';

			for (int index = 1; index < count; index++) {
				output[length++] = (char)('0' + decimal.digits[index]);
			}
		}

		output[length++] = 'e';
		int exponent = point - 1;

		if (exponent < 0) {
			output[length++] = '-';
		}

		length += soml__format_unsigned((uint64_t)(exponent < 0 ? -exponent : exponent), output + length);
	}

	return length;
}

// MARK: Dates

/*
Days since 1970-01-01 in the proleptic Gregorian calendar, by Howard Hinnant's algorithm.
*/
[[nodiscard]] static int64_t soml__days_from_civil(int64_t year, int64_t month, int64_t day) {
	year -= month <= 2;
	int64_t era = (year >= 0 ? year : year - 399) / 400;
	int64_t year_of_era = year - era * 400;
	int64_t day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
	int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	return era * 146097 + day_of_era - 719468;
}

static void soml__civil_from_days(int64_t days, int64_t *year, int64_t *month, int64_t *day) {
	days += 719468;
	int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	int64_t day_of_era = days - era * 146097;
	int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
	int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
	int64_t month_index = (5 * day_of_year + 2) / 153;
	*day = day_of_year - (153 * month_index + 2) / 5 + 1;
	*month = month_index < 10 ? month_index + 3 : month_index - 9;
	*year = year_of_era + era * 400 + (*month <= 2);
}

[[nodiscard]] static int64_t soml__days_in_month(int64_t year, int64_t month) {
	if (month == 2) {
		bool is_leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
		return is_leap_year ? 29 : 28;
	}

	return month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31;
}

// MARK: Parser

static const char soml__too_deep_message[] = "The document is nested more than 100 levels deep";

typedef struct soml__entry {
	soml_string key;
	soml_value *value;
	// Where the key starts, which places errors.
	size_t offset;
} soml__entry;

typedef struct soml__parser {
	const unsigned char *text;
	size_t length;
	size_t position;
	soml_document *document;
	// The line and column are found from the offset at the end.
	soml_error error;

	// Stacks shared by all open containers. A container's part starts where the stack was when it opened, and is removed when it closes.
	soml_value **items;
	size_t item_count;
	size_t item_capacity;
	soml__entry *entries;
	size_t entry_count;
	size_t entry_capacity;

	// Memory for building an object when it closes, kept from one object to the next. It also holds an escaped string or key while it is decoded. The uses never overlap, because an object's sort order is used before the parser reads anything more.
	unsigned char *scratch;
	size_t scratch_capacity;

	// Where the document's object or array starts, after the comments and whitespace before it.
	size_t document_start;
} soml__parser;

/*
Records the error and returns `false`. Every `soml__fail` function returns `false`, so that a caller can return its result as its own failure.
*/
static bool soml__fail_with_code(soml__parser *parser, soml_error_code code, size_t offset, const char *message) {
	parser->error = (soml_error){.code = code, .message = message, .offset = offset};
	return false;
}

static bool soml__fail(soml__parser *parser, size_t offset, const char *message) {
	return soml__fail_with_code(parser, SOML_ERROR_SYNTAX, offset, message);
}

static bool soml__fail_out_of_memory(soml__parser *parser) {
	return soml__fail_with_code(parser, SOML_ERROR_OUT_OF_MEMORY, parser->position, soml__out_of_memory_message);
}

/*
The byte at `offset` from the current position, or -1 past the end.
*/
[[nodiscard]] static int soml__peek(const soml__parser *parser, size_t offset) {
	size_t index;

	if (ckd_add(&index, parser->position, offset) || index >= parser->length) {
		return -1;
	}

	return parser->text[index];
}

[[nodiscard]] static int soml__byte_at(const soml__parser *parser, size_t index) {
	return index < parser->length ? parser->text[index] : -1;
}

[[nodiscard]] static bool soml__is_at_end(const soml__parser *parser) {
	return parser->position >= parser->length;
}

/*
The scalar value of the character at `offset`, which is in the text. The text is valid UTF-8, which was checked first.
*/
[[nodiscard]] static uint32_t soml__scalar_at(const soml__parser *parser, size_t offset) {
	const unsigned char *bytes = parser->text + offset;
	if (bytes[0] < 0x80) {
		return bytes[0];
	}

	return soml__decode_utf8(bytes, soml__utf8_sequence_length(bytes, parser->length - offset, nullptr));
}

/*
Whether a scalar value looks like whitespace but is not whitespace in SOML: a Unicode space, U+0085, U+200B, or U+FEFF.
*/
[[nodiscard]] static bool soml__is_lookalike_space(uint32_t scalar) {
	return scalar == 0x85 || scalar == 0x200B || scalar == 0xFEFF || soml__is_unicode_space(scalar);
}

/*
Whether the character at `offset` looks like whitespace but is not. An error that points at one says so, because the line looks right to the person reading it.
*/
[[nodiscard]] static bool soml__is_lookalike_space_at(const soml__parser *parser, size_t offset) {
	return offset < parser->length && parser->text[offset] >= 0x80 && soml__is_lookalike_space(soml__scalar_at(parser, offset));
}

#define SOML__SPACE_HINT ". The character there looks like whitespace, but only space, tab, and line feed are whitespace"

// The message, with the hint when the character at `offset` looks like whitespace. The message is a string literal.
#define SOML__WITH_SPACE_HINT(parser, offset, message) (soml__is_lookalike_space_at(parser, offset) ? message SOML__SPACE_HINT : message)

/*
Whether a `//` comment, as in JavaScript, starts at `offset`. An error that points at one says that a comment starts with `#`.
*/
[[nodiscard]] static bool soml__is_slash_comment_at(const soml__parser *parser, size_t offset) {
	return soml__byte_at(parser, offset) == '/' && soml__byte_at(parser, offset + 1) == '/';
}

#define SOML__SLASH_COMMENT_HINT ", but found “/”. A comment starts with “#”"

// The message, with the hint for a `//` comment or for a character that looks like whitespace at `offset`. The message is a string literal.
#define SOML__WITH_HINTS(parser, offset, message) (soml__is_slash_comment_at(parser, offset) ? message SOML__SLASH_COMMENT_HINT : SOML__WITH_SPACE_HINT(parser, offset, message))

/*
Whether a value ends at `offset`, so that a suggestion that ends there leaves nothing out. A `/` only starts a comment with a `*` after it, or a `//` comment of another language, which the error for what follows names, so in `km/h` the text goes on.
*/
[[nodiscard]] static bool soml__is_value_end_at(const soml__parser *parser, size_t offset) {
	int character = soml__byte_at(parser, offset);
	int next = soml__byte_at(parser, offset + 1);
	return character == -1 || (soml__is_value_end(character) && (character != '/' || next == '*' || next == '/'));
}

/*
The end of the text from `start` that was most likely meant as one unquoted string, such as `John Smith`: up to the end of the line, a comma, a closing bracket, or a comment after a space or a tab, without the spaces and tabs at its end. Like the reference implementation, it looks at 1,000 characters at most, counted here in bytes rather than in UTF-16 code units, which only changes the message for a longer text.
*/
[[nodiscard]] static size_t soml__unquoted_text_end(const soml__parser *parser, size_t start) {
	const unsigned char *text = parser->text;
	size_t limit = parser->length - start > 1000 ? start + 1000 : parser->length;
	size_t end = start;

	for (; end < limit; end++) {
		unsigned char character = text[end];
		bool is_comment = character == '#' || (character == '/' && soml__byte_at(parser, end + 1) == '*');

		if (character == '\n' || character == ',' || character == ']' || character == '}' || (is_comment && end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t'))) {
			break;
		}
	}

	while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
		end--;
	}

	return end;
}

/*
Finds `needle` from `start`, and returns its offset or `length`. The needle is the two bytes `first` and `second`.
*/
[[nodiscard]] static size_t soml__find_pair(const unsigned char *text, size_t length, size_t start, unsigned char first, unsigned char second) {
	while (start < length && length - start > 1) {
		const unsigned char *found = memchr(text + start, first, length - start - 1);

		if (found == nullptr) {
			break;
		}

		size_t index = (size_t)(found - text);

		if (text[index + 1] == second) {
			return index;
		}

		start = index + 1;
	}

	return length;
}

[[nodiscard]] static size_t soml__find_byte(const unsigned char *text, size_t length, size_t start, unsigned char byte) {
	if (start >= length) {
		return length;
	}

	const unsigned char *found = memchr(text + start, byte, length - start);
	return found == nullptr ? length : (size_t)(found - text);
}

/*
Whether eight ASCII bytes, one in each byte of `word`, hold a control character: a byte below a space other than tab and line feed, or DEL. Each byte is tested on its own, in any byte order: every byte has its high bit set before a subtraction, so none borrows from the next.
*/
[[nodiscard]] static bool soml__has_control_byte(uint64_t word) {
	uint64_t below_space = ~((word | soml__byte_high_bits) - soml__byte_ones * 0x20) & soml__byte_high_bits;
	uint64_t tabs = soml__equal_byte_mask(word, '\t');
	uint64_t line_feeds = soml__equal_byte_mask(word, '\n');
	uint64_t deletes = soml__equal_byte_mask(word, 0x7F);
	return ((below_space & ~tabs & ~line_feeds) | deletes) != 0;
}

/*
Characters that are errors wherever they appear are checked once, before parsing: invalid UTF-8 first, then a BOM, then the first raw control character, as in the reference implementation. This is the one pass over every byte, so eight ASCII bytes without a control character are skipped at once, and after the first control character, eight ASCII bytes.
*/
[[nodiscard]] static bool soml__check_characters(soml__parser *parser) {
	const unsigned char *text = parser->text;
	size_t length = parser->length;
	size_t first_control = length;
	size_t index = 0;

	while (index < length) {
		while (length - index >= 8) {
			uint64_t word;
			memcpy(&word, text + index, sizeof(word));

			if ((word & soml__byte_high_bits) != 0 || (first_control == length && soml__has_control_byte(word))) {
				break;
			}

			index += 8;
		}

		if (index >= length) {
			break;
		}

		unsigned char byte = text[index];

		if (byte < 0x80) {
			if (first_control == length && soml__is_control(byte)) {
				first_control = index;
			}

			index++;
			continue;
		}

		bool is_truncated = false;
		size_t sequence_length = soml__utf8_sequence_length(text + index, length - index, &is_truncated);

		if (sequence_length == 0) {
			return soml__fail(parser, index, is_truncated ? "Incomplete UTF-8 sequence at the end of the input" : "Invalid UTF-8");
		}

		index += sequence_length;
	}

	if (length >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) {
		return soml__fail(parser, 0, "A byte order mark (BOM) is not allowed");
	}

	if (first_control < length) {
		if (text[first_control] == '\r') {
			return soml__fail(parser, first_control, "A carriage return (U+000D) is not allowed anywhere. Use LF line endings");
		}

		return soml__fail(parser, first_control, "A raw control character is not allowed anywhere, including in strings and comments. In a string, write it as a \\u{…} escape inside \"...\"");
	}

	return true;
}

[[nodiscard]] static bool soml__skip_block_comment(soml__parser *parser) {
	size_t start = parser->position;
	// The search starts after the opening, so that its `*` does not also begin the closing `*/`.
	size_t index = start + 2;
	size_t nested = parser->length;

	while (index < parser->length) {
		size_t star = soml__find_byte(parser->text, parser->length, index, '*');

		if (star == parser->length) {
			break;
		}

		// The body may not contain `/*`. An opening that overlaps the closing `*/`, as in `/*/`, is not in the body.
		if (parser->length - star > 1 && parser->text[star + 1] == '/') {
			if (nested < parser->length) {
				return soml__fail(parser, nested, "Block comments cannot be nested, and their body may not contain “/*”");
			}

			parser->position = star + 2;
			return true;
		}

		if (nested == parser->length && star > start + 2 && parser->text[star - 1] == '/') {
			nested = star - 1;
		}

		index = star + 1;
	}

	return soml__fail(parser, start, "Unterminated block comment");
}

/*
Skips spaces, tabs, line breaks, and comments. `has_crossed_line_break` reports whether a line break outside a block comment was crossed.
*/
[[nodiscard]] static bool soml__skip_trivia(soml__parser *parser, bool *has_crossed_line_break) {
	bool crossed = false;

	for (;;) {
		int character = soml__peek(parser, 0);

		if (character == ' ' || character == '\t') {
			parser->position = soml__skip_blanks(parser->text, parser->position + 1, parser->length);
		} else if (character == '\n') {
			crossed = true;
			parser->position++;
		} else if (character == '#') {
			parser->position = soml__find_byte(parser->text, parser->length, parser->position, '\n');
		} else if (character == '/' && soml__peek(parser, 1) == '*') {
			if (!soml__skip_block_comment(parser)) {
				return false;
			}
		} else {
			break;
		}
	}

	if (has_crossed_line_break != nullptr) {
		*has_crossed_line_break = crossed;
	}

	return true;
}

// MARK: Strings

[[nodiscard]] static bool soml__is_hex_digit(int character) {
	return soml__is_uppercase_hex_digit(character) || soml__lowercase_hex_value(character) != -1;
}

/*
The value of a hexadecimal digit in either case.
*/
[[nodiscard]] static int soml__hex_digit_value(int character) {
	return soml__is_digit(character) ? character - '0' : (character | 0x20) - 'a' + 10;
}

/*
The message for the value of a `\u{…}` escape that names no character SOML can hold: a carriage return, a surrogate, or a value above U+10FFFF. `nullptr` for any other value.
*/
[[nodiscard]] static const char *soml__describe_bad_escape_value(uint64_t scalar) {
	if (scalar == 0x0D) {
		return "A carriage return (U+000D) cannot be represented, so \\u{d} is not allowed";
	}

	if (scalar >= 0xD800 && scalar <= 0xDFFF) {
		return "A Unicode escape cannot name a surrogate, which is not a Unicode scalar value";
	}

	return scalar > 0x10FFFF ? "A Unicode escape is above U+10FFFF, the largest Unicode scalar value" : nullptr;
}

/*
The value of four hexadecimal digits in either case from `offset`, as in JSON's `\uXXXX`, or -1 when four do not follow.
*/
[[nodiscard]] static int32_t soml__four_digit_escape_value(const soml__parser *parser, size_t offset) {
	int32_t value = 0;

	for (size_t index = offset; index < offset + 4; index++) {
		int character = soml__byte_at(parser, index);

		if (!soml__is_hex_digit(character)) {
			return -1;
		}

		value = value * 16 + soml__hex_digit_value(character);
	}

	return value;
}

#define SOML__FOUR_DIGIT_FORM "The four-digit \\uXXXX form is not an escape"

/*
A message for a `\\u` at `offset` that is not followed by `{`, one to six lowercase hexadecimal digits, and `}`, following the reference implementation's checks in order, except that an escape with uppercase digits whose value no escape can name gets the error for its value, as lowercase digits do. Like it, this looks at the 12 characters from the `u` only.
*/
[[nodiscard]] static const char *soml__describe_bad_unicode_escape(const soml__parser *parser, size_t offset) {
	// The form JSON uses: four hexadecimal digits in either case, without braces. JSON writes a character above U+FFFF as two of them, a surrogate pair, which is one `\u{…}` escape here. Here, a lone surrogate and a carriage return have no escape.
	int32_t code = soml__four_digit_escape_value(parser, offset + 1);

	if (code != -1) {
		int32_t low = soml__byte_at(parser, offset + 5) == '\\' && soml__byte_at(parser, offset + 6) == 'u' ? soml__four_digit_escape_value(parser, offset + 7) : -1;

		if (code >= 0xD800 && code <= 0xDBFF && low >= 0xDC00 && low <= 0xDFFF) {
			return SOML__FOUR_DIGIT_FORM ", and a surrogate pair is one character. Write it as one \\u{…} escape";
		}

		if (code >= 0xD800 && code <= 0xDFFF) {
			return SOML__FOUR_DIGIT_FORM ", and a lone surrogate is not a Unicode scalar value. Write the character it is half of as one \\u{…} escape";
		}

		if (code == 0x0D) {
			return SOML__FOUR_DIGIT_FORM ", and a carriage return (U+000D) cannot be represented";
		}

		return SOML__FOUR_DIGIT_FORM ". Write \\u{…} with lowercase hexadecimal digits and no leading zeros";
	}

	if (soml__byte_at(parser, offset + 1) != '{') {
		return "A Unicode escape is written \\u{…} with one to six lowercase hexadecimal digits";
	}

	size_t digit_count = 0;
	bool has_uppercase = false;
	uint64_t value = 0;

	for (size_t index = offset + 2; index < offset + 12 && soml__is_hex_digit(soml__byte_at(parser, index)); index++) {
		int character = soml__byte_at(parser, index);
		has_uppercase |= character >= 'A' && character <= 'F';
		value = value * 16 + (uint64_t)soml__hex_digit_value(character);
		digit_count++;
	}

	if (has_uppercase) {
		// The value is checked before the case, as before the leading zeros, so that lowercasing the digits never gives an escape that is not allowed either.
		const char *message = digit_count <= 6 && soml__byte_at(parser, offset + 2 + digit_count) == '}' ? soml__describe_bad_escape_value(value) : nullptr;
		return message != nullptr ? message : "A Unicode escape uses lowercase hexadecimal digits";
	}

	if (soml__byte_at(parser, offset + 2) == '}') {
		return "A Unicode escape needs one to six hexadecimal digits";
	}

	return digit_count > 6 ? "A Unicode escape has at most six hexadecimal digits" : "A Unicode escape is written \\u{…} with one to six lowercase hexadecimal digits";
}

/*
Decodes the escape at `offset`, which is a backslash, into `output`, and returns the offset after it in `end`. `is_block` is whether it is in a block string, so that an error suggests the literal string of the same kind: ''' for a block, and '...' on one line, which a key is too.
*/
[[nodiscard]] static bool soml__decode_escape(soml__parser *parser, size_t offset, bool is_block, unsigned char *output, size_t *output_length, size_t *end) {
	int character = soml__byte_at(parser, offset + 1);
	*output_length = 1;
	*end = offset + 2;

	switch (character) {
		case 'n': {
			output[0] = '\n';
			return true;
		}

		case 't': {
			output[0] = '\t';
			return true;
		}

		case '\\':
		case '"': {
			output[0] = (unsigned char)character;
			return true;
		}

		case 'u': {
			break;
		}

		case 'r': {
			return soml__fail(parser, offset, "There is no \\r escape, because a carriage return cannot be represented");
		}

		case '\'': {
			return soml__fail(parser, offset, "A ' needs no escape inside \"...\"");
		}

		case -1:
		case '\n': {
			return soml__fail(parser, offset, is_block ? "A backslash must be followed by an escape character. Use \\\\ for a literal backslash, or a ''' string" : "A backslash must be followed by an escape character. Use \\\\ for a literal backslash, or a '...' string");
		}

		default: {
			return soml__fail(parser, offset, is_block ? "Unknown escape. The escapes are \\\\, \\\", \\n, \\t, and \\u{…}; use a ''' string for literal backslashes" : "Unknown escape. The escapes are \\\\, \\\", \\n, \\t, and \\u{…}; use a '...' string for literal backslashes");
		}
	}

	// `\u{` followed by one to six lowercase hexadecimal digits and `}`.
	size_t digits_start = offset + 3;
	size_t index = digits_start;
	uint32_t scalar = 0;

	if (soml__byte_at(parser, offset + 2) == '{') {
		// One digit more than allowed is read, to tell that there are too many, and seven digits still fit in `scalar`.
		while (index - digits_start < 7) {
			int digit = soml__lowercase_hex_value(soml__byte_at(parser, index));

			if (digit == -1) {
				break;
			}

			scalar = scalar * 16 + (uint32_t)digit;
			index++;
		}
	}

	size_t digit_count = index - digits_start;

	if (soml__byte_at(parser, offset + 2) != '{' || digit_count == 0 || digit_count > 6 || soml__byte_at(parser, index) != '}') {
		return soml__fail(parser, offset, soml__describe_bad_unicode_escape(parser, offset + 1));
	}

	// The value is checked before the spelling, so that the leading zeros error never suggests an escape that is not allowed either.
	const char *message = soml__describe_bad_escape_value(scalar);

	if (message != nullptr) {
		return soml__fail(parser, offset, message);
	}

	if (digit_count > 1 && soml__byte_at(parser, digits_start) == '0') {
		return soml__fail(parser, offset, "A Unicode escape may not have leading zeros");
	}

	*output_length = soml__encode_utf8(scalar, output);
	*end = index + 1;
	return true;
}

/*
Decodes the escapes in text[start, end) into `output`, which has room for `end - start` bytes, and adds the length to `*output_length`. A decoded escape is never longer than its source. `is_block` is as for `soml__decode_escape`.
*/
[[nodiscard]] static bool soml__unescape(soml__parser *parser, size_t start, size_t end, bool is_block, unsigned char *output, size_t *output_length) {
	size_t index = start;

	while (index < end) {
		size_t backslash = soml__find_byte(parser->text, end, index, '\\');
		size_t chunk = backslash - index;

		if (chunk > 0) {
			memcpy(output + *output_length, parser->text + index, chunk);
			*output_length += chunk;
		}

		if (backslash == end) {
			break;
		}

		size_t decoded_length;
		size_t escape_end;

		if (!soml__decode_escape(parser, backslash, is_block, output + *output_length, &decoded_length, &escape_end)) {
			return false;
		}

		*output_length += decoded_length;
		index = escape_end;
	}

	return true;
}

/*
The next quote or line feed, and, in an escaped string, backslash. Word loads stay within the supplied range and do not require alignment.
*/
[[nodiscard]] static size_t soml__string_chunk_end(const unsigned char *text, size_t length, size_t index, unsigned char quote) {
	while (length - index >= sizeof(uint64_t)) {
		uint64_t word;
		memcpy(&word, text + index, sizeof(word));
		uint64_t special = soml__equal_byte_mask(word, quote) | soml__equal_byte_mask(word, '\n');

		if (quote == '"') {
			special |= soml__equal_byte_mask(word, '\\');
		}

		if (special != 0) {
			break;
		}

		index += sizeof(word);
	}

	while (index < length && text[index] != quote && text[index] != '\n' && (quote != '"' || text[index] != '\\')) {
		index++;
	}

	return index;
}

/*
Copies decoded bytes into the document and publishes a string only after allocation succeeds.
*/
[[nodiscard]] static bool soml__store_string(soml__parser *parser, const unsigned char *bytes, size_t length, soml_string *output) {
	char *copy = soml__copy_bytes(parser->document, (const char *)bytes, length);

	if (copy == nullptr) {
		return soml__fail_out_of_memory(parser);
	}

	*output = (soml_string){.data = copy, .length = length};
	return true;
}

[[nodiscard]] static bool soml__parse_quoted_string(soml__parser *parser, soml_string *output) {
	const unsigned char *text = parser->text;
	size_t start = parser->position;
	unsigned char quote = text[start];
	size_t index = start + 1;
	size_t length = 0;
	const char *unterminated = quote == '\'' ? "Unterminated string. A '...' string must end on the line it starts on; use a block string (''') for multiple lines" : "Unterminated string. A \"...\" string must end on the line it starts on; use a block string (\"\"\") for multiple lines";

	// An unescaped string needs no scratch storage or second copy.
	size_t first_end = soml__string_chunk_end(text, parser->length, index, quote);

	if (first_end < parser->length && text[first_end] == quote) {
		if (!soml__store_string(parser, text + index, first_end - index, output)) {
			return false;
		}

		parser->position = first_end + 1;
		return true;
	}

	if (first_end == parser->length || text[first_end] == '\n') {
		return soml__fail(parser, start, unterminated);
	}

	// One pass decodes into the parser's scratch block, which grows as needed and is kept from one string to the next, and the result is copied to the document at its length. Escapes are decoded in order, so an invalid escape is reported before a missing quote.
	for (;;) {
		size_t chunk_start = index;

		index = chunk_start == start + 1 ? first_end : soml__string_chunk_end(text, parser->length, index, '"');

		int character = soml__byte_at(parser, index);

		if (character == -1 || character == '\n') {
			return soml__fail(parser, start, unterminated);
		}

		// The text so far, the run of plain characters, and an escape, which decodes to at most 4 bytes.
		size_t chunk_length = index - chunk_start;
		size_t needed;
		unsigned char *scratch = ckd_add(&needed, length, chunk_length) || ckd_add(&needed, needed, character == '\\' ? 4 : 0) ? nullptr : soml__grow(&parser->document->allocator, parser->scratch, &parser->scratch_capacity, 1, needed);

		if (scratch == nullptr) {
			return soml__fail_out_of_memory(parser);
		}

		parser->scratch = scratch;
		memcpy(scratch + length, text + chunk_start, chunk_length);
		length += chunk_length;

		if (character == '"') {
			break;
		}

		size_t decoded_length;

		if (!soml__decode_escape(parser, index, false, scratch + length, &decoded_length, &index)) {
			return false;
		}

		length += decoded_length;
	}

	if (!soml__store_string(parser, parser->scratch, length, output)) {
		return false;
	}
	parser->position = index + 1;
	return true;
}

[[nodiscard]] static bool soml__is_blank(const unsigned char *text, size_t start, size_t end) {
	return soml__skip_blanks(text, start, end) == end;
}

/*
Whether a content line of an unterminated block string ends with its delimiter, as TOML allows, so the error can say to move the delimiter to a new line. A line of only quotes, longer than the delimiter, is content.
*/
[[nodiscard]] SOML__NOINLINE static bool soml__has_inline_closing_delimiter(const soml__parser *parser, size_t content_start, unsigned char quote, size_t delimiter_length) {
	const unsigned char *text = parser->text;
	size_t length = parser->length;

	for (size_t line_start = content_start; line_start < length;) {
		size_t line_end = soml__find_byte(text, length, line_start, '\n');
		size_t start = soml__skip_blanks(text, line_start, line_end);
		size_t end = line_end;

		while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
			end--;
		}

		size_t run = 0;

		while (run < end - start && text[end - 1 - run] == quote) {
			run++;
		}

		if (run >= delimiter_length && run < end - start) {
			return true;
		}

		line_start = line_end + 1;
	}

	return false;
}

/*
A block string opens with a run of three or more quotes and a line break, and closes at the first line whose first content after spaces and tabs is a run of exactly that many quotes. The closing line's indentation is removed from every content line except a blank one, which holds only spaces and tabs and becomes an empty line. Blank lines at the start and the end are dropped.
*/
[[nodiscard]] static bool soml__parse_block_string(soml__parser *parser, unsigned char quote, soml_string *output) {
	const unsigned char *text = parser->text;
	size_t length = parser->length;
	size_t start = parser->position;
	size_t delimiter_length = 0;

	while (start + delimiter_length < length && text[start + delimiter_length] == quote) {
		delimiter_length++;
	}

	parser->position = start + delimiter_length;

	if (soml__is_at_end(parser)) {
		return soml__fail(parser, start, "Unterminated block string");
	}

	if (soml__peek(parser, 0) != '\n') {
		return soml__fail(parser, parser->position, "A block string's opening delimiter must be followed directly by a line break, and its content starts on the next line");
	}

	// Find the closing line, and the first and last lines that are not blank.
	size_t content_start = parser->position + 1;
	size_t line_start = content_start;
	size_t first_line = SIZE_MAX;
	size_t last_line_end = 0;
	size_t indentation_start;
	size_t indentation_end;

	for (;;) {
		if (line_start >= length) {
			return soml__fail(parser, start, soml__has_inline_closing_delimiter(parser, content_start, quote, delimiter_length) ? "Unterminated block string. Its closing delimiter must start a line, so move the delimiter at the end of a content line to a new line" : "Unterminated block string");
		}

		size_t line_end = soml__find_byte(text, length, line_start, '\n');
		size_t content = soml__skip_blanks(text, line_start, line_end);

		size_t run = 0;

		while (content + run < length && text[content + run] == quote) {
			run++;
		}

		if (run == delimiter_length) {
			indentation_start = line_start;
			indentation_end = content;
			parser->position = content + delimiter_length;
			break;
		}

		if (content < line_end) {
			if (first_line == SIZE_MAX) {
				first_line = line_start;
			}

			last_line_end = line_end;
		}

		line_start = line_end + 1;
	}

	size_t indentation_length = indentation_end - indentation_start;
	size_t closing_line_start = indentation_start;

	size_t span = 0;

	// Every line that is not blank must start with the indentation, including lines outside the kept range, which are all blank.
	for (size_t line = content_start; line < closing_line_start;) {
		size_t line_end = soml__find_byte(text, length, line, '\n');

		bool is_blank = soml__is_blank(text, line, line_end);

		if (!is_blank && (line_end - line < indentation_length || memcmp(text + line, text + indentation_start, indentation_length) != 0)) {
			return soml__fail(parser, line, "This line does not start with the indentation of its block string's closing delimiter. Every line except a blank one must start with exactly the same spaces and tabs");
		}

		if (first_line != SIZE_MAX && line >= first_line && line <= last_line_end) {
			span += (line > first_line ? 1 : 0) + (is_blank ? 0 : line_end - line - indentation_length);
		}

		line = line_end + 1;
	}

	// The content is never longer than its lines in the source, because removing the indentation and decoding escapes only make it shorter.
	size_t size;

	if (ckd_add(&size, span, 1)) {
		return soml__fail_out_of_memory(parser);
	}

	unsigned char *buffer = soml__allocate_bytes(parser->document, size);

	if (buffer == nullptr) {
		return soml__fail_out_of_memory(parser);
	}

	size_t output_length = 0;

	if (first_line != SIZE_MAX) {
		for (size_t line = first_line; line <= last_line_end;) {
			size_t line_end = soml__find_byte(text, length, line, '\n');

			if (line > first_line) {
				buffer[output_length++] = '\n';
			}

			if (!soml__is_blank(text, line, line_end)) {
				size_t line_content = line + indentation_length;

				if (quote == '\'') {
					memcpy(buffer + output_length, text + line_content, line_end - line_content);
					output_length += line_end - line_content;
				} else if (!soml__unescape(parser, line_content, line_end, true, buffer, &output_length)) {
					return false;
				}
			}

			if (line_end >= last_line_end) {
				break;
			}

			line = line_end + 1;
		}
	}

	buffer[output_length] = '\0';
	*output = (soml_string){.data = (const char *)buffer, .length = output_length};
	return true;
}

[[nodiscard]] static bool soml__parse_string(soml__parser *parser, soml_string *output) {
	int quote = soml__peek(parser, 0);

	if (soml__peek(parser, 1) == quote && soml__peek(parser, 2) == quote) {
		return soml__parse_block_string(parser, (unsigned char)quote, output);
	}

	return soml__parse_quoted_string(parser, output);
}

// MARK: Numbers

typedef enum soml__number_kind : unsigned char {
	soml__number_none,
	soml__number_decimal,
	soml__number_radix,
	soml__number_float,
} soml__number_kind;

/*
Returns the index after a run of digits with single underscores between them, or `index` when no digit is there.
*/
[[nodiscard]] static size_t soml__skip_digits(const unsigned char *text, size_t length, size_t index, int radix) {
	if (index >= length || !soml__is_radix_digit(text[index], radix)) {
		return index;
	}

	index++;

	for (;;) {
		if (index < length && soml__is_radix_digit(text[index], radix)) {
			index++;
		} else if (index + 1 < length && text[index] == '_' && soml__is_radix_digit(text[index + 1], radix)) {
			index += 2;
		} else {
			return index;
		}
	}
}

/*
The index after an integer part, which is `0` or digits without a leading zero, or `index` when there is none.
*/
[[nodiscard]] static size_t soml__skip_integer_part(const unsigned char *text, size_t length, size_t index) {
	if (index < length && text[index] == '0') {
		return index + 1;
	}

	return soml__skip_digits(text, length, index, 10);
}

[[nodiscard]] static soml__number_kind soml__classify_number(const unsigned char *text, size_t length) {
	int radix = length >= 2 && text[0] == '0' ? soml__radix_of(text[1]) : 0;

	if (radix != 0) {
		size_t end = soml__skip_digits(text, length, 2, radix);
		return end > 2 && end == length ? soml__number_radix : soml__number_none;
	}

	size_t index = length > 0 && text[0] == '-' ? 1 : 0;
	size_t integer_end = soml__skip_integer_part(text, length, index);

	if (integer_end == index) {
		return soml__number_none;
	}

	index = integer_end;
	soml__number_kind kind = soml__number_decimal;

	if (index < length && text[index] == '.') {
		size_t end = soml__skip_digits(text, length, index + 1, 10);

		if (end == index + 1) {
			return soml__number_none;
		}

		index = end;
		kind = soml__number_float;
	}

	if (index < length && text[index] == 'e') {
		bool is_negative = index + 1 < length && text[index + 1] == '-';
		size_t exponent_start = index + (is_negative ? 2 : 1);
		// The exponent follows the integer part's rule for leading zeros, and its zero is only `e0`.
		size_t end = is_negative && exponent_start < length && text[exponent_start] == '0' ? exponent_start : soml__skip_integer_part(text, length, exponent_start);

		if (end == exponent_start) {
			return soml__number_none;
		}

		index = end;
		kind = soml__number_float;
	}

	return index == length ? kind : soml__number_none;
}

/*
The magnitude of a 64-bit integer. For a negative value it is ~x + 1, which never wraps, so it holds for `INT64_MIN` too.
*/
[[nodiscard]] static uint64_t soml__magnitude(int64_t value) {
	return value < 0 ? ~(uint64_t)value + 1 : (uint64_t)value;
}

/*
The 64-bit integer with this sign and magnitude. The magnitude must be at most 2^63 for a negative value, and at most `INT64_MAX` for a positive one.
*/
[[nodiscard]] static int64_t soml__signed(uint64_t magnitude, bool is_negative) {
	if (!is_negative) {
		return (int64_t)magnitude;
	}

	return magnitude == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)magnitude;
}

[[nodiscard]] static bool soml__parse_integer(soml__parser *parser, const unsigned char *text, size_t length, size_t start, int64_t *output) {
	bool is_negative = text[0] == '-';
	size_t index = is_negative ? 1 : 0;
	int radix = length >= 2 && text[0] == '0' ? soml__radix_of(text[1]) : 0;

	if (radix == 0) {
		radix = 10;
	} else {
		index = 2;
	}

	uint64_t limit = is_negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	uint64_t magnitude = 0;
	uint64_t cutoff = limit / (uint64_t)radix;
	uint64_t last_digit = limit % (uint64_t)radix;

	for (; index < length; index++) {
		if (text[index] == '_') {
			continue;
		}

		int character = text[index];
		// The literal matched the grammar, so a letter is an uppercase hexadecimal digit.
		uint64_t digit = (uint64_t)(soml__is_digit(character) ? character - '0' : character - 'A' + 10);

		if (magnitude > cutoff || (magnitude == cutoff && digit > last_digit)) {
			return soml__fail(parser, start, "The integer is outside the 64-bit range (-9223372036854775808 to 9223372036854775807)");
		}
		magnitude = magnitude * (uint64_t)radix + digit;
	}

	*output = soml__signed(magnitude, is_negative);
	return true;
}

[[nodiscard]] static uint64_t soml__power_of_ten(int power) {
	// Exact integer powers for bounded float conversion and duration fractions.
	static constexpr uint64_t powers[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000, 10000000000, 100000000000, 1000000000000, 10000000000000, 100000000000000, 1000000000000000, 10000000000000000, 100000000000000000, 1000000000000000000, 10000000000000000000ULL};
	return powers[power];
}

/*
Converts up to nineteen significant digits with a decimal exponent from -19 to 19 using an exact rational in 128 bits. The scaled numerator and denominator stay below 2^128, and rounding the quotient produces the nearest binary64 mantissa with ties to even, independent of the thread's floating-point mode.
*/
[[nodiscard]] static bool soml__parse_wide_float(const soml__decimal *decimal, double *output) {
	int decimal_exponent = decimal->point - decimal->count;

	if (decimal->truncated || decimal->count == 0 || decimal->count > 19 || decimal_exponent < -19 || decimal_exponent > 19) {
		return false;
	}

	uint64_t mantissa = 0;

	for (int index = 0; index < decimal->count; index++) {
		mantissa = mantissa * 10 + decimal->digits[index];
	}

	soml__wide_unsigned numerator = mantissa;
	soml__wide_unsigned denominator = 1;

	if (decimal_exponent >= 0) {
		numerator *= soml__power_of_ten(decimal_exponent);
	} else {
		denominator = soml__power_of_ten(-decimal_exponent);
	}

	int exponent = __builtin_clzg(denominator) - __builtin_clzg(numerator);

	if (exponent >= 0 ? numerator < (denominator << (unsigned)exponent) : (numerator << (unsigned)-exponent) < denominator) {
		exponent--;
	}

	int shift = soml__mantissa_bits - exponent;

	if (shift >= 0) {
		numerator <<= (unsigned)shift;
	} else {
		denominator <<= (unsigned)-shift;
	}

	soml__wide_unsigned quotient = numerator / denominator;
	soml__wide_unsigned remainder = numerator % denominator;
	soml__wide_unsigned twice_remainder = remainder * 2;

	if (twice_remainder > denominator || (twice_remainder == denominator && (quotient & 1) != 0)) {
		quotient++;
	}

	uint64_t rounded = (uint64_t)quotient;

	if (rounded == (uint64_t)2 << soml__mantissa_bits) {
		rounded >>= 1;
		exponent++;
	}

	uint64_t bits = ((uint64_t)(exponent - soml__exponent_bias) << soml__mantissa_bits) | (rounded & (((uint64_t)1 << soml__mantissa_bits) - 1));
	memcpy(output, &bits, sizeof(bits));
	return true;
}

/*
Clinger's fast path: at most 15 significant digits are an exact binary64 integer, and so is 10^e up to 10^22, so one correctly rounded multiplication or division gives the correctly rounded value. Most floats in a document take this path. It needs arithmetic in binary64 itself, rounding to nearest, so it is off where `FLT_EVAL_METHOD` is not 0 (x87), under `-ffast-math`, which may divide by multiplying with a reciprocal, and while the thread rounds another way. The high-precision path uses only integers, so it is right in every rounding mode.
*/
[[nodiscard]] static bool soml__parse_short_float(const soml__decimal *decimal, double *output) {
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0 && !defined(__FAST_MATH__)
	static constexpr double powers_of_ten[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
	int exponent = decimal->point - decimal->count;

	if (decimal->truncated || decimal->count > 15 || exponent < -22 || exponent > 22) {
		return false;
	}

	// Note: Not using `fegetround()` because it needs libm on some systems. The calling thread may have changed the rounding mode with `fesetround`, and the one multiplication or division is only correct when rounding to nearest. Two sums tell that mode apart from the others without `<fenv.h>`: rounding to nearest gives 1 for 1 + 2^-53, which upward does not, and 1 + 2^-52 for 1 + 1.5 × 2^-53, which downward and toward zero do not. `volatile` keeps the compiler from working them out in advance.
	volatile double one = 1.0;

	if (one + 0x1p-53 != 1.0 || one + 0x1.8p-53 != 1.0 + 0x1p-52) {
		return false;
	}

	uint64_t mantissa = 0;

	for (int index = 0; index < decimal->count; index++) {
		mantissa = mantissa * 10 + decimal->digits[index];
	}

	if (exponent == 0) {
		*output = (double)mantissa;
	} else {
		*output = exponent > 0 ? (double)mantissa * powers_of_ten[exponent] : (double)mantissa / powers_of_ten[-exponent];
	}
	return true;
#else
	(void)decimal;
	(void)output;
	return false;
#endif
}

/*
Reads a float literal that matches the grammar, rounding to nearest with ties to even. The literal is normalized first: underscores and leading and trailing zeros are dropped, and the decimal exponent is computed exactly, so that the conversion only sees at most 800 digits and a small exponent.
*/
[[nodiscard]] static bool soml__parse_float(soml__parser *parser, const unsigned char *text, size_t length, size_t start, double *output) {
	bool is_negative = text[0] == '-';
	size_t index = is_negative ? 1 : 0;
	soml__decimal decimal = {.count = 0};
	// The position of the decimal point relative to the first significant digit, counted as the literal is read.
	int64_t point = 0;
	bool has_significant_digit = false;
	bool has_seen_point = false;

	for (; index < length && text[index] != 'e'; index++) {
		unsigned char character = text[index];

		if (character == '_') {
			continue;
		}

		if (character == '.') {
			has_seen_point = true;
			continue;
		}

		int digit = character - '0';

		if (!has_significant_digit) {
			if (digit == 0) {
				if (has_seen_point) {
					point--;
				}

				continue;
			}

			has_significant_digit = true;
		}

		if (!has_seen_point) {
			point++;
		}

		if (decimal.count < soml__decimal_capacity) {
			decimal.digits[decimal.count++] = (unsigned char)digit;
		} else if (digit != 0) {
			decimal.truncated = true;
		}
	}

	if (!has_significant_digit) {
		// Negative zero is the same value as zero.
		*output = 0.0;
		return true;
	}

	// The exponent saturates, which cannot change the result, because the limits below are far smaller.
	int64_t exponent = 0;

	if (index < length) {
		index++;
		bool is_exponent_negative = index < length && text[index] == '-';

		if (is_exponent_negative) {
			index++;
		}

		for (; index < length; index++) {
			if (text[index] != '_' && exponent < 1000000000000) {
				exponent = exponent * 10 + (text[index] - '0');
			}
		}

		if (is_exponent_negative) {
			exponent = -exponent;
		}
	}

	soml__decimal_trim(&decimal);
	// A decimal point this far out is infinite or zero either way, so clamping it into an `int` changes nothing.
	int64_t decimal_point = point + exponent;
	if (decimal_point > 1000) {
		decimal_point = 1000;
	} else if (decimal_point < -1000) {
		decimal_point = -1000;
	}

	decimal.point = (int)decimal_point;
	double value;

	if (soml__parse_short_float(&decimal, &value) || soml__parse_wide_float(&decimal, &value)) {
		*output = is_negative ? -value : value;
		return true;
	}

	if (!soml__decimal_to_double(&decimal, &value)) {
		return soml__fail(parser, start, is_negative ? "The float is too large to be finite. Use -infinity if you mean it" : "The float is too large to be finite. Use infinity if you mean it");
	}

	// The literal has a nonzero digit, so an exact zero means that it rounded to zero.
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));

	if (bits == 0) {
		return soml__fail(parser, start, "The float is too small to be told apart from zero. Write 0.0 if you mean zero");
	}

	*output = is_negative ? -value : value;
	return true;
}

/*
A message for a token that is not a number, an instant, or a duration, following the reference implementation's checks in order. `is_whole_text` is whether the token is all of the unquoted text it begins, as in `0644` but not `0412 345 678`.
*/
[[nodiscard]] static const char *soml__describe_bad_number(const unsigned char *text, size_t length, bool is_whole_text) {
	size_t index = text[0] == '-' ? 1 : 0;
	if (memchr(text, '+', length) != nullptr) {
		return "A “+” sign is not allowed in a number, including in an exponent";
	}

	if (index + 1 < length && text[index] == '0' && (text[index + 1] == 'B' || text[index + 1] == 'O' || text[index + 1] == 'X')) {
		return "A number prefix is lowercase: 0x, 0o, or 0b";
	}

	int radix = index + 1 < length && text[index] == '0' ? soml__radix_of(text[index + 1]) : 0;

	if (radix != 0) {
		if (index == 1) {
			return "An integer with a radix prefix cannot have a sign, because it states a bit pattern rather than a quantity";
		}

		if (length == 2) {
			return "Expected digits after the radix prefix";
		}

		bool has_lowercase_hex = false;
		bool is_hex_like = true;

		for (size_t position = 2; position < length; position++) {
			unsigned char character = text[position];

			if (character >= 'a' && character <= 'f') {
				has_lowercase_hex = true;
			} else if (!soml__is_digit(character) && !(character >= 'A' && character <= 'F') && character != '_') {
				is_hex_like = false;
			}
		}

		if (radix == 16 && has_lowercase_hex && is_hex_like) {
			// The uppercase spelling is only suggested when it is valid, so a misplaced underscore is reported first, and a value outside the 64-bit range gets no example.
			if (text[2] == '_' || text[length - 1] == '_' || soml__find_pair(text, length, 2, '_', '_') < length) {
				return "An underscore in a number must be between two digits";
			}

			size_t significant = 2;
			size_t digit_count = 0;

			while (significant < length && (text[significant] == '0' || text[significant] == '_')) {
				significant++;
			}

			for (size_t position = significant; position < length; position++) {
				digit_count += text[position] != '_';
			}

			bool is_in_range = digit_count < 16 || (digit_count == 16 && text[significant] <= '7');
			return is_in_range ? "Hexadecimal digits are uppercase, as in 0xFF" : "Hexadecimal digits are uppercase";
		}

		for (size_t position = 2; position < length; position++) {
			if (text[position] != '_' && !soml__is_radix_digit(text[position], radix)) {
				return radix == 16 ? "Invalid hexadecimal digit" : (radix == 8 ? "Invalid octal digit" : "Invalid binary digit");
			}
		}

		return "An underscore in a number must be between two digits";
	}

	// The shape `-?digits(.digits)?` followed by what comes next, where digits may hold underscores.
	size_t position = soml__skip_digits_and_underscores(text, length, index);
	bool has_integer_digit = position > index && soml__is_digit(text[index]);
	size_t mantissa_end = position;

	// Underscores alone before the point, as in `_.`, are digits here, as in the reference implementation.
	if (position > index && position < length && text[position] == '.') {
		size_t fraction = soml__skip_digits_and_underscores(text, length, position + 1);

		if (fraction == position + 1) {
			return "A decimal point must be followed by a digit";
		}

		mantissa_end = fraction;
	}

	// Only when the “E” is where an exponent marker goes, with digits, a `-`, or nothing after it. In `5EUR` or `10EB`, it is part of a word, so lowercasing it would not help.
	if (has_integer_digit && mantissa_end < length && text[mantissa_end] == 'E') {
		size_t exponent = mantissa_end + 1 < length && text[mantissa_end + 1] == '-' ? mantissa_end + 2 : mantissa_end + 1;

		if (soml__skip_digits_and_underscores(text, length, exponent) == length) {
			return "An exponent marker is a lowercase “e”";
		}
	}

	if (length >= 2 && text[0] == '-' && text[1] == '.') {
		return "A number cannot begin with “.”; write a digit before it, as in -0.5";
	}

	// Groups of digits separated by colons, as in a time of day, such as `12:30`, which is `H:MM` or `HH:MM`, with optional `:SS`. Like the reference implementation, this looks at the first 1,000 characters only.
	size_t limit = length > 1000 ? 1000 : length;
	size_t group_count = 0;
	bool is_time = true;
	bool is_colon_separated = false;

	for (size_t group_start = 0; group_start < limit;) {
		size_t group_end = group_start;

		while (group_end < limit && soml__is_digit(text[group_end])) {
			group_end++;
		}

		size_t group_length = group_end - group_start;
		is_time = is_time && group_count < 3 && (group_count == 0 ? group_length <= 2 : group_length == 2);
		group_count++;

		if (group_length == 0 || (group_end < limit && text[group_end] != ':')) {
			break;
		}

		if (group_end == limit) {
			is_colon_separated = group_count >= 2;
			break;
		}

		group_start = group_end + 1;
	}

	if (is_colon_separated) {
		return is_time ? "A time of day is a string, so it must be quoted" : "Invalid number. A value that contains “:” must be quoted";
	}

	// A value with a `:` that starts with a zero, such as the MAC address `00:1A:2B`, is not a number with a leading zero, because removing the zero would not make it valid.
	if (index + 1 < length && text[index] == '0' && (soml__is_digit(text[index + 1]) || text[index + 1] == '_') && memchr(text, ':', length) == nullptr) {
		// Removing the zero gives a valid number with another meaning, so for digits alone the message says what the zero usually meant.
		size_t digits_end = 0;
		bool is_octal = true;

		while (digits_end < length && soml__is_digit(text[digits_end])) {
			is_octal = is_octal && text[digits_end] <= '7';
			digits_end++;
		}

		if (digits_end < length) {
			return "Leading zeros are not allowed in a decimal number";
		}

		// The octal suggestion is only for the number on its own, not for one that more text follows, as in `0412 345 678`, and only when it is in the 64-bit range, which holds 21 octal digits after the leading zeros.
		size_t significant = 0;

		while (significant < length && text[significant] == '0') {
			significant++;
		}

		return is_octal && is_whole_text && length - significant <= 21 ? "Leading zeros are not allowed in a decimal number. Write an octal number, such as a file mode, with the prefix 0o, and an identifier, such as a ZIP code, as a string" : "Leading zeros are not allowed in a decimal number. Write an identifier, such as a ZIP code, as a string";
	}

	// Only in a number: an underscore next to a letter other than the exponent marker is part of a word, such as `4k_video`.
	bool has_only_number_characters = true;

	for (size_t character = 0; character < length; character++) {
		if (!soml__is_digit(text[character]) && text[character] != '-' && text[character] != '.' && text[character] != '_' && text[character] != 'e') {
			has_only_number_characters = false;
			break;
		}
	}

	for (size_t underscore = 0; has_only_number_characters && underscore < length; underscore++) {
		if (text[underscore] == '_' && (underscore == 0 || underscore + 1 == length || !soml__is_digit(text[underscore - 1]) || !soml__is_digit(text[underscore + 1]))) {
			return "An underscore in a number must be between two digits";
		}
	}

	if (has_integer_digit && mantissa_end < length && text[mantissa_end] == 'e') {
		size_t exponent = mantissa_end + 1;
		bool is_negative = exponent < length && text[exponent] == '-';

		if (is_negative) {
			exponent++;
		}

		if (exponent == length) {
			return "Expected digits after the exponent marker “e”";
		}

		if (text[exponent] == '0') {
			if (exponent + 1 == length) {
				return "“e-0” is not allowed, because an exponent of zero has one spelling: e0";
			}

			if (soml__is_digit(text[exponent + 1]) || text[exponent + 1] == '_') {
				return "Leading zeros are not allowed in an exponent";
			}
		}
	}

	size_t dot_count = 0;

	for (size_t dot = 0; dot < length; dot++) {
		dot_count += text[dot] == '.';
	}

	if (dot_count > 1) {
		return "Invalid number. A value with several dots, such as a version number, must be quoted";
	}

	if (text[0] == '-' && (length == 1 || !soml__is_digit(text[1]))) {
		// The whole word, as for one without the “-”, so that a word such as `-nano` or `-info` is not taken for NaN or infinity.
		if (soml__equals_ignoring_case(text + 1, length - 1, (soml_string){"nan", 3})) {
			return "NaN is not representable. Use null for a missing value";
		}

		if (soml__equals_ignoring_case(text + 1, length - 1, (soml_string){"inf", 3}) || soml__equals_ignoring_case(text + 1, length - 1, (soml_string){"infinity", 8})) {
			return "Negative infinity is written -infinity";
		}

		return "Expected a digit or “infinity” after “-”";
	}

	for (size_t letter = 0; letter < length; letter++) {
		if (soml__is_letter(text[letter])) {
			return "Invalid number. A string value must be quoted";
		}
	}

	return "Invalid number";
}

/*
Whether a duration unit is a year unit in any case: y, yr, yrs, year, or years.
*/
[[nodiscard]] static bool soml__is_year_unit(const unsigned char *unit, size_t length) {
	return soml__equals_ignoring_case(unit, length, (soml_string){"y", 1}) || soml__equals_ignoring_case(unit, length, (soml_string){"yr", 2}) || soml__equals_ignoring_case(unit, length, (soml_string){"yrs", 3}) || soml__equals_ignoring_case(unit, length, (soml_string){"year", 4}) || soml__equals_ignoring_case(unit, length, (soml_string){"years", 5});
}

/*
Whether a token that is not a number or an instant was meant as a duration: its first letter could begin a unit, a day, or a week, or it is a year unit.
*/
[[nodiscard]] static bool soml__is_duration_like(const unsigned char *text, size_t length) {
	size_t index = length > 0 && text[0] == '-' ? 1 : 0;

	if (index >= length || !soml__is_digit(text[index])) {
		return false;
	}

	index = soml__skip_digits_and_underscores(text, length, index);

	if (index < length && text[index] == '.') {
		index = soml__skip_digits_and_underscores(text, length, index + 1);
	}

	if (index >= length) {
		return false;
	}

	switch (text[index] | 0x20) {
		case 'd':
		case 'h':
		case 'm':
		case 'n':
		case 's':
		case 'u':
		case 'w': {
			return true;
		}

		default: {
			// A year unit, as in `1y` or `2years`, is only one when the token or another part follows it, so that a word such as `100yen` stays a string to quote.
			size_t unit_end = index;

			while (unit_end < length && soml__is_letter(text[unit_end])) {
				unit_end++;
			}

			return soml__is_year_unit(text + index, unit_end - index) && (unit_end == length || soml__is_digit(text[unit_end]));
		}
	}
}

typedef struct soml__unit {
	// The length in nanoseconds is multiplier × 10^power.
	uint64_t multiplier;
	int power;
	uint64_t nanoseconds;
} soml__unit;

static constexpr soml__unit soml__units[] = {
	{36, 11, soml__nanoseconds_per_hour},
	{6, 10, soml__nanoseconds_per_minute},
	{1, 9, soml__nanoseconds_per_second},
	{1, 6, 1000000},
	{1, 3, 1000},
	{1, 0, 1},
};

/*
The rank of one of the six exact duration units, or -1. The rank is also its index in `soml__units`.
*/
[[nodiscard]] static int soml__unit_rank(const unsigned char *unit, size_t length) {
	if (length == 1) {
		switch (unit[0]) {
			case 'h': {
				return 0;
			}
			case 'm': {
				return 1;
			}
			case 's': {
				return 2;
			}
			default: {
				return -1;
			}
		}
	}

	if (length == 2 && unit[1] == 's') {
		switch (unit[0]) {
			case 'm': {
				return 3;
			}
			case 'u': {
				return 4;
			}
			case 'n': {
				return 5;
			}
			default: {
				return -1;
			}
		}
	}

	return -1;
}

/*
A message for a duration part whose unit is not one, given the unit and the whole token.
*/
[[nodiscard]] static const char *soml__describe_bad_unit(const unsigned char *unit, size_t length, const unsigned char *text, size_t text_length) {
	if (length == 0) {
		return "Invalid duration: every number needs a unit: h, m, s, ms, us, or ns";
	}

	if ((length == 1 && unit[0] == 'd') || (length == 3 && memcmp(unit, "day", 3) == 0) || (length == 4 && memcmp(unit, "days", 4) == 0)) {
		return "Invalid duration: there is no day unit, because a day is not a fixed length. Write 24h for a fixed 24 hours";
	}

	if ((length == 1 && unit[0] == 'w') || (length == 4 && memcmp(unit, "week", 4) == 0) || (length == 5 && memcmp(unit, "weeks", 5) == 0)) {
		return "Invalid duration: there is no week unit, because a day is not a fixed length. Write 168h for a fixed 168 hours";
	}

	if (soml__is_year_unit(unit, length)) {
		return "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days";
	}

	if (length == 1 && unit[0] == 'M') {
		// A number with `M` alone is more often a size, as in `memory: 512M`, and `512m` would read as minutes.
		bool is_size = soml__skip_digits_and_underscores(text, text_length, 0) + 1 == text_length;
		return is_size ? "Invalid duration: there is no month unit, because a month is not a fixed length. A size, such as 512M, is a string" : "Invalid duration: there is no month unit, because a month is not a fixed length";
	}

	if (length <= 2) {
		char lowercase[2] = {(char)(unit[0] | 0x20), length == 2 ? (char)(unit[1] | 0x20) : '\0'};

		if (soml__unit_rank((const unsigned char *)lowercase, length) >= 0) {
			return "Invalid duration: the units are lowercase";
		}
	}

	return "Invalid duration: that is not a unit. The units are h, m, s, ms, us, and ns, and a string must be quoted";
}

/*
Reads every part in order, so each error names the part it is about, and then checks the value: a whole number of nanoseconds, no `-` before zero, and the int64 range.
*/
[[nodiscard]] static bool soml__parse_duration(soml__parser *parser, const unsigned char *text, size_t length, size_t start, int64_t *output) {
	bool is_negative = text[0] == '-';
	size_t index = is_negative ? 1 : 0;
	int previous_rank = -1;
	uint64_t total = 0;
	bool is_out_of_range = false;
	// The last part's fraction without trailing zeros, and its unit. Only the last part may have one.
	size_t fraction_start = 0;
	size_t fraction_length = 0;
	const soml__unit *fraction_unit = nullptr;

	while (index < length) {
		size_t digits_end = soml__skip_integer_part(text, length, index);

		if (digits_end == index) {
			if (text[index] == '-' && index + 1 < length && soml__is_digit(text[index + 1])) {
				return soml__fail(parser, start, "Invalid duration: only the whole duration takes a sign, as in -1h30m");
			}

			return soml__fail(parser, start, text[index] == '+' ? "Invalid duration: a “+” sign is not allowed" : "Invalid duration: expected a number");
		}

		int next = digits_end < length ? text[digits_end] : -1;

		if (text[index] == '0' && (soml__is_digit(next) || next == '_')) {
			return soml__fail(parser, start, "Invalid duration: leading zeros are not allowed");
		}

		if (next == '_') {
			return soml__fail(parser, start, "Invalid duration: an underscore must be between two digits");
		}

		fraction_length = 0;
		size_t unit_start = digits_end;

		if (next == '.') {
			unit_start = soml__skip_digits(text, length, digits_end + 1, 10);

			if (unit_start == digits_end + 1) {
				return soml__fail(parser, start, "Invalid duration: a “.” must be followed by a digit");
			}

			if (unit_start < length && text[unit_start] == '_') {
				return soml__fail(parser, start, "Invalid duration: an underscore must be between two digits");
			}

			fraction_start = digits_end + 1;
			fraction_length = unit_start - fraction_start;
		}

		size_t unit_end = unit_start;

		while (unit_end < length && soml__is_letter(text[unit_end])) {
			unit_end++;
		}

		int rank = soml__unit_rank(text + unit_start, unit_end - unit_start);

		if (rank < 0) {
			return soml__fail(parser, start, soml__describe_bad_unit(text + unit_start, unit_end - unit_start, text, length));
		}

		const soml__unit *unit = &soml__units[rank];

		if (rank <= previous_rank) {
			return soml__fail(parser, start, "Invalid duration: the units are in the order h, m, s, ms, us, ns, and each appears at most once");
		}

		if (next == '.' && unit_end < length && soml__is_digit(text[unit_end])) {
			return soml__fail(parser, start, "Invalid duration: only the last part may have a fraction");
		}

		uint64_t unit_length = unit->nanoseconds;
		uint64_t whole = 0;

		for (size_t digit = index; digit < digits_end; digit++) {
			if (text[digit] != '_' && (ckd_mul(&whole, whole, 10) || ckd_add(&whole, whole, (uint64_t)(text[digit] - '0')))) {
				is_out_of_range = true;
			}
		}

		uint64_t part;

		if (ckd_mul(&part, whole, unit_length) || ckd_add(&total, total, part)) {
			is_out_of_range = true;
		}

		fraction_unit = unit;
		index = unit_end;
		previous_rank = rank;
	}

	if (fraction_length > 0) {
		// The fraction's digits without underscores and trailing zeros, as F / 10^k.
		uint64_t numerator = 0;
		size_t digit_count = 0;
		size_t pending_zeros = 0;

		for (size_t digit = fraction_start; digit < fraction_start + fraction_length; digit++) {
			if (text[digit] == '_') {
				continue;
			}

			if (text[digit] == '0') {
				pending_zeros++;
				continue;
			}

			// More than 13 significant digits is finer than a nanosecond in any unit.
			if (digit_count + pending_zeros + 1 > 13) {
				return soml__fail(parser, start, "Invalid duration: it is not a whole number of nanoseconds");
			}

			numerator *= soml__power_of_ten((int)pending_zeros);
			digit_count += pending_zeros;
			pending_zeros = 0;

			numerator = numerator * 10 + (uint64_t)(text[digit] - '0');
			digit_count++;
		}

		if (digit_count > 0) {
			// F × multiplier × 10^power / 10^k must be whole. F has no factor of 10, and no multiplier has a factor of 5, so that needs k ≤ power + 2.
			size_t power = (size_t)fraction_unit->power;

			if (digit_count > power + 2) {
				return soml__fail(parser, start, "Invalid duration: it is not a whole number of nanoseconds");
			}

			// Neither product can overflow: the numerator is below 10^13, the multiplier is at most 36, and the fraction is less than one of its unit.
			uint64_t scaled = numerator * fraction_unit->multiplier;
			uint64_t fraction_nanoseconds;

			if (digit_count <= power) {
				fraction_nanoseconds = scaled * soml__power_of_ten((int)(power - digit_count));
			} else {
				uint64_t divisor = soml__power_of_ten((int)(digit_count - power));

				if (scaled % divisor != 0) {
					return soml__fail(parser, start, "Invalid duration: it is not a whole number of nanoseconds");
				}

				fraction_nanoseconds = scaled / divisor;
			}

			if (ckd_add(&total, total, fraction_nanoseconds)) {
				is_out_of_range = true;
			}
		}
	}

	if (is_negative && total == 0 && !is_out_of_range) {
		return soml__fail(parser, start, "Invalid duration: “-” is not allowed before zero, because zero has one spelling: 0s");
	}

	if (is_out_of_range || total > (is_negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX)) {
		return soml__fail(parser, start, "Invalid duration: it is outside the 64-bit range of nanoseconds, about 292 years either way");
	}

	*output = soml__signed(total, is_negative);
	return true;
}

[[nodiscard]] static bool soml__has_digits(const unsigned char *text, size_t start, size_t count) {
	for (size_t index = start; index < start + count; index++) {
		if (!soml__is_digit(text[index])) {
			return false;
		}
	}

	return true;
}

[[nodiscard]] static int64_t soml__read_digits(const unsigned char *text, size_t start, size_t count) {
	int64_t value = 0;

	for (size_t index = start; index < start + count; index++) {
		value = value * 10 + (text[index] - '0');
	}

	return value;
}

/*
Whether the text, which starts with a date, continues with `THH:MM:SS`.
*/
[[nodiscard]] static bool soml__has_time(const unsigned char *text, size_t length) {
	return length >= 19 && text[10] == 'T' && soml__has_digits(text, 11, 2) && text[13] == ':' && soml__has_digits(text, 14, 2) && text[16] == ':' && soml__has_digits(text, 17, 2);
}

/*
Whether the text, which starts with a date, has the shape of an instant: `YYYY-MM-DDTHH:MM:SS(.fraction)?(Z|±HH:MM)`.
*/
[[nodiscard]] static bool soml__is_instant_format(const unsigned char *text, size_t length) {
	// The time and at least one more character, for the offset.
	if (!soml__has_time(text, length)) {
		return false;
	}

	size_t index = 19;

	if (index < length && text[index] == '.') {
		size_t fraction_start = ++index;

		while (index < length && soml__is_digit(text[index])) {
			index++;
		}

		if (index == fraction_start) {
			return false;
		}
	}

	bool has_utc_offset = index + 1 == length && text[index] == 'Z';
	bool has_numeric_offset = index + 6 == length && (text[index] == '+' || text[index] == '-') && soml__has_digits(text, index + 1, 2) && text[index + 3] == ':' && soml__has_digits(text, index + 4, 2);
	return has_utc_offset || has_numeric_offset;
}

/*
Whether the text, which starts with a date, is a date and a time without an offset: `YYYY-MM-DDTHH:MM:SS(.fraction)?`.
*/
[[nodiscard]] static bool soml__is_local_date_time(const unsigned char *text, size_t length) {
	// A complete date and time with an optional fraction, but no offset.
	return soml__has_time(text, length) && (length == 19 || (length > 20 && text[19] == '.' && soml__has_digits(text, 20, length - 20)));
}

/*
Reads an instant in the shape of one, with every component range-checked, the date checked to exist, and the instant checked to be in range in UTC. Returns `nullptr`, or the message for the first check that fails.
*/
[[nodiscard]] static const char *soml__read_instant(const unsigned char *text, soml_instant *output) {
	size_t fraction_start = 20;
	size_t offset_start = 19;

	if (text[19] == '.') {
		offset_start = fraction_start;

		// The text has the shape of an instant, so a `Z` or an offset ends the digits.
		while (soml__is_digit(text[offset_start])) {
			offset_start++;
		}
	}

	size_t fraction_length = offset_start > fraction_start ? offset_start - fraction_start : 0;

	if (fraction_length > 9) {
		return "Invalid instant: a fractional second has at most nine digits";
	}

	int64_t year = soml__read_digits(text, 0, 4);
	int64_t month = soml__read_digits(text, 5, 2);
	int64_t day = soml__read_digits(text, 8, 2);
	int64_t hour = soml__read_digits(text, 11, 2);
	int64_t minute = soml__read_digits(text, 14, 2);
	int64_t second = soml__read_digits(text, 17, 2);

	if (year == 0) {
		return "Invalid instant: the year must be 0001 to 9999";
	}

	if (month < 1 || month > 12) {
		return "Invalid instant: the month must be 01 to 12";
	}

	if (day < 1 || day > soml__days_in_month(year, month)) {
		return "Invalid instant: the day does not exist in that month";
	}

	if (hour > 23) {
		return "Invalid instant: the hour must be 00 to 23";
	}

	if (minute > 59) {
		return "Invalid instant: the minute must be 00 to 59";
	}

	if (second > 59) {
		return "Invalid instant: the second must be 00 to 59, and a leap second is not representable";
	}

	int64_t offset_seconds = 0;

	if (text[offset_start] != 'Z') {
		int64_t offset_hour = soml__read_digits(text, offset_start + 1, 2);
		int64_t offset_minute = soml__read_digits(text, offset_start + 4, 2);

		if (offset_hour > 23) {
			return "Invalid instant: the offset hour must be 00 to 23";
		}

		if (offset_minute > 59) {
			return "Invalid instant: the offset minute must be 00 to 59";
		}

		bool is_negative = text[offset_start] == '-';

		if (is_negative && offset_hour == 0 && offset_minute == 0) {
			return "Invalid instant: -00:00 means “offset unknown” in RFC 3339, which is not representable; use Z or +00:00";
		}

		offset_seconds = (offset_hour * 3600 + offset_minute * 60) * (is_negative ? -1 : 1);
	}

	int64_t seconds = soml__days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;

	if (seconds < soml__minimum_instant_seconds || seconds > soml__maximum_instant_seconds) {
		return "Invalid instant: in UTC it falls outside the years 0001 to 9999";
	}

	int32_t nanoseconds = 0;

	for (size_t digit = 0; digit < 9; digit++) {
		nanoseconds = nanoseconds * 10 + (digit < fraction_length ? text[fraction_start + digit] - '0' : 0);
	}

	*output = (soml_instant){.seconds = seconds, .nanoseconds = nanoseconds};
	return nullptr;
}

/*
Whether the text, which starts with a date, is a valid instant, so that an error only suggests an instant that is one.
*/
[[nodiscard]] static bool soml__is_valid_instant(const unsigned char *text, size_t length) {
	soml_instant instant;
	return soml__is_instant_format(text, length) && soml__read_instant(text, &instant) == nullptr;
}

#define SOML__INSTANT_FORMAT "An instant is written as 2026-09-19T14:00:00Z, with an optional fraction of up to nine digits and an offset of Z or ±HH:MM"
#define SOML__SPACE_SEPARATOR "The date and time separator in an instant is an uppercase “T”, not a space"

/*
A date and a time with a space between them, as TOML and Python write a date and time, where an instant has a "T". The instant is only shown when it is valid, and following the example must not turn a local time into UTC silently, so a time without an offset is described as what it is. `date` is the date's 10 characters, and `time` the token after the space.
*/
[[nodiscard]] static const char *soml__describe_space_separated_instant(const unsigned char *date, const unsigned char *time, size_t time_length, bool is_whole_value) {
	// The longest valid instant has 35 characters, so a longer one, with its "Z", is not valid.
	unsigned char instant[40];
	size_t length = 11 + time_length;

	if (length >= sizeof(instant)) {
		return SOML__SPACE_SEPARATOR ". " SOML__INSTANT_FORMAT;
	}

	memcpy(instant, date, 10);
	instant[10] = 'T';
	memcpy(instant + 11, time, time_length);

	if (soml__is_valid_instant(instant, length)) {
		return SOML__SPACE_SEPARATOR ", as in 2026-09-19T14:00:00Z";
	}

	bool is_local = soml__is_local_date_time(instant, length);
	instant[length] = 'Z';

	if (is_whole_value && is_local && soml__is_valid_instant(instant, length + 1)) {
		return SOML__SPACE_SEPARATOR ", and an instant needs the offset it was meant in, as in 2026-09-19T14:00:00Z for UTC. A date and time without an offset is a local time, which is not an instant, so write it as a string, as in '2026-09-19 14:00:00'";
	}

	return SOML__SPACE_SEPARATOR ". " SOML__INSTANT_FORMAT;
}

/*
A message for the token at `start`, which starts with a date but is not in the shape of an instant, following the reference implementation's checks in order, except that a date and time without an offset whose date or time does not exist gets the error for that part. Like it, a token of more than 1,000 characters gets the general message.
*/
[[nodiscard]] SOML__NOINLINE static const char *soml__describe_bad_instant(const soml__parser *parser, size_t start, size_t length) {
	const unsigned char *text = parser->text + start;
	const char *generic = "Invalid instant. " SOML__INSTANT_FORMAT;

	if (length > 1000) {
		return generic;
	}

	// A time after a space, and whether the value ends after it, or after the token without one. Advance past the token only when a time follows, so an EOF date keeps pointers within the input. More of the instant may follow, as in `14:00:00,5Z` or `14:00:00[Europe/Oslo]`, so it may have an offset.
	size_t time_start = start + length;
	size_t time_end = time_start;

	if (soml__byte_at(parser, time_start) == ' ' && soml__is_digit(soml__byte_at(parser, time_start + 1))) {
		time_start++;
		time_end = time_start + 1;

		while (time_end < parser->length && soml__is_number_character(parser->text[time_end])) {
			time_end++;
		}
	}

	bool is_whole_value = soml__is_value_end_at(parser, time_end) && !(soml__byte_at(parser, time_end) == ',' && soml__is_digit(soml__byte_at(parser, time_end + 1)));

	if (length == 10) {
		size_t time_length = time_end > time_start ? time_end - time_start : 0;
		const unsigned char *time = parser->text + time_start;
		bool is_time = time_length >= 5 && soml__has_digits(time, 0, 2) && time[2] == ':' && soml__has_digits(time, 3, 2);
		if (is_time) {
			return soml__describe_space_separated_instant(text, time, time_length, is_whole_value);
		}

		// The instant a date could be is only shown when the date exists, as in the reference implementation, whose example is the date itself.
		int64_t year = soml__read_digits(text, 0, 4);
		int64_t month = soml__read_digits(text, 5, 2);
		int64_t day = soml__read_digits(text, 8, 2);
		bool is_existing = year >= 1 && month >= 1 && month <= 12 && day >= 1 && day <= soml__days_in_month(year, month);
		return is_existing ? "A date is not an instant. Write a date as a string, as in '2026-09-19'. An instant needs a time and an offset, as in 2026-09-19T00:00:00Z" : "A date is not an instant. Write a date as a string, as in '2026-09-19'";
	}

	if (text[10] == 't') {
		return "The date and time separator in an instant is an uppercase “T”";
	}

	if (text[length - 1] == 'z') {
		return "The UTC offset in an instant is an uppercase “Z”";
	}

	if (length >= 5 && (text[length - 5] == '+' || text[length - 5] == '-') && soml__has_digits(text, length - 4, 4)) {
		return "An instant's offset is written with a colon, as in +07:00";
	}

	if (!soml__is_local_date_time(text, length)) {
		return generic;
	}

	// The token has at most 1,000 characters, and an offset at most 6.
	unsigned char instant[1006];
	memcpy(instant, text, length);

	// An offset on its own after spaces, as in `2026-09-19T14:00:00 Z`, belongs directly after the time. The instant has the offset it was meant in, so it is not a local time to write as a string.
	const unsigned char *source = parser->text;
	size_t offset_start = soml__skip_blanks(source, start + length, parser->length);
	bool is_numeric_offset = offset_start + 6 <= parser->length && (source[offset_start] == '+' || source[offset_start] == '-') && soml__has_digits(source, offset_start + 1, 2) && source[offset_start + 3] == ':' && soml__has_digits(source, offset_start + 4, 2);
	size_t offset_length = is_numeric_offset ? 6 : (soml__byte_at(parser, offset_start) == 'Z' ? 1 : 0);

	if (offset_start > start + length && offset_length > 0 && soml__is_value_end_at(parser, offset_start + offset_length)) {
		memcpy(instant + length, source + offset_start, offset_length);
		return soml__is_valid_instant(instant, length + offset_length) ? "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z" : "An instant's offset follows its time directly, without a space";
	}

	// Adding an offset only gives an instant when the date and time exist, so otherwise the reason they do not is the error.
	instant[length] = 'Z';
	const char *message = soml__read_instant(instant, &(soml_instant){});

	if (message != nullptr) {
		return message;
	}

	return is_whole_value ? "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in" : "An instant needs an offset: Z or ±HH:MM";
}

/*
`YYYY-MM-DDTHH:MM:SS(.fraction)?(Z|±HH:MM)`, with every component range-checked, the date checked to exist, and the instant checked to be in range in UTC.
*/
[[nodiscard]] static bool soml__parse_instant(soml__parser *parser, const unsigned char *text, size_t length, size_t start, soml_instant *output) {
	if (!soml__is_instant_format(text, length)) {
		return soml__fail(parser, start, soml__describe_bad_instant(parser, start, length));
	}

	const char *message = soml__read_instant(text, output);
	return message == nullptr || soml__fail(parser, start, message);
}

/*
Where the digits that end before `end` start.
*/
[[nodiscard]] static size_t soml__digits_start(const unsigned char *text, size_t end) {
	while (end > 0 && soml__is_digit(text[end - 1])) {
		end--;
	}

	return end;
}

/*
A comma used as a thousands separator, as in `[1,000]`, makes the group after it a separate item, which fails only when it has a leading zero, as the token at `start` does. Writing that item in octal, as the leading zero message suggests, would parse to the wrong items. Returns `nullptr` when the token is not such a group.
*/
[[nodiscard]] SOML__NOINLINE static const char *soml__describe_thousands_separator(const soml__parser *parser, size_t start, size_t length) {
	const unsigned char *text = parser->text;

	// Guard backward lookups at the start of the text before subtracting their offsets.
	if (start == 0 || length != 3 || text[start] != '0' || !soml__is_digit(text[start + 1]) || !soml__is_digit(text[start + 2]) || soml__byte_at(parser, start - 1) != ',') {
		return nullptr;
	}

	size_t group_end = start - 1;
	size_t number_start = soml__digits_start(text, group_end);

	// Earlier groups of three digits, as in `12,345,000`.
	while (number_start >= 2 && group_end - number_start == 3 && soml__byte_at(parser, number_start - 1) == ',' && soml__is_digit(soml__byte_at(parser, number_start - 2))) {
		group_end = number_start - 1;
		number_start = soml__digits_start(text, group_end);
	}

	int before = number_start == 0 ? -1 : soml__byte_at(parser, number_start - 1);

	// The first group has one to three digits, which are not the end of a float, of a number with underscores, or of a number with a radix prefix.
	if (number_start == group_end || before == '.' || before == '_' || number_start + 3 < group_end || soml__is_letter(before)) {
		return nullptr;
	}

	// The number with its later groups, as in `1,000,000`, is suggested with underscores or without the commas only when it is a valid int: without a leading zero, and in the 64-bit range. The sign belongs to the number, as in `-1,000`.
	size_t number_end = start + 3;

	while (number_end + 4 <= parser->length && text[number_end] == ',' && soml__has_digits(text, number_end + 1, 3) && !soml__is_digit(soml__byte_at(parser, number_end + 4))) {
		number_end += 4;
	}

	uint64_t limit = before == '-' ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
	uint64_t magnitude = 0;
	bool is_valid = text[number_start] != '0';

	for (size_t index = number_start; is_valid && index < number_end; index++) {
		if (text[index] != ',') {
			is_valid = !ckd_mul(&magnitude, magnitude, 10) && !ckd_add(&magnitude, magnitude, (uint64_t)(text[index] - '0')) && magnitude <= limit;
		}
	}

	return is_valid ? "Leading zeros are not allowed in a decimal number. A comma separates items, so a number with commas between its digits is not one number. Write it with underscores, as in 1_000, or without the commas" : "Leading zeros are not allowed in a decimal number. A comma separates items, so a number with commas between its digits is not one number";
}

/*
A number, an instant, or a duration: one maximal run of number characters, classified as a whole.
*/
[[nodiscard]] SOML__NOINLINE static bool soml__parse_number_like(soml__parser *parser, soml_value *value) {
	size_t start = parser->position;
	size_t end = start + 1;

	while (end < parser->length && soml__is_number_character(parser->text[end])) {
		end++;
	}

	const unsigned char *text = parser->text + start;
	size_t length = end - start;
	parser->position = end;

	if (length >= 10 && soml__has_digits(text, 0, 4) && text[4] == '-' && soml__has_digits(text, 5, 2) && text[7] == '-' && soml__has_digits(text, 8, 2)) {
		value->type = SOML_TYPE_INSTANT;
		return soml__parse_instant(parser, text, length, start, &value->as.instant);
	}

	soml__number_kind kind = soml__classify_number(text, length);

	switch (kind) {
		case soml__number_decimal:
		case soml__number_radix: {
			if (length == 2 && text[0] == '-' && text[1] == '0') {
				return soml__fail(parser, start, "“-0” is not allowed, because zero has one spelling: 0");
			}

			value->type = SOML_TYPE_INT;
			return soml__parse_integer(parser, text, length, start, &value->as.integer);
		}

		case soml__number_float: {
			value->type = SOML_TYPE_FLOAT;
			return soml__parse_float(parser, text, length, start, &value->as.number);
		}

		case soml__number_none: {
			if (soml__is_duration_like(text, length)) {
				value->type = SOML_TYPE_DURATION;
				return soml__parse_duration(parser, text, length, start, &value->as.duration);
			}

			const char *message = soml__describe_thousands_separator(parser, start, length);
			return soml__fail(parser, start, message != nullptr ? message : soml__describe_bad_number(text, length, soml__unquoted_text_end(parser, start) == end));
		}
	}

	unreachable();
}

// MARK: Structure

/*
Note: Not using `__builtin_inf()` because Apple Clang rejects it under `-ffast-math` with warnings treated as errors. Positive infinity is constructed from its bits, so that the library needs no `<math.h>`.
*/
[[nodiscard]] static double soml__infinity(void) {
	double value;
	constexpr uint64_t bits = soml__float_infinity_bits;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

[[nodiscard]] static bool soml__parse_value(soml__parser *parser, size_t depth, soml_value **output);

#define SOML__IS_KEYWORD(parser, literal) soml__is_keyword(parser, literal, sizeof(literal) - 1)

[[nodiscard]] static bool soml__is_keyword(soml__parser *parser, const char *word, size_t length) {
	if (parser->length - parser->position < length || memcmp(parser->text + parser->position, word, length) != 0) {
		return false;
	}

	// A longer word, such as `nullable`, is not the keyword.
	if (soml__is_bare_key_character(soml__peek(parser, length))) {
		return false;
	}

	parser->position += length;
	return true;
}

/*
A message for the word from `start` to `end` where a value should be, an unquoted string or a misspelled keyword. A keyword hint is only for the word on its own: in `Yes please` or `Nan Goldin`, following it would leave text behind or change the value. The example of a quoted string is left out when the unquoted text holds a `'`, which a `'...'` string cannot hold.
*/
[[nodiscard]] static const char *soml__describe_unknown_word(const soml__parser *parser, size_t start, size_t end) {
	static const soml_string booleans[] = {{"true", 4}, {"false", 5}, {"yes", 3}, {"no", 2}, {"on", 2}, {"off", 3}};
	static const soml_string nulls[] = {{"null", 4}, {"nil", 3}, {"none", 4}, {"undefined", 9}};
	const unsigned char *word = parser->text + start;
	size_t length = end - start;
	size_t text_end = soml__unquoted_text_end(parser, start);

	if (text_end == end) {
		for (size_t index = 0; index < sizeof(booleans) / sizeof(booleans[0]); index++) {
			if (soml__equals_ignoring_case(word, length, booleans[index])) {
				return "That is not a value. Booleans are written true and false, in lowercase";
			}
		}

		for (size_t index = 0; index < sizeof(nulls) / sizeof(nulls[0]); index++) {
			if (soml__equals_ignoring_case(word, length, nulls[index])) {
				return "That is not a value. Null is written null, in lowercase";
			}
		}

		if (soml__equals_ignoring_case(word, length, (soml_string){"nan", 3})) {
			return "NaN is not representable. Use null for a missing value";
		}

		if (soml__equals_ignoring_case(word, length, (soml_string){"inf", 3}) || soml__equals_ignoring_case(word, length, (soml_string){"infinity", 8})) {
			return "That is not a value. Infinity is written infinity, in lowercase";
		}
	}

	return soml__find_byte(parser->text, text_end, start, '\'') < text_end ? "Unexpected word. A string value must be quoted" : "Unexpected word. A string value must be quoted, as in 'text'";
}

static const char soml__table_header_message[] = "There are no table headers. Write the table as an object, as in name: {…}";

/*
Whether the line of `offset` is a TOML table header, as in `[server]` or `[[servers]]` on a line of its own, which reads as an array that holds a word, or as a key that starts with "[". Its name is a valid key, so that the suggestion is valid. Where a value is expected, `is_value`, it is only a table header when its bracket opens the document, because a table cannot be written as an item of an array inside it. Like the reference implementation, this looks at the first 1,000 characters of the line only.
*/
[[nodiscard]] SOML__NOINLINE static bool soml__is_table_header(const soml__parser *parser, size_t offset, bool is_value) {
	const unsigned char *text = parser->text;
	size_t line_start = offset;

	while (line_start > 0 && text[line_start - 1] != '\n') {
		line_start--;
	}

	size_t index = soml__skip_blanks(text, line_start, parser->length);

	if (is_value && index != parser->document_start) {
		return false;
	}

	size_t line_end = soml__find_byte(text, parser->length, offset, '\n');
	size_t end = line_end - line_start > 1000 ? line_start + 1000 : line_end;

	if (index >= end || text[index] != '[') {
		return false;
	}

	index += index + 1 < end && text[index + 1] == '[' ? 2 : 1;

	if (index >= end || !(soml__is_letter(text[index]) || text[index] == '_')) {
		return false;
	}

	index = soml__skip_bare_key(text, end, index);

	if (index >= end || text[index] != ']') {
		return false;
	}

	index += index + 1 < end && text[index + 1] == ']' ? 2 : 1;
	return soml__skip_blanks(text, index, end) == end;
}

/*
Whether a YAML literal block scalar indicator, as in `key: |` or `key: |-`, is at `offset`, at the end of its line. A folded block scalar, `>`, joins its lines, which a block string does not, so it is not one.
*/
[[nodiscard]] static bool soml__is_block_scalar_indicator(const soml__parser *parser, size_t offset) {
	if (soml__byte_at(parser, offset) != '|') {
		return false;
	}

	int chomping = soml__byte_at(parser, offset + 1);
	size_t end = soml__skip_blanks(parser->text, chomping == '-' || chomping == '+' ? offset + 2 : offset + 1, parser->length);
	int next = soml__byte_at(parser, end);
	return next == '\n' || next == -1;
}

[[nodiscard]] static bool soml__fail_unexpected_value(soml__parser *parser) {
	size_t start = parser->position;

	if (soml__is_at_end(parser)) {
		return soml__fail(parser, start, "Expected a value, but reached the end of the document");
	}

	int character = soml__peek(parser, 0);

	if (character == '+') {
		return soml__fail(parser, start, "A “+” sign is not allowed. A number without a sign is positive");
	}

	// A `.` that no digit follows begins a string, such as `.env` or `./foo`, rather than a number.
	if (character == '.') {
		return soml__fail(parser, start, soml__is_digit(soml__peek(parser, 1)) ? "A number cannot begin with “.”; write a digit before it, as in 0.5" : soml__describe_unknown_word(parser, start, start + 1));
	}

	size_t end = soml__skip_bare_key(parser->text, parser->length, start);

	if (end == start) {
		if (character == '\n') {
			return soml__fail(parser, start, "Expected a value, but found a line break");
		}

		if (soml__is_block_scalar_indicator(parser, start)) {
			return soml__fail(parser, start, "Expected a value, but found “|”. Write a multiline string as a block string, between ''' lines");
		}

		return soml__fail(parser, start, SOML__WITH_HINTS(parser, start, "Expected a value"));
	}

	// A key where a value should be, as when the value of an entry is left out and the next line has a key, or as in `[a: 1]`, is reported as a key. A word after the `:` of a member and a space, as in `msg: Error: file not found` or `url: https://example.com`, is that member's value instead, an unquoted string. Without the space, as in `a:b: 1`, the first `:` was most likely meant as part of the key.
	size_t spaces_start = start;

	while (spaces_start > 0 && (parser->text[spaces_start - 1] == ' ' || parser->text[spaces_start - 1] == '\t')) {
		spaces_start--;
	}

	// Leading whitespace has no preceding colon, so do not look before the start of the text.
	bool is_member_value = spaces_start > 0 && spaces_start < start && soml__byte_at(parser, spaces_start - 1) == ':';

	if (!is_member_value && soml__byte_at(parser, end) == ':') {
		return soml__fail(parser, start, "Expected a value, but found a key. Was a value left out?");
	}

	if (soml__is_table_header(parser, start, true)) {
		return soml__fail(parser, start, soml__table_header_message);
	}

	return soml__fail(parser, start, soml__describe_unknown_word(parser, start, end));
}

#define SOML__KEY_QUOTING_HINT ". A key that contains characters other than letters, digits, “_”, and “-” must be quoted"

/*
Whether the character at the current position cannot be in a bare key but is likely meant as part of one, such as the `$` in `$schema` or the `é` in `é: 1`, so an error there can say that such a key is quoted. A character with a meaning in the grammar, whitespace, U+200B and U+FEFF, which look like whitespace, and a C1 control are not. Neither is a `=`, which after a key, as in `name=foo`, was most likely meant as the `:` of INI and TOML. Without Unicode property tables, another invisible character, such as U+00AD, gets the hint too, which is still true of it.
*/
[[nodiscard]] static bool soml__is_quotable_key_character(const soml__parser *parser) {
	int character = soml__peek(parser, 0);

	if (character < 0x80) {
		return character > ' ' && character < 0x7F && !soml__is_bare_key_character(character) && strchr("\"#'*,/:=[]{}", character) == nullptr;
	}

	uint32_t scalar = soml__scalar_at(parser, parser->position);
	return scalar >= 0xA0 && !soml__is_lookalike_space(scalar);
}

/*
`key = bare-key / literal / escaped`.
*/
[[nodiscard]] static bool soml__parse_key(soml__parser *parser, soml_string *output) {
	size_t start = parser->position;
	int character = soml__peek(parser, 0);

	if (character == '\'' || character == '"') {
		if (soml__peek(parser, 1) == character && soml__peek(parser, 2) == character) {
			return soml__fail(parser, start, "A block string cannot be a key");
		}

		// A key is a string, so it may hold anything a string can, including a line feed written as an escape.
		return soml__parse_string(parser, output);
	}

	size_t end = soml__skip_bare_key(parser->text, parser->length, start);

	if (end == start) {
		if (character == '[' && soml__is_table_header(parser, start, false)) {
			return soml__fail(parser, start, soml__table_header_message);
		}

		if (soml__is_quotable_key_character(parser)) {
			return soml__fail(parser, start, "Expected a key" SOML__KEY_QUOTING_HINT);
		}

		return soml__fail(parser, start, soml__is_at_end(parser) ? "Expected a key, but reached the end of the document" : SOML__WITH_HINTS(parser, start, "Expected a key"));
	}

	if (!soml__store_string(parser, parser->text + start, end - start, output)) {
		return false;
	}

	parser->position = end;
	return true;
}

/*
A `.` directly after a key, as in `example.com: 1`. A key is never a path, so the `.` is meant either as part of the key or as nesting. As in the reference implementation, the example of quoting and nesting is only given for a bare key whose dots each have a word on both sides and that a `:` follows, so that both are valid, and it looks at 1,000 characters of the key at most.
*/
[[nodiscard]] static bool soml__fail_dot_in_key(soml__parser *parser, size_t key_start) {
	size_t end = parser->position;
	bool has_empty_word = false;

	while (end < key_start + 1000 && (soml__is_bare_key_character(soml__byte_at(parser, end)) || soml__byte_at(parser, end) == '.')) {
		has_empty_word = has_empty_word || (parser->text[end] == '.' && parser->text[end - 1] == '.');
		end++;
	}

	has_empty_word = has_empty_word || parser->text[end - 1] == '.';

	if (soml__byte_at(parser, end) != ':' || !soml__is_bare_key_character(parser->text[key_start]) || has_empty_word) {
		return soml__fail(parser, parser->position, "A key cannot contain “.” unless it is quoted. Quote the whole key, or use braces to nest, as in a: {b: …}");
	}

	return soml__fail(parser, parser->position, "A bare key cannot contain “.”. Quote it, as in 'example.com', or use braces to nest, as in example: {com: …}");
}

[[nodiscard]] static bool soml__fail_missing_colon(soml__parser *parser, size_t key_start) {
	size_t next = soml__skip_blanks(parser->text, parser->position, parser->length);

	if (next > parser->position && soml__byte_at(parser, next) == ':') {
		return soml__fail(parser, parser->position, "Whitespace is not allowed between a key and its “:”");
	}

	// A block comment followed by the `:` was meant to come before it.
	if (soml__byte_at(parser, next) == '/' && soml__byte_at(parser, next + 1) == '*') {
		size_t comment_end = soml__find_pair(parser->text, parser->length, next + 2, '*', '/');

		if (comment_end < parser->length && soml__byte_at(parser, soml__skip_blanks(parser->text, comment_end + 2, parser->length)) == ':') {
			return soml__fail(parser, next, "A comment is not allowed between a key and its “:”");
		}
	}

	// A key with a space in it, such as `the name: 1`. The hint is only given for plain words, because quoting a quoted key would change what it means, and for a `:` that whitespace or the end follows, because quoting the words before the `:` in `server localhost:8080` would give a valid document with another meaning. So `the name:1` gets no hint.
	if (next > parser->position && soml__is_bare_key_character(soml__byte_at(parser, next))) {
		size_t line_end = soml__find_byte(parser->text, parser->length, next, '\n');
		size_t colon = soml__find_byte(parser->text, line_end, next, ':');
		int after_colon = soml__byte_at(parser, colon + 1);

		if (colon < line_end && (after_colon == -1 || after_colon == '\n' || after_colon == ' ' || after_colon == '\t')) {
			bool is_words_with_spaces = true;

			for (size_t index = key_start; index < colon; index++) {
				int character = parser->text[index];

				if (character != ' ' && character != '\t' && !soml__is_bare_key_character(character)) {
					is_words_with_spaces = false;
					break;
				}
			}

			if (is_words_with_spaces) {
				return soml__fail(parser, key_start, "A bare key cannot contain spaces. Quote it, as in 'the name'");
			}
		}
	}

	if (soml__is_at_end(parser) || soml__peek(parser, 0) == '\n') {
		return soml__fail(parser, parser->position, "Expected “:” after the key");
	}

	// A character directly after a bare key is most likely meant to be part of it. A quoted key is quoted already.
	int previous = parser->text[parser->position - 1];

	if (next == parser->position && previous != '\'' && previous != '"' && soml__is_quotable_key_character(parser)) {
		return soml__fail(parser, next, "Expected “:” after the key" SOML__KEY_QUOTING_HINT);
	}

	return soml__fail(parser, next, SOML__WITH_SPACE_HINT(parser, next, "Expected “:” after the key"));
}

[[nodiscard]] static bool soml__push_entry(soml__parser *parser, soml__entry entry) {
	size_t needed;
	soml__entry *entries = ckd_add(&needed, parser->entry_count, 1) ? nullptr : soml__grow(&parser->document->allocator, parser->entries, &parser->entry_capacity, sizeof(*entries), needed);

	if (entries == nullptr) {
		return soml__fail_out_of_memory(parser);
	}

	parser->entries = entries;
	parser->entries[parser->entry_count++] = entry;
	return true;
}

/*
The number of spaces and tabs before `offset` on its line, or `SIZE_MAX` when something else comes before it.
*/
[[nodiscard]] static size_t soml__indentation_at(const soml__parser *parser, size_t offset) {
	size_t line_start = offset;

	while (line_start > 0 && parser->text[line_start - 1] != '\n') {
		line_start--;
	}

	return soml__skip_blanks(parser->text, line_start, offset) == offset ? offset - line_start : SIZE_MAX;
}

#define SOML__COMMENT_AS_VALUE_HINT ". “#” starts a comment, so a value that starts with “#” must be quoted"

/*
A member whose value was left out, as in `a:` followed by `'b': 1` on the next line, reads the next key as its value, which fails after the key or inside it. That failure is reported as what it is, at the key, as in the reference implementation, and so is a YAML block sequence. The entry's key starts at `key_start`, and `has_comment_hint` is whether its value was meant to start with `#`. Returns whether the failure is reported.
*/
[[nodiscard]] static bool soml__diagnose_missing_value(soml__parser *parser, size_t start, size_t key_start, bool has_comment_hint) {
	int first = soml__byte_at(parser, start);

	if (first == '-' && (soml__byte_at(parser, start + 1) == ' ' || soml__byte_at(parser, start + 1) == '\t')) {
		soml__fail(parser, start, "Expected a value, but found a “-” list. An array is written in brackets, as in [80, 443]");
		return true;
	}

	soml_string key;
	parser->position = start;
	// A key that fails replaces the error, which `soml__diagnose_bad_value` restores, or replaces with its own message, when this returns `false`.
	bool is_key = soml__parse_key(parser, &key) && soml__peek(parser, 0) == ':';

	// Running out of memory is reported as it is, as everywhere else.
	if (parser->error.code == SOML_ERROR_OUT_OF_MEMORY) {
		return true;
	}

	// Whitespace or the end follows the `:` of a bare key. A digit follows the `:` in an instant such as `2026-09-19T25:00:00Z`, whose own error is more precise. A quoted key cannot be part of a value, so anything may follow its `:`.
	int next = soml__peek(parser, 1);

	if (!is_key || !(first == '\'' || first == '"' || next == -1 || next == '\n' || next == ' ' || next == '\t')) {
		return false;
	}

	// A key indented under the entry whose value is missing, as YAML nests an object.
	size_t key_indentation = soml__indentation_at(parser, key_start);
	size_t indentation = soml__indentation_at(parser, start);
	const char *message = "Expected a value, but found a key. Was a value left out?";

	if (has_comment_hint) {
		message = "Expected a value, but found a key" SOML__COMMENT_AS_VALUE_HINT;
	} else if (key_indentation != SIZE_MAX && indentation != SIZE_MAX && indentation > key_indentation) {
		message = "Expected a value, but found a key. Indentation does not nest objects, so write the key in braces, as in server: {port: …}";
	}

	soml__fail(parser, start, message);
	return true;
}

/*
Whether a key that starts at `key_start` is a bare key followed directly by `:` and more of a key, up to a `:` that whitespace or the end follows, as in `a:b: 1`.
*/
[[nodiscard]] static bool soml__is_key_with_colon(const soml__parser *parser, size_t key_start) {
	// Like the other diagnoses, it looks at 1,000 characters at most.
	for (size_t index = key_start; index < key_start + 1000; index++) {
		int character = soml__byte_at(parser, index);

		if (character == ':') {
			int next = soml__byte_at(parser, index + 1);

			if (next == -1 || next == '\n' || next == ' ' || next == '\t') {
				return true;
			}
		} else if (!soml__is_bare_key_character(character)) {
			return false;
		}
	}

	return false;
}

/*
A value that failed to parse, in an entry whose key starts at `key_start` and whose `:` is at `colon`, is reported as what the entry was meant to be when that is clear, as in the reference implementation. Otherwise, the original error stands. It is kept out of the recursive parser, as the functions with large locals are, so that it does not make every level's frame larger.
*/
SOML__NOINLINE static void soml__diagnose_bad_value(soml__parser *parser, size_t key_start, size_t colon) {
	if (parser->error.code != SOML_ERROR_SYNTAX) {
		return;
	}

	// A key that contains a `:`, as in `12:30: 'lunch'`, ends at the first `:`, and the rest is read as the value.
	if (soml__is_bare_key_character(soml__byte_at(parser, colon + 1)) && soml__is_key_with_colon(parser, key_start)) {
		soml__fail(parser, key_start, "A key that contains “:” must be quoted");
		return;
	}

	auto error = parser->error;
	parser->position = colon + 1;
	bool is_value_on_next_line = false;

	// The trivia was skipped once already, so this cannot fail.
	if (!soml__skip_trivia(parser, &is_value_on_next_line)) {
		parser->error = error;
		return;
	}

	// A `#` directly after the `:`, as in `color: #FFF`, starts a comment rather than a value. A `#` followed by a space, or by another `#`, starts an ordinary comment, and a separator or a closing bracket is not part of a value.
	size_t value_start = parser->position;
	size_t hash = soml__skip_blanks(parser->text, colon + 1, parser->length);
	int after_hash = soml__byte_at(parser, hash + 1);
	bool has_comment_hint = soml__byte_at(parser, hash) == '#' && after_hash != -1 && strchr("\t\n #,]}", after_hash) == nullptr;

	if (is_value_on_next_line && soml__diagnose_missing_value(parser, value_start, key_start, has_comment_hint)) {
		return;
	}

	// A value that was left out at the end of the document or of an object.
	int first = soml__byte_at(parser, value_start);

	if (has_comment_hint && error.offset == value_start && (first == '}' || first == -1)) {
		soml__fail(parser, value_start, first == -1 ? "Expected a value, but reached the end of the document" SOML__COMMENT_AS_VALUE_HINT : "Expected a value" SOML__COMMENT_AS_VALUE_HINT);
		return;
	}

	parser->error = error;
}

/*
`entry = key ":" ws value`. The entry is pushed on the entry stack, and its object is built when the object closes.
*/
[[nodiscard]] static bool soml__parse_entry(soml__parser *parser, size_t depth) {
	size_t key_start = parser->position;
	soml_string key;

	if (!soml__parse_key(parser, &key)) {
		return false;
	}

	if (soml__peek(parser, 0) == '.') {
		return soml__fail_dot_in_key(parser, key_start);
	}

	if (soml__peek(parser, 0) != ':') {
		return soml__fail_missing_colon(parser, key_start);
	}

	size_t colon = parser->position;
	parser->position++;

	if (!soml__skip_trivia(parser, nullptr)) {
		return false;
	}

	soml_value *value;

	if (!soml__parse_value(parser, depth + 1, &value)) {
		soml__diagnose_bad_value(parser, key_start, colon);
		return false;
	}

	return soml__push_entry(parser, (soml__entry){
		.key = key,
		.value = value,
		.offset = key_start,
	});
}

[[nodiscard]] static int soml__compare_keys(const void *context, size_t left, size_t right) {
	const soml__entry *entries = context;
	return soml__compare_strings(entries[left].key, entries[right].key);
}

/*
Sorts the entries pushed since `entry_base` by key, in a block the parser keeps from one object to the next, and checks that no two keys are equal. The first duplicate in the source is reported, as a reader that checks each entry as it reads would report it. An already ordered range returns nullptr for its implicit identity order; otherwise the block holds the sort order. Returns `false` for a duplicate or when the allocator fails.
*/
[[nodiscard]] SOML__NOINLINE static bool soml__check_duplicates(soml__parser *parser, size_t entry_base, size_t **order_output) {
	size_t count = parser->entry_count - entry_base;
	const soml__entry *entries = parser->entries + entry_base;
	bool is_sorted = true;

	for (size_t index = 1; index < count; index++) {
		int comparison = soml__compare_strings(entries[index - 1].key, entries[index].key);

		if (comparison == 0) {
			return soml__fail(parser, entries[index].offset, "Duplicate key");
		}

		if (comparison > 0) {
			is_sorted = false;
			break;
		}
	}

	if (is_sorted) {
		*order_output = nullptr;
		return true;
	}

	size_t size;
	unsigned char *scratch = ckd_mul(&size, count, 2 * sizeof(size_t)) ? nullptr : soml__grow(&parser->document->allocator, parser->scratch, &parser->scratch_capacity, 1, size);

	if (scratch == nullptr) {
		return soml__fail_out_of_memory(parser);
	}

	parser->scratch = scratch;
	// The block comes from the allocator, which returns memory aligned for any type, so it can hold indexes.
	size_t *order = (size_t *)scratch;
	*order_output = order;

	for (size_t index = 0; index < count; index++) {
		order[index] = index;
	}

	// The sort is stable and the entries are in source order, so equal keys are next to each other in source order, and the second of them is the duplicate.
	soml__merge_sort(order, order + count, count, soml__compare_keys, entries);
	size_t duplicate_offset = SIZE_MAX;

	for (size_t index = 1; index < count; index++) {
		const soml__entry *entry = &entries[order[index]];

		if (entry->offset < duplicate_offset && soml__compare_strings(entries[order[index - 1]].key, entry->key) == 0) {
			duplicate_offset = entry->offset;
		}
	}

	return duplicate_offset == SIZE_MAX || soml__fail(parser, duplicate_offset, "Duplicate key");
}

/*
Builds the object for `count` entries starting at `entry_base`. Members stay in source order; `order` lists them in key order, or is nullptr when source order is key order.
*/
[[nodiscard]] static bool soml__build_object(soml__parser *parser, size_t entry_base, const size_t *order, size_t count, soml_value **output) {
	soml_document *document = parser->document;
	soml_value *object = soml__new_value(document, (soml_value){.type = SOML_TYPE_OBJECT, .is_sorted = true});

	if (object == nullptr) {
		return soml__fail_out_of_memory(parser);
	}

	*output = object;

	if (count == 0) {
		return true;
	}

	// An empty object may have no entries buffer, so form this pointer only after the empty check.
	const soml__entry *entries = parser->entries + entry_base;
	soml__member *members = soml__allocate_array(document, count, sizeof(*members));
	size_t *sorted = order == nullptr ? nullptr : soml__allocate_array(document, count, sizeof(*sorted));

	if (members == nullptr || (order != nullptr && sorted == nullptr)) {
		return soml__fail_out_of_memory(parser);
	}

	for (size_t index = 0; index < count; index++) {
		members[index] = (soml__member){.key = entries[index].key, .value = entries[index].value};
		entries[index].value->is_attached = true;
	}

	if (order != nullptr) {
		memcpy(sorted, order, count * sizeof(*sorted));
	}

	object->as.object.members = members;
	object->as.object.sorted = sorted;
	object->as.object.count = count;
	object->as.object.capacity = count;
	return true;
}

/*
Builds the object from the entries pushed since `entry_base`, after checking for duplicate keys.
*/
[[nodiscard]] SOML__NOINLINE static bool soml__finish_object(soml__parser *parser, size_t entry_base, soml_value **output) {
	size_t count = parser->entry_count - entry_base;
	size_t *order = nullptr;
	bool is_success = count < 2 || soml__check_duplicates(parser, entry_base, &order);

	if (is_success) {
		is_success = soml__build_object(parser, entry_base, order, count, output);
	}

	parser->entry_count = entry_base;
	return is_success;
}

/*
An object that fails before it closes has entries that were not checked for duplicates yet. They all come before the failure in the text, so a duplicate among them is the first error, and is reported instead, as a reader that checks each entry as it reads would report it. Each open object checks its own entries as the failure passes through it, from the innermost out, so the outermost duplicate wins, because its entries come before the object that holds the failure. Running out of memory is reported as it is, also in the check, because without the check the failure may not be the first error. Returns `false`.
*/
SOML__NOINLINE static bool soml__fail_open_object(soml__parser *parser, size_t entry_base) {
	if (parser->error.code != SOML_ERROR_OUT_OF_MEMORY && parser->entry_count - entry_base > 1) {
		auto error = parser->error;
		size_t *order;

		// Without a duplicate, the failure stands.
		if (soml__check_duplicates(parser, entry_base, &order)) {
			parser->error = error;
		}
	}

	parser->entry_count = entry_base;
	return false;
}

/*
After a member or an item: a comma, a line break, both, the closing bracket, or an error. A comma must be on the line of the item before it. `start` is where the container opened, where an unterminated one is reported.
*/
[[nodiscard]] static bool soml__skip_separator(soml__parser *parser, int closing, size_t start) {
	size_t item_end = parser->position;
	bool has_crossed_line_break;

	if (!soml__skip_trivia(parser, &has_crossed_line_break)) {
		return false;
	}

	int character = soml__peek(parser, 0);

	if (character == ',') {
		if (has_crossed_line_break) {
			return soml__fail(parser, parser->position, "A comma must be on the same line as the item before it. The line break already separates the items, so remove the comma");
		}

		parser->position++;
		return soml__skip_trivia(parser, nullptr);
	}

	// After a line break, the caller reports the end of the text as an unterminated container.
	if (character == closing || has_crossed_line_break) {
		return true;
	}

	bool is_object = closing == '}';

	if (character == -1) {
		return soml__fail(parser, start, is_object ? "Unterminated object: expected “}”" : "Unterminated array: expected “]”");
	}

	// Without a line break that separates, a line break in the gap is inside a block comment.
	if (soml__find_byte(parser->text, parser->position, item_end, '\n') < parser->position) {
		return soml__fail(parser, parser->position, is_object ? SOML__WITH_SPACE_HINT(parser, parser->position, "Expected “,”, a line break, or “}” after an object member. A line break inside a block comment does not separate items") : SOML__WITH_SPACE_HINT(parser, parser->position, "Expected “,”, a line break, or “]” after an array item. A line break inside a block comment does not separate items"));
	}

	return soml__fail(parser, parser->position, is_object ? SOML__WITH_HINTS(parser, parser->position, "Expected “,”, a line break, or “}” after an object member") : SOML__WITH_HINTS(parser, parser->position, "Expected “,”, a line break, or “]” after an array item"));
}

[[nodiscard]] static bool soml__parse_object(soml__parser *parser, size_t depth, soml_value **output) {
	if (depth > soml__maximum_depth) {
		return soml__fail_with_code(parser, SOML_ERROR_TOO_DEEP, parser->position, soml__too_deep_message);
	}

	size_t start = parser->position;
	size_t entry_base = parser->entry_count;
	parser->position++;

	if (!soml__skip_trivia(parser, nullptr)) {
		return false;
	}

	for (;;) {
		if (soml__peek(parser, 0) == '}') {
			parser->position++;
			return soml__finish_object(parser, entry_base, output);
		}

		if (soml__is_at_end(parser)) {
			soml__fail(parser, start, "Unterminated object: expected “}”");
			soml__fail_open_object(parser, entry_base);
			return false;
		}

		if (!soml__parse_entry(parser, depth) || !soml__skip_separator(parser, '}', start)) {
			soml__fail_open_object(parser, entry_base);
			return false;
		}
	}
}

[[nodiscard]] static bool soml__push_item(soml__parser *parser, soml_value *item) {
	size_t needed;
	soml_value **items = ckd_add(&needed, parser->item_count, 1) ? nullptr : soml__grow(&parser->document->allocator, parser->items, &parser->item_capacity, sizeof(*items), needed);

	if (items == nullptr) {
		return soml__fail_out_of_memory(parser);
	}

	parser->items = items;
	parser->items[parser->item_count++] = item;
	return true;
}

[[nodiscard]] static bool soml__finish_array(soml__parser *parser, size_t item_base, soml_value **output) {
	size_t count = parser->item_count - item_base;
	soml_value *array = soml__new_value(parser->document, (soml_value){.type = SOML_TYPE_ARRAY});

	if (array == nullptr) {
		return soml__fail_out_of_memory(parser);
	}

	if (count > 0) {
		soml_value **items = soml__allocate_array(parser->document, count, sizeof(*items));

		if (items == nullptr) {
			return soml__fail_out_of_memory(parser);
		}

		memcpy(items, parser->items + item_base, count * sizeof(*items));

		for (size_t index = 0; index < count; index++) {
			items[index]->is_attached = true;
		}

		array->as.array.items = items;
		array->as.array.count = count;
		array->as.array.capacity = count;
	}

	parser->item_count = item_base;
	*output = array;
	return true;
}

[[nodiscard]] static bool soml__parse_array(soml__parser *parser, size_t depth, soml_value **output) {
	if (depth > soml__maximum_depth) {
		return soml__fail_with_code(parser, SOML_ERROR_TOO_DEEP, parser->position, soml__too_deep_message);
	}

	size_t start = parser->position;
	size_t item_base = parser->item_count;
	parser->position++;

	if (!soml__skip_trivia(parser, nullptr)) {
		return false;
	}

	for (;;) {
		if (soml__peek(parser, 0) == ']') {
			parser->position++;
			return soml__finish_array(parser, item_base, output);
		}

		if (soml__is_at_end(parser)) {
			return soml__fail(parser, start, "Unterminated array: expected “]”");
		}

		soml_value *item;

		// Unlike an object's entries, which `soml__fail_open_object` checks, the items are not read after a failure, so they are left on the stack.
		if (!soml__parse_value(parser, depth + 1, &item) || !soml__push_item(parser, item) || !soml__skip_separator(parser, ']', start)) {
			return false;
		}
	}
}

/*
A character that cannot follow the value that starts at `start`. `is_number` is whether the value is an int or a float written with digits.
*/
[[nodiscard]] SOML__NOINLINE static bool soml__fail_after_value(soml__parser *parser, size_t start, bool is_number) {
	size_t position = parser->position;
	int character = soml__byte_at(parser, position);
	int second = soml__byte_at(parser, position + 1);

	// Go writes microseconds as `µs`, with the micro sign, U+00B5, or the Greek letter mu, U+03BC.
	if (is_number && ((character == 0xC2 && second == 0xB5) || (character == 0xCE && second == 0xBC)) && soml__byte_at(parser, position + 2) == 's') {
		return soml__fail(parser, position, "The unit for microseconds is written us");
	}

	// A `''` inside a '...' string, as SQL and YAML escape a quote, ends the string.
	if (character == '\'' && soml__byte_at(parser, start) == '\'') {
		return soml__fail(parser, position, "There is no '' escape in a '...' string. Write a string that contains ' as \"...\", as in \"it's\"");
	}

	return soml__fail(parser, position, SOML__WITH_SPACE_HINT(parser, position, "Unexpected character after a value"));
}

/*
A number followed by a space and a word, such as `512 MiB` or `10 seconds`, which is an error anyway, is reported as a unit after a space. A word followed by more than spaces, a separator, or a comment is left to the general errors, because it may be a key, as in `a: 1 b: 2`, or a sentence, and a keyword after a number is a missing comma. Returns `false` when it reports one.
*/
[[nodiscard]] SOML__NOINLINE static bool soml__check_unit_after_space(soml__parser *parser) {
	size_t unit_start = soml__skip_blanks(parser->text, parser->position, parser->length);
	size_t unit_end = unit_start;

	while (unit_end < parser->length && soml__is_letter(parser->text[unit_end])) {
		unit_end++;
	}

	const unsigned char *unit = parser->text + unit_start;
	size_t length = unit_end - unit_start;
	bool is_keyword = (length == 4 && (memcmp(unit, "true", 4) == 0 || memcmp(unit, "null", 4) == 0)) || (length == 5 && memcmp(unit, "false", 5) == 0) || (length == 8 && memcmp(unit, "infinity", 8) == 0);

	if (length == 0 || !soml__is_value_end_at(parser, soml__skip_blanks(parser->text, unit_end, parser->length)) || is_keyword) {
		return true;
	}

	return soml__fail(parser, unit_start, "A unit cannot follow a number after a space. Write a duration without the space, as in 10s, and anything else, such as a size, as a string, as in '512 MiB'");
}

[[nodiscard]] static bool soml__parse_value(soml__parser *parser, size_t depth, soml_value **output) {
	int character = soml__peek(parser, 0);

	if (character == '{') {
		return soml__parse_object(parser, depth, output);
	}

	if (character == '[') {
		return soml__parse_array(parser, depth, output);
	}

	size_t start = parser->position;
	soml_value *value = soml__new_value(parser->document, (soml_value){.type = SOML_TYPE_NULL});
	// An int or a float written with digits.
	bool is_number = false;

	if (value == nullptr) {
		return soml__fail_out_of_memory(parser);
	}

	if (character == '\'' || character == '"') {
		value->type = SOML_TYPE_STRING;

		if (!soml__parse_string(parser, &value->as.string)) {
			return false;
		}
	} else if (character == 't' && SOML__IS_KEYWORD(parser, "true")) {
		value->type = SOML_TYPE_BOOL;
		value->as.boolean = true;
	} else if (character == 'f' && SOML__IS_KEYWORD(parser, "false")) {
		value->type = SOML_TYPE_BOOL;
		value->as.boolean = false;
	} else if (character == 'n' && SOML__IS_KEYWORD(parser, "null")) {
		// The default scalar type is already null.
	} else if (character == 'i' && SOML__IS_KEYWORD(parser, "infinity")) {
		value->type = SOML_TYPE_FLOAT;
		value->as.number = soml__infinity();
	} else if (character == '-' && SOML__IS_KEYWORD(parser, "-infinity")) {
		value->type = SOML_TYPE_FLOAT;
		value->as.number = -soml__infinity();
	} else if (soml__is_digit(character) || character == '-') {
		if (!soml__parse_number_like(parser, value)) {
			return false;
		}

		is_number = value->type == SOML_TYPE_INT || value->type == SOML_TYPE_FLOAT;
	} else {
		return soml__fail_unexpected_value(parser);
	}

	int next = soml__peek(parser, 0);

	if (next != -1 && !soml__is_value_end(next)) {
		return soml__fail_after_value(parser, start, is_number);
	}

	if (is_number && (next == ' ' || next == '\t') && !soml__check_unit_after_space(parser)) {
		return false;
	}

	*output = value;
	return true;
}

/*
`bare-object = entry ( entry-sep entry )*`, where a separator is one or more line breaks.
*/
[[nodiscard]] static bool soml__parse_bare_object(soml__parser *parser, soml_value **output) {
	size_t start = parser->position;

	if (!soml__parse_entry(parser, 1)) {
		// A whole document that is one scalar, such as `5` or an instant, fails as an entry. That failure is reported as what it is.
		auto error = parser->error;
		parser->position = start;
		parser->entry_count = 0;
		parser->item_count = 0;
		soml_value *value;
		bool is_bare_value = soml__parse_value(parser, 1, &value) && soml__skip_trivia(parser, nullptr) && soml__is_at_end(parser);

		// Running out of memory is reported as it is, because the entry's failure may be later in the text than the bare value.
		if (parser->error.code == SOML_ERROR_OUT_OF_MEMORY) {
			return false;
		}

		// Only the error is restored. The rest of the parser, including buffers that may have grown, is current.
		parser->error = error;

		if (is_bare_value) {
			return soml__fail(parser, start, "A bare value is not a document. A document is an object or an array, so write it as `key: value` or `[value]`");
		}

		return false;
	}

	for (;;) {
		bool has_crossed_line_break;

		if (!soml__skip_trivia(parser, &has_crossed_line_break)) {
			return soml__fail_open_object(parser, 0);
		}

		if (soml__is_at_end(parser)) {
			return soml__finish_object(parser, 0, output);
		}

		// A comma is explained wherever it is, also at the start of a line or after the last entry, where it would otherwise be a missing key.
		if (soml__peek(parser, 0) == ',') {
			soml__fail(parser, parser->position, "Top-level entries are separated by line breaks, not commas. Use braces for a one-line object");
			return soml__fail_open_object(parser, 0);
		}

		if (!has_crossed_line_break) {
			soml__fail(parser, parser->position, SOML__WITH_HINTS(parser, parser->position, "Expected a line break before the next entry"));
			return soml__fail_open_object(parser, 0);
		}

		if (!soml__parse_entry(parser, 1)) {
			return soml__fail_open_object(parser, 0);
		}
	}
}

[[nodiscard]] static bool soml__parse_document(soml__parser *parser, soml_value **output) {
	if (!soml__check_characters(parser) || !soml__skip_trivia(parser, nullptr)) {
		return false;
	}

	parser->document_start = parser->position;

	if (soml__is_at_end(parser)) {
		return soml__fail(parser, parser->position, "A document must contain an object or an array, but this one is empty");
	}

	int character = soml__peek(parser, 0);
	bool is_success;

	if (character == '{') {
		is_success = soml__parse_object(parser, 1, output);
	} else if (character == '[') {
		is_success = soml__parse_array(parser, 1, output);
	} else {
		is_success = soml__parse_bare_object(parser, output);
	}

	if (!is_success || !soml__skip_trivia(parser, nullptr)) {
		return false;
	}

	if (!soml__is_at_end(parser)) {
		return soml__fail(parser, parser->position, SOML__WITH_SPACE_HINT(parser, parser->position, "Unexpected content after the end of the document"));
	}

	return true;
}

static void soml__set_error(soml_error *error, soml_error_code code, const char *message) {
	if (error != nullptr) {
		*error = (soml_error){.code = code, .message = message};
	}
}

/*
The 1-based line, and the 1-based column in Unicode scalar values, of a byte offset in text whose bytes before the offset are valid UTF-8.
*/
static void soml__locate(const unsigned char *text, size_t length, size_t offset, size_t *line, size_t *column) {
	size_t current_line = 1;
	size_t current_column = 1;

	if (offset > length) {
		offset = length;
	}

	size_t line_start = 0;

	while (line_start < offset) {
		size_t line_end = soml__find_byte(text, offset, line_start, '\n');

		if (line_end == offset) {
			break;
		}

		current_line++;
		line_start = line_end + 1;
	}

	for (size_t index = line_start; index < offset; index++) {
		current_column += (text[index] & 0xC0) != 0x80;
	}

	*line = current_line;
	*column = current_column;
}

SOMLDEF soml_document *soml_parse(const char *text, size_t length, const soml_allocator *allocator, soml_error *error) {
	if (text == nullptr && length > 0) {
		soml__set_error(error, SOML_ERROR_INVALID_ARGUMENT, "The text is nullptr, but its length is not 0");
		return nullptr;
	}

	// Start with a modest chunk rather than reserving the size of the source, which may consist mostly of comments. Geometric growth keeps the number of chunks small.
	soml_document *document = soml__create_document(allocator);

	if (document == nullptr) {
		soml__set_error(error, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return nullptr;
	}

	// Empty text can be `nullptr`, and arithmetic on `nullptr` is undefined, even adding 0.
	soml__parser parser = {
		.text = (const unsigned char *)(text == nullptr ? "" : text),
		.length = length,
		.document = document,
	};

	soml_value *root = nullptr;
	bool is_success = soml__parse_document(&parser, &root);
	soml__release_scratch(&document->allocator, parser.items, parser.item_capacity, sizeof(*parser.items));
	soml__release_scratch(&document->allocator, parser.entries, parser.entry_capacity, sizeof(*parser.entries));
	soml__release(&document->allocator, parser.scratch, parser.scratch_capacity);

	if (!is_success) {
		if (error != nullptr) {
			*error = parser.error;
			soml__locate(parser.text, parser.length, parser.error.offset, &error->line, &error->column);
		}

		soml_document_free(document);
		return nullptr;
	}

	// `soml__parse_document` sets `root` whenever it succeeds.
	root->is_attached = true;
	document->root = root;
	soml__set_error(error, SOML_ERROR_NONE, nullptr);
	return document;
}

// MARK: Accessors

/*
The value when it has this type, and `nullptr` for `nullptr` or another type.
*/
[[nodiscard]] static const soml_value *soml__of_type(const soml_value *value, soml_type type) {
	return value != nullptr && value->type == type ? value : nullptr;
}

SOMLDEF soml_value *soml_document_root(const soml_document *document) {
	return document == nullptr ? nullptr : document->root;
}

SOMLDEF bool soml_document_get_error(const soml_document *document, soml_error *output) {
	if (document == nullptr || output == nullptr || document->failure == SOML_ERROR_NONE) {
		return false;
	}

	*output = (soml_error){.code = document->failure, .message = document->failure_message};
	return true;
}

SOMLDEF size_t soml_document_allocated_bytes(const soml_document *document) {
	if (document == nullptr) {
		return 0;
	}

	size_t total = sizeof(*document);

	for (const soml__chunk *chunk = document->chunks; chunk != nullptr; chunk = chunk->next) {
		if (ckd_add(&total, total, sizeof(*chunk) + chunk->size)) {
			return SIZE_MAX;
		}
	}

	return total;
}

SOMLDEF soml_type soml_type_of(const soml_value *value) {
	return value == nullptr ? SOML_TYPE_NONE : value->type;
}

SOMLDEF bool soml_get_string(const soml_value *value, soml_string *output) {
	const soml_value *typed = soml__of_type(value, SOML_TYPE_STRING);

	if (typed == nullptr || output == nullptr) {
		return false;
	}

	*output = typed->as.string;
	return true;
}

SOMLDEF bool soml_get_int(const soml_value *value, int64_t *output) {
	const soml_value *typed = soml__of_type(value, SOML_TYPE_INT);

	if (typed == nullptr || output == nullptr) {
		return false;
	}

	*output = typed->as.integer;
	return true;
}

SOMLDEF bool soml_get_float(const soml_value *value, double *output) {
	const soml_value *typed = soml__of_type(value, SOML_TYPE_FLOAT);

	if (typed == nullptr || output == nullptr) {
		return false;
	}

	*output = typed->as.number;
	return true;
}

SOMLDEF bool soml_get_bool(const soml_value *value, bool *output) {
	const soml_value *typed = soml__of_type(value, SOML_TYPE_BOOL);

	if (typed == nullptr || output == nullptr) {
		return false;
	}

	*output = typed->as.boolean;
	return true;
}

SOMLDEF bool soml_get_instant(const soml_value *value, soml_instant *output) {
	const soml_value *typed = soml__of_type(value, SOML_TYPE_INSTANT);

	if (typed == nullptr || output == nullptr) {
		return false;
	}

	*output = typed->as.instant;
	return true;
}

SOMLDEF bool soml_get_duration(const soml_value *value, int64_t *nanoseconds) {
	const soml_value *typed = soml__of_type(value, SOML_TYPE_DURATION);

	if (typed == nullptr || nanoseconds == nullptr) {
		return false;
	}

	*nanoseconds = typed->as.duration;
	return true;
}

SOMLDEF bool soml_is_null(const soml_value *value) {
	return soml__of_type(value, SOML_TYPE_NULL) != nullptr;
}

SOMLDEF size_t soml_count(const soml_value *value) {
	if (value == nullptr) {
		return 0;
	}

	if (value->type == SOML_TYPE_ARRAY) {
		return value->as.array.count;
	}

	return value->type == SOML_TYPE_OBJECT ? value->as.object.count : 0;
}

SOMLDEF soml_value *soml_array_at(const soml_value *value, size_t index) {
	const soml_value *array = soml__of_type(value, SOML_TYPE_ARRAY);
	return array != nullptr && index < array->as.array.count ? array->as.array.items[index] : nullptr;
}

SOMLDEF soml_array_view soml_array_items(const soml_value *value) {
	const soml_value *array = soml__of_type(value, SOML_TYPE_ARRAY);
	return array == nullptr ? (soml_array_view){} : (soml_array_view){.data = array->as.array.items, .count = array->as.array.count};
}

SOMLDEF soml_object_view soml_object_members(const soml_value *value) {
	const soml_value *object = soml__of_type(value, SOML_TYPE_OBJECT);
	return object == nullptr ? (soml_object_view){} : (soml_object_view){.data = object->as.object.members, .count = object->as.object.count};
}

/*
The source-order member index for a length-bearing key, or `SIZE_MAX`. Both lookups and object editing use the same indexed or linear search.
*/
[[nodiscard]] static size_t soml__find_member(const soml_value *object, const char *key, size_t length) {
	if (soml__of_type(object, SOML_TYPE_OBJECT) == nullptr || (key == nullptr && length > 0)) {
		return SIZE_MAX;
	}

	soml_string target = {.data = key, .length = length};
	const soml__member *members = object->as.object.members;
	const size_t *order = object->as.object.sorted;

	if (object->is_sorted) {
		size_t low = 0;
		size_t high = object->as.object.count;

		while (low < high) {
			size_t middle = low + (high - low) / 2;
			const soml__member *member = &members[order == nullptr ? middle : order[middle]];
			int result = soml__compare_strings(member->key, target);

			if (result == 0) {
				return order == nullptr ? middle : order[middle];
			}

			if (result < 0) {
				low = middle + 1;
			} else {
				high = middle;
			}
		}

		return SIZE_MAX;
	}

	for (size_t index = 0; index < object->as.object.count; index++) {
		if (members[index].key.length == length && (length == 0 || memcmp(members[index].key.data, key, length) == 0)) {
			return index;
		}
	}

	return SIZE_MAX;
}

SOMLDEF soml_value *soml_object_get_with_length(const soml_value *object, const char *key, size_t length) {
	size_t index = soml__find_member(object, key, length);
	return index == SIZE_MAX ? nullptr : object->as.object.members[index].value;
}

SOMLDEF soml_value *soml_object_get(const soml_value *object, const char *key) {
	if (key == nullptr || soml__of_type(object, SOML_TYPE_OBJECT) == nullptr) {
		return nullptr;
	}

	return soml_object_get_with_length(object, key, strlen(key));
}

/*
A member of an object, or `nullptr` when `value` is not an object or `index` is out of range.
*/
[[nodiscard]] static const soml__member *soml__member_at(const soml_value *value, size_t index) {
	const soml_value *object = soml__of_type(value, SOML_TYPE_OBJECT);
	return object != nullptr && index < object->as.object.count ? &object->as.object.members[index] : nullptr;
}

SOMLDEF soml_string soml_object_key_at(const soml_value *object, size_t index) {
	const soml__member *member = soml__member_at(object, index);
	return member != nullptr ? member->key : (soml_string){.data = nullptr, .length = 0};
}

SOMLDEF soml_value *soml_object_value_at(const soml_value *object, size_t index) {
	const soml__member *member = soml__member_at(object, index);
	return member != nullptr ? member->value : nullptr;
}

// MARK: Builder

static void soml__record_failure(soml_document *document, soml_error_code code, const char *message) {
	if (document != nullptr && document->failure == SOML_ERROR_NONE) {
		document->failure = code;
		document->failure_message = message;
	}
}

[[nodiscard]] static soml_value *soml__build_value(soml_document *document, soml_value contents) {
	if (document == nullptr) {
		return nullptr;
	}

	soml_value *value = soml__new_value(document, contents);

	if (value == nullptr) {
		soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
	}

	return value;
}

SOMLDEF soml_document *soml_document_create(const soml_allocator *allocator) {
	return soml__create_document(allocator);
}

SOMLDEF soml_value *soml_new_string(soml_document *document, const char *data, size_t length) {
	if (document == nullptr) {
		return nullptr;
	}

	if (data == nullptr && length > 0) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "A string is nullptr, but its length is not 0");
		return nullptr;
	}

	char *copy = soml__copy_bytes(document, data, length);

	if (copy == nullptr) {
		soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return nullptr;
	}

	return soml__build_value(document, (soml_value){.type = SOML_TYPE_STRING, .as.string = {.data = copy, .length = length}});
}

SOMLDEF soml_value *soml_new_int(soml_document *document, int64_t integer) {
	return soml__build_value(document, (soml_value){.type = SOML_TYPE_INT, .as.integer = integer});
}

SOMLDEF soml_value *soml_new_float(soml_document *document, double number) {
	// Note: Not using a floating-point comparison to detect negative zero because `-ffast-math` may ignore its sign. Negative zero is the same value as zero, so check the sign bit directly.
	uint64_t bits;
	memcpy(&bits, &number, sizeof(bits));
	return soml__build_value(document, (soml_value){.type = SOML_TYPE_FLOAT, .as.number = bits == soml__float_sign_bit ? 0.0 : number});
}

SOMLDEF soml_value *soml_new_bool(soml_document *document, bool boolean) {
	return soml__build_value(document, (soml_value){.type = SOML_TYPE_BOOL, .as.boolean = boolean});
}

SOMLDEF soml_value *soml_new_null(soml_document *document) {
	return soml__build_value(document, (soml_value){.type = SOML_TYPE_NULL});
}

SOMLDEF soml_value *soml_new_instant(soml_document *document, soml_instant instant) {
	return soml__build_value(document, (soml_value){.type = SOML_TYPE_INSTANT, .as.instant = instant});
}

SOMLDEF soml_value *soml_new_duration(soml_document *document, int64_t nanoseconds) {
	return soml__build_value(document, (soml_value){.type = SOML_TYPE_DURATION, .as.duration = nanoseconds});
}

SOMLDEF soml_value *soml_new_array(soml_document *document) {
	return soml__build_value(document, (soml_value){.type = SOML_TYPE_ARRAY});
}

SOMLDEF soml_value *soml_new_object(soml_document *document) {
	return soml__build_value(document, (soml_value){.type = SOML_TYPE_OBJECT, .is_sorted = true});
}

/*
Grows an array in the document to hold `needed` elements. The old block stays in the document until it is freed.
*/
[[nodiscard]] static void *soml__grow_in_document(soml_document *document, void *buffer, size_t count, size_t *capacity, size_t element_size, size_t needed) {
	if (needed <= *capacity) {
		return buffer;
	}

	size_t new_capacity;
	size_t size;

	if (!soml__next_capacity(*capacity, needed, 4, element_size, &new_capacity, &size)) {
		return nullptr;
	}

	void *grown = soml__allocate(document, size);

	if (grown != nullptr) {
		if (count > 0) {
			memcpy(grown, buffer, count * element_size);
		}

		*capacity = new_capacity;
	}

	return grown;
}

/*
Whether `value` can be put in `document`: it belongs to it, and it is not in a container or the root already. Records the failure otherwise.
*/
[[nodiscard]] static bool soml__check_attachable(soml_document *document, const soml_value *value) {
	if (value->document != document) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "A value belongs to another document");
		return false;
	}

	if (value->is_attached) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "A value is already in a container or is the root. A value can be in one place only");
		return false;
	}

	return true;
}

/*
Check a builder's container type and ownership before touching its payload. `message` names the caller when the type is wrong.
*/
[[nodiscard]] static bool soml__check_container(soml_document *document, const soml_value *container, soml_type type, const char *message) {
	if (document == nullptr) {
		return false;
	}

	if (container == nullptr) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "A builder function got nullptr for a value");
		return false;
	}

	if (container->type != type) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, message);
		return false;
	}

	if (container->document != document) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "A value belongs to another document");
		return false;
	}

	return true;
}

[[nodiscard]] static bool soml__check_builder_arguments(soml_document *document, soml_value *container, soml_type type, soml_value *value, const char *message) {
	if (document == nullptr || value == nullptr) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "A builder function got nullptr for a value");
		return false;
	}

	return soml__check_container(document, container, type, message) && soml__check_attachable(document, value);
}

/*
A key may be nullptr only when its length is zero, as with strings.
*/
[[nodiscard]] static bool soml__check_key(soml_document *document, const char *key, size_t length) {
	if (key == nullptr && length > 0) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "A key is nullptr, but its length is not 0");
		return false;
	}

	return true;
}

SOMLDEF bool soml_array_reserve(soml_document *document, soml_value *array, size_t capacity) {
	if (!soml__check_container(document, array, SOML_TYPE_ARRAY, "soml_array_reserve needs an array")) {
		return false;
	}

	auto collection = &array->as.array;

	if (capacity <= collection->capacity) {
		return true;
	}

	soml_value **items = soml__grow_in_document(document, collection->items, collection->count, &collection->capacity, sizeof(*items), capacity);

	if (items == nullptr) {
		soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return false;
	}

	collection->items = items;
	return true;
}

SOMLDEF bool soml_object_reserve(soml_document *document, soml_value *object, size_t capacity) {
	if (!soml__check_container(document, object, SOML_TYPE_OBJECT, "soml_object_reserve needs an object")) {
		return false;
	}

	auto collection = &object->as.object;

	if (capacity <= collection->capacity) {
		return true;
	}

	soml__member *members = soml__grow_in_document(document, collection->members, collection->count, &collection->capacity, sizeof(*members), capacity);

	if (members == nullptr) {
		soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return false;
	}

	collection->members = members;
	return true;
}

SOMLDEF bool soml_array_append(soml_document *document, soml_value *array, soml_value *item) {
	if (!soml__check_builder_arguments(document, array, SOML_TYPE_ARRAY, item, "soml_array_append needs an array")) {
		return false;
	}

	auto collection = &array->as.array;
	size_t capacity = collection->capacity;
	size_t needed;
	soml_value **items = ckd_add(&needed, collection->count, 1) ? nullptr : soml__grow_in_document(document, collection->items, collection->count, &capacity, sizeof(*items), needed);

	if (items == nullptr) {
		soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return false;
	}

	items[collection->count++] = item;
	item->is_attached = true;
	collection->items = items;
	collection->capacity = capacity;
	return true;
}

SOMLDEF bool soml_object_add(soml_document *document, soml_value *object, const char *key, size_t key_length, soml_value *value) {
	if (!soml__check_builder_arguments(document, object, SOML_TYPE_OBJECT, value, "soml_object_add needs an object")) {
		return false;
	}

	if (!soml__check_key(document, key, key_length)) {
		return false;
	}

	char *copy = soml__copy_bytes(document, key, key_length);
	auto collection = &object->as.object;
	size_t count = collection->count;
	// Members need capacity; a sorted index is only needed for a parsed object whose keys arrived out of order.
	size_t member_capacity = collection->capacity;
	size_t needed;
	if (copy == nullptr || ckd_add(&needed, count, 1)) {
		soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return false;
	}

	soml__member *members = soml__grow_in_document(document, collection->members, count, &member_capacity, sizeof(*members), needed);

	if (members == nullptr) {
		soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return false;
	}

	soml_string new_key = {.data = copy, .length = key_length};

	// Member order stays sorted while every new key sorts after the ones before it; a parsed index is invalidated by mutation.
	if (object->is_sorted && count > 0) {
		const size_t *order = collection->sorted;
		// Extending an existing nonidentity index would require growing it; fall back to the writer's temporary sort instead.
		object->is_sorted = order == nullptr && soml__compare_strings(members[count - 1].key, new_key) < 0;
	}

	members[count] = (soml__member){.key = new_key, .value = value};
	value->is_attached = true;
	collection->members = members;
	collection->sorted = nullptr;
	collection->capacity = member_capacity;
	collection->count = count + 1;
	return true;
}

/*
Replace a contained value only after checking the new value. Failed replacements leave both attachment states and the slot unchanged.
*/
[[nodiscard]] static bool soml__replace_value(soml_document *document, soml_value **slot, soml_value *value) {
	if (value == nullptr) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "A builder function got nullptr for a value");
		return false;
	}

	if (*slot == value) {
		return true;
	}

	if (!soml__check_attachable(document, value)) {
		return false;
	}

	(*slot)->is_attached = false;
	value->is_attached = true;
	*slot = value;
	return true;
}

SOMLDEF bool soml_array_insert(soml_document *document, soml_value *array, size_t index, soml_value *item) {
	if (!soml__check_builder_arguments(document, array, SOML_TYPE_ARRAY, item, "soml_array_insert needs an array")) {
		return false;
	}

	size_t count = array->as.array.count;

	if (index > count) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "The array insertion index is past its end");
		return false;
	}

	size_t needed;

	if (ckd_add(&needed, count, 1) || !soml_array_reserve(document, array, needed)) {
		soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return false;
	}

	soml_value **items = array->as.array.items;
	memmove(items + index + 1, items + index, (count - index) * sizeof(*items));
	items[index] = item;
	array->as.array.count = needed;
	item->is_attached = true;
	return true;
}

SOMLDEF bool soml_array_set(soml_document *document, soml_value *array, size_t index, soml_value *item) {
	if (!soml__check_container(document, array, SOML_TYPE_ARRAY, "soml_array_set needs an array")) {
		return false;
	}

	if (index >= array->as.array.count) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "The array replacement index is past its end");
		return false;
	}

	return soml__replace_value(document, &array->as.array.items[index], item);
}

/*
Remove one in-range item and return it unattached. The array's reserved capacity is unchanged.
*/
[[nodiscard]] static soml_value *soml__remove_item(soml_value *array, size_t index) {
	auto collection = &array->as.array;
	soml_value *item = collection->items[index];
	memmove(collection->items + index, collection->items + index + 1, (collection->count - index - 1) * sizeof(*collection->items));
	collection->count--;
	item->is_attached = false;
	return item;
}

SOMLDEF bool soml_array_remove(soml_document *document, soml_value *array, size_t index) {
	if (!soml__check_container(document, array, SOML_TYPE_ARRAY, "soml_array_remove needs an array")) {
		return false;
	}

	if (index >= array->as.array.count) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "The array removal index is past its end");
		return false;
	}

	(void)soml__remove_item(array, index);
	return true;
}

SOMLDEF soml_value *soml_array_pop(soml_document *document, soml_value *array) {
	if (!soml__check_container(document, array, SOML_TYPE_ARRAY, "soml_array_pop needs an array") || array->as.array.count == 0) {
		return nullptr;
	}

	return soml__remove_item(array, array->as.array.count - 1);
}

SOMLDEF bool soml_array_clear(soml_document *document, soml_value *array) {
	if (!soml__check_container(document, array, SOML_TYPE_ARRAY, "soml_array_clear needs an array")) {
		return false;
	}

	for (size_t index = 0; index < array->as.array.count; index++) {
		array->as.array.items[index]->is_attached = false;
	}

	array->as.array.count = 0;
	return true;
}

SOMLDEF bool soml_object_set(soml_document *document, soml_value *object, const char *key, size_t key_length, soml_value *value) {
	if (!soml__check_container(document, object, SOML_TYPE_OBJECT, "soml_object_set needs an object") || !soml__check_key(document, key, key_length)) {
		return false;
	}

	size_t index = soml__find_member(object, key, key_length);

	if (index == SIZE_MAX) {
		return soml_object_add(document, object, key, key_length, value);
	}

	return soml__replace_value(document, &object->as.object.members[index].value, value);
}

/*
Remove one in-range member while keeping an existing sorted index valid. Both source positions and index entries shift, so a parsed object's binary lookup remains usable.
*/
[[nodiscard]] static soml_value *soml__remove_member(soml_value *object, size_t index) {
	auto collection = &object->as.object;
	soml_value *value = collection->members[index].value;
	memmove(collection->members + index, collection->members + index + 1, (collection->count - index - 1) * sizeof(*collection->members));

	if (collection->sorted != nullptr) {
		size_t written = 0;

		for (size_t position = 0; position < collection->count; position++) {
			size_t member_index = collection->sorted[position];

			if (member_index != index) {
				collection->sorted[written++] = member_index - (member_index > index);
			}
		}
	}

	collection->count--;
	value->is_attached = false;
	return value;
}

SOMLDEF soml_value *soml_object_take(soml_document *document, soml_value *object, const char *key, size_t key_length) {
	if (!soml__check_container(document, object, SOML_TYPE_OBJECT, "soml_object_take needs an object") || !soml__check_key(document, key, key_length)) {
		return nullptr;
	}

	size_t index = soml__find_member(object, key, key_length);
	return index == SIZE_MAX ? nullptr : soml__remove_member(object, index);
}

SOMLDEF bool soml_object_remove(soml_document *document, soml_value *object, const char *key, size_t key_length) {
	if (!soml__check_container(document, object, SOML_TYPE_OBJECT, "soml_object_remove needs an object") || !soml__check_key(document, key, key_length)) {
		return false;
	}

	size_t index = soml__find_member(object, key, key_length);

	if (index != SIZE_MAX) {
		(void)soml__remove_member(object, index);
	}

	return true;
}

SOMLDEF bool soml_object_clear(soml_document *document, soml_value *object) {
	if (!soml__check_container(document, object, SOML_TYPE_OBJECT, "soml_object_clear needs an object")) {
		return false;
	}

	for (size_t index = 0; index < object->as.object.count; index++) {
		object->as.object.members[index].value->is_attached = false;
	}

	object->as.object.count = 0;
	object->as.object.sorted = nullptr;
	object->is_sorted = true;
	return true;
}

SOMLDEF bool soml_document_set_root(soml_document *document, soml_value *root) {
	if (document == nullptr) {
		return false;
	}

	if (root == nullptr) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "The root is nullptr");
		return false;
	}

	// The current root is attached, so setting it again succeeds here, before the check that refuses an attached value.
	if (root == document->root) {
		return true;
	}

	if (!soml__check_attachable(document, root)) {
		return false;
	}

	if (document->root != nullptr) {
		document->root->is_attached = false;
	}

	root->is_attached = true;
	document->root = root;
	return true;
}

// MARK: Writer

// The size of the pieces a callback gets.
constexpr size_t soml__write_chunk_size = 4096;

/*
Collects the text in `buffer`, and gives it to `write` each time the buffer is full. `soml_write` and `soml_write_canonical` use a callback that collects the pieces in one block.
*/
typedef struct soml__writer {
	const soml_allocator *allocator;
	bool (*write)(void *context, const char *bytes, size_t length);
	void *context;
	// Whether members are written sorted by key, rather than in the order of the document.
	bool is_canonical;
	soml_error_code error_code;
	const char *error_message;
	size_t length;
	char buffer[soml__write_chunk_size];
} soml__writer;

/*
Records the first error only, because a later one follows from it, and returns `false`, so that a caller can return its result.
*/
static bool soml__writer_fail(soml__writer *writer, soml_error_code code, const char *message) {
	if (writer->error_code == SOML_ERROR_NONE) {
		writer->error_code = code;
		writer->error_message = message;
	}

	return false;
}

[[nodiscard]] static bool soml__writer_send(soml__writer *writer, const void *bytes, size_t length) {
	if (length == 0 || writer->write(writer->context, bytes, length)) {
		return true;
	}

	return soml__writer_fail(writer, SOML_ERROR_WRITE, "The write callback failed");
}

[[nodiscard]] static bool soml__writer_flush(soml__writer *writer) {
	size_t length = writer->length;
	writer->length = 0;
	return soml__writer_send(writer, writer->buffer, length);
}

[[nodiscard]] static bool soml__append(soml__writer *writer, const void *bytes, size_t length) {
	if (length > sizeof(writer->buffer) - writer->length) {
		if (!soml__writer_flush(writer)) {
			return false;
		}

		// Text longer than the buffer, such as a long string, goes to the callback directly.
		if (length > sizeof(writer->buffer)) {
			return soml__writer_send(writer, bytes, length);
		}
	}

	// After a flush, `writer->length` is 0, so the bytes fit.
	if (length > 0) {
		memcpy(writer->buffer + writer->length, bytes, length);
		writer->length += length;
	}

	return true;
}

// A literal's size is known at compile time, including in unoptimized builds.
#define SOML__APPEND_LITERAL(writer, literal) soml__append(writer, literal, sizeof(literal) - 1)

[[nodiscard]] static bool soml__append_byte(soml__writer *writer, char byte) {
	if (writer->length == sizeof(writer->buffer) && !soml__writer_flush(writer)) {
		return false;
	}

	writer->buffer[writer->length++] = byte;
	return true;
}

[[nodiscard]] static bool soml__append_indentation(soml__writer *writer, size_t depth) {
	// Every supported depth fits in one append.
	static constexpr char tabs[] =
		"\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t"
		"\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t"
		"\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t"
		"\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t"
		"\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t";
	static_assert(sizeof(tabs) - 1 == soml__maximum_depth);
	return soml__append(writer, tabs, depth);
}

/*
Checks a string or key before writing its canonical quotes. One scan checks that it can be written (valid UTF-8 and no carriage return) and finds whether it needs `"..."`: for a `'`, a tab, a line feed, another C0 control, or DEL.
*/
[[nodiscard]] static bool soml__check_string(soml__writer *writer, soml_string string, bool *output) {
	const unsigned char *bytes = (const unsigned char *)string.data;
	bool needs_escapes = false;
	size_t index = 0;

	while (index < string.length) {
		// Skip eight ordinary ASCII bytes at once. Loads stay inside the string and need no alignment; special bytes still go through the checks below.
		while (string.length - index >= sizeof(uint64_t)) {
			uint64_t word;
			memcpy(&word, bytes + index, sizeof(word));
			uint64_t below_space = ~((word | soml__byte_high_bits) - soml__byte_ones * 0x20);
			uint64_t deletes = soml__equal_byte_mask(word, 0x7F);
			uint64_t quotes = soml__equal_byte_mask(word, '\'');

			if (((word | below_space | deletes | quotes) & soml__byte_high_bits) != 0) {
				break;
			}

			index += sizeof(word);
		}

		if (index == string.length) {
			break;
		}

		unsigned char byte = bytes[index];

		if (byte == '\r') {
			return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "A string or key holds a carriage return, which SOML cannot represent");
		}

		needs_escapes = needs_escapes || byte == '\'' || byte < 0x20 || byte == 0x7F;
		size_t length = soml__utf8_sequence_length(bytes + index, string.length - index, nullptr);

		if (length == 0) {
			return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "A string or key is not valid UTF-8");
		}

		index += length;
	}

	*output = needs_escapes;
	return true;
}

[[nodiscard]] static bool soml__write_string(soml__writer *writer, soml_string string) {
	bool needs_escapes;

	if (!soml__check_string(writer, string, &needs_escapes)) {
		return false;
	}

	const unsigned char *bytes = (const unsigned char *)string.data;
	size_t index;

	if (!needs_escapes) {
		return soml__append_byte(writer, '\'') && soml__append(writer, string.data, string.length) && soml__append_byte(writer, '\'');
	}

	if (!soml__append_byte(writer, '"')) {
		return false;
	}

	size_t chunk_start = 0;

	for (index = 0; index < string.length; index++) {
		unsigned char byte = bytes[index];
		char escape[8];
		size_t escape_length = 0;

		if (byte == '\\' || byte == '"') {
			escape[0] = '\\';
			escape[1] = (char)byte;
			escape_length = 2;
		} else if (byte == '\n') {
			memcpy(escape, "\\n", 2);
			escape_length = 2;
		} else if (byte == '\t') {
			memcpy(escape, "\\t", 2);
			escape_length = 2;
		} else if (byte < 0x20 || byte == 0x7F) {
			static constexpr char hex[] = "0123456789abcdef";
			memcpy(escape, "\\u{", 3);
			escape_length = 3;

			// Canonical form writes no leading zero, so a byte below 0x10 has one digit.
			if (byte >= 0x10) {
				escape[escape_length++] = hex[byte >> 4];
			}

			escape[escape_length++] = hex[byte & 0xF];
			escape[escape_length++] = '}';
		} else {
			continue;
		}

		if (!soml__append(writer, string.data + chunk_start, index - chunk_start) || !soml__append(writer, escape, escape_length)) {
			return false;
		}

		chunk_start = index + 1;
	}

	return soml__append(writer, string.data + chunk_start, string.length - chunk_start) && soml__append_byte(writer, '"');
}

[[nodiscard]] static bool soml__write_key(soml__writer *writer, soml_string key) {
	bool is_bare = key.length > 0 && soml__skip_bare_key((const unsigned char *)key.data, key.length, 0) == key.length;

	return is_bare ? soml__append(writer, key.data, key.length) : soml__write_string(writer, key);
}

[[nodiscard]] static size_t soml__format_two_digits(int64_t value, char *output) {
	output[0] = (char)('0' + value / 10);
	output[1] = (char)('0' + value % 10);
	return 2;
}

/*
Nine digits of a fraction of a second, without trailing zeros, after a `.`. Writes nothing for zero.
*/
[[nodiscard]] static size_t soml__format_fraction(int64_t nanoseconds, char *output) {
	if (nanoseconds == 0) {
		return 0;
	}

	char digits[9];

	size_t count = sizeof(digits);

	while (nanoseconds % 10 == 0) {
		nanoseconds /= 10;
		count--;
	}

	for (size_t index = count; index > 0;) {
		digits[--index] = (char)('0' + nanoseconds % 10);
		nanoseconds /= 10;
	}

	output[0] = '.';
	memcpy(output + 1, digits, count);
	return count + 1;
}

/*
The canonical UTC spelling of an in-range instant. `text` holds at least 40 bytes; the returned length excludes a terminator.
*/
[[nodiscard]] static size_t soml__format_instant(soml_instant instant, char *text) {
	int64_t days = instant.seconds / 86400;
	int64_t second_of_day = instant.seconds % 86400;

	// `/` and `%` round toward zero, so a negative remainder is made positive by moving to the day before.
	if (second_of_day < 0) {
		second_of_day += 86400;
		days--;
	}

	int64_t year;
	int64_t month;
	int64_t day;
	soml__civil_from_days(days, &year, &month, &day);
	size_t length = 0;
	length += soml__format_two_digits(year / 100, text + length);
	length += soml__format_two_digits(year % 100, text + length);
	text[length++] = '-';
	length += soml__format_two_digits(month, text + length);
	text[length++] = '-';
	length += soml__format_two_digits(day, text + length);
	text[length++] = 'T';
	length += soml__format_two_digits(second_of_day / 3600, text + length);
	text[length++] = ':';
	length += soml__format_two_digits(second_of_day / 60 % 60, text + length);
	text[length++] = ':';
	length += soml__format_two_digits(second_of_day % 60, text + length);
	length += soml__format_fraction(instant.nanoseconds, text + length);
	text[length++] = 'Z';
	return length;
}

[[nodiscard]] static bool soml__check_instant(soml__writer *writer, soml_instant instant) {
	if (instant.nanoseconds < 0 || instant.nanoseconds > 999999999 || instant.seconds < soml__minimum_instant_seconds || instant.seconds > soml__maximum_instant_seconds) {
		return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "An instant is outside the years 0001 to 9999, or its nanoseconds are outside 0 to 999,999,999");
	}

	return true;
}

[[nodiscard]] static bool soml__write_instant(soml__writer *writer, soml_instant instant) {
	if (!soml__check_instant(writer, instant)) {
		return false;
	}

	char text[40];
	return soml__append(writer, text, soml__format_instant(instant, text));
}

[[nodiscard]] static bool soml__write_duration(soml__writer *writer, int64_t nanoseconds) {
	if (nanoseconds == 0) {
		return SOML__APPEND_LITERAL(writer, "0s");
	}

	uint64_t magnitude = soml__magnitude(nanoseconds);
	uint64_t hours = magnitude / soml__nanoseconds_per_hour;
	uint64_t minutes = magnitude / soml__nanoseconds_per_minute % 60;
	uint64_t seconds = magnitude / soml__nanoseconds_per_second % 60;
	uint64_t fraction = magnitude % soml__nanoseconds_per_second;
	char text[48];
	size_t length = 0;

	if (nanoseconds < 0) {
		text[length++] = '-';
	}

	if (hours > 0) {
		length += soml__format_unsigned(hours, text + length);
		text[length++] = 'h';
	}

	if (minutes > 0) {
		length += soml__format_unsigned(minutes, text + length);
		text[length++] = 'm';
	}

	if (seconds > 0 || fraction > 0) {
		length += soml__format_unsigned(seconds, text + length);
		length += soml__format_fraction((int64_t)fraction, text + length);
		text[length++] = 's';
	}

	return soml__append(writer, text, length);
}

[[nodiscard]] static int soml__compare_member_keys(const void *context, size_t left, size_t right) {
	const soml__member *members = context;
	return soml__compare_strings(members[left].key, members[right].key);
}

[[nodiscard]] static bool soml__write_value(soml__writer *writer, const soml_value *value, size_t indentation, size_t level);

/*
Note: Not using `isnan()` or `value != value` because `-ffast-math`, which the user's build may use, can optimize these checks away. Check the bits directly.
*/
[[nodiscard]] static bool soml__is_nan(double value) {
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return (bits & ~soml__float_sign_bit) > soml__float_infinity_bits;
}

/*
Check an object's unique key order. The caller owns `*temporary` and releases it with `count * 2 * sizeof(size_t)` bytes. A validated or already strictly ordered object needs no temporary allocation.
*/
[[nodiscard]] static bool soml__prepare_order(soml__writer *writer, const soml_value *object, size_t **temporary) {
	*temporary = nullptr;
	size_t count = object->as.object.count;
	const soml__member *members = object->as.object.members;

	if (object->is_sorted || count < 2) {
		return true;
	}

	bool is_ordered = true;

	for (size_t index = 1; index < count; index++) {
		int comparison = soml__compare_strings(members[index - 1].key, members[index].key);

		if (comparison == 0) {
			return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "An object has a duplicate key");
		}

		if (comparison > 0) {
			is_ordered = false;
			break;
		}
	}

	if (is_ordered) {
		return true;
	}

	size_t *order = soml__allocate_scratch(writer->allocator, count, 2 * sizeof(*order));

	if (order == nullptr) {
		return soml__writer_fail(writer, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
	}

	for (size_t index = 0; index < count; index++) {
		order[index] = index;
	}

	soml__merge_sort(order, order + count, count, soml__compare_member_keys, members);

	for (size_t index = 1; index < count; index++) {
		if (soml__compare_strings(members[order[index - 1]].key, members[order[index]].key) == 0) {
			soml__release_scratch(writer->allocator, order, count, 2 * sizeof(*order));
			return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "An object has a duplicate key");
		}
	}

	*temporary = order;
	return true;
}

/*
Writes the members of an object, each on its own line at `indentation`, sorted by key in canonical form and in the order of the document otherwise. Containers in the members are at nesting level `level + 1`.

Both orders check unvalidated objects for duplicate keys using the sorted order first, so a duplicate is refused before any member is written. Parsed objects and strictly ordered builders already have unique keys.
*/
[[nodiscard]] static bool soml__write_members(soml__writer *writer, const soml_value *object, size_t indentation, size_t level) {
	size_t count = object->as.object.count;
	const soml__member *members = object->as.object.members;
	size_t *temporary;

	// `count` is never 0 here, because an empty object is written `{}`.
	if (!soml__prepare_order(writer, object, &temporary)) {
		return false;
	}

	const size_t *order = object->is_sorted ? object->as.object.sorted : temporary;

	if (!writer->is_canonical) {
		order = nullptr;
	}

	bool is_success = true;

	// Parsed objects and strictly ordered builders already guarantee unique keys; preparation checks every other object before emitting any member.
	for (size_t index = 0; is_success && index < count; index++) {
		const soml__member *member = &members[order == nullptr ? index : order[index]];

		is_success = soml__append_indentation(writer, indentation)
			&& soml__write_key(writer, member->key)
			&& SOML__APPEND_LITERAL(writer, ": ")
			&& soml__write_value(writer, member->value, indentation, level + 1)
			&& soml__append_byte(writer, '\n');
	}

	soml__release_scratch(writer->allocator, temporary, count, 2 * sizeof(*temporary));
	return is_success;
}

/*
Writes a value after a key or at an item's indentation. `level` is the nesting level the value has when it is a container.
*/
[[nodiscard]] static bool soml__write_value(soml__writer *writer, const soml_value *value, size_t indentation, size_t level) {
	bool is_container = value->type == SOML_TYPE_ARRAY || value->type == SOML_TYPE_OBJECT;

	if (is_container && level > soml__maximum_depth) {
		return soml__writer_fail(writer, SOML_ERROR_TOO_DEEP, "The value is nested more than 100 levels deep, which is the most a SOML reader accepts");
	}

	switch (value->type) {
		case SOML_TYPE_STRING: {
			return soml__write_string(writer, value->as.string);
		}

		case SOML_TYPE_INT: {
			char text[24];
			size_t length = 0;
			int64_t integer = value->as.integer;

			if (integer < 0) {
				text[length++] = '-';
			}

			length += soml__format_unsigned(soml__magnitude(integer), text + length);
			return soml__append(writer, text, length);
		}

		case SOML_TYPE_FLOAT: {
			if (soml__is_nan(value->as.number)) {
				return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "NaN is not a SOML value");
			}

			char text[32];
			return soml__append(writer, text, soml__format_float(value->as.number, text));
		}

		case SOML_TYPE_BOOL: {
			return value->as.boolean ? SOML__APPEND_LITERAL(writer, "true") : SOML__APPEND_LITERAL(writer, "false");
		}

		case SOML_TYPE_NULL: {
			return SOML__APPEND_LITERAL(writer, "null");
		}

		case SOML_TYPE_INSTANT: {
			return soml__write_instant(writer, value->as.instant);
		}

		case SOML_TYPE_DURATION: {
			return soml__write_duration(writer, value->as.duration);
		}

		case SOML_TYPE_ARRAY: {
			if (value->as.array.count == 0) {
				return SOML__APPEND_LITERAL(writer, "[]");
			}

			if (!SOML__APPEND_LITERAL(writer, "[\n")) {
				return false;
			}

			for (size_t index = 0; index < value->as.array.count; index++) {
				bool is_success = soml__append_indentation(writer, indentation + 1)
					&& soml__write_value(writer, value->as.array.items[index], indentation + 1, level + 1)
					&& soml__append_byte(writer, '\n');

				if (!is_success) {
					return false;
				}
			}

			return soml__append_indentation(writer, indentation) && soml__append_byte(writer, ']');
		}

		case SOML_TYPE_OBJECT: {
			if (value->as.object.count == 0) {
				return SOML__APPEND_LITERAL(writer, "{}");
			}

			return SOML__APPEND_LITERAL(writer, "{\n")
				&& soml__write_members(writer, value, indentation + 1, level)
				&& soml__append_indentation(writer, indentation)
				&& soml__append_byte(writer, '}');
		}

		case SOML_TYPE_NONE: {
			break;
		}
	}

	return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "A value has an unknown type");
}

[[nodiscard]] static const soml_value *soml__check_document(soml__writer *writer, const soml_document *document) {
	if (document->failure != SOML_ERROR_NONE) {
		soml__writer_fail(writer, document->failure, document->failure_message);
		return nullptr;
	}

	const soml_value *root = document->root;

	if (root == nullptr || (root->type != SOML_TYPE_OBJECT && root->type != SOML_TYPE_ARRAY)) {
		soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "A document must be an object or an array");
		return nullptr;
	}

	return root;
}

[[nodiscard]] static bool soml__write_document(soml__writer *writer, const soml_document *document) {
	const soml_value *root = soml__check_document(writer, document);

	if (root == nullptr) {
		return false;
	}

	// A non-empty top-level object is written without braces, and its entries start at column 0, but it is still the first level.
	if (root->type == SOML_TYPE_OBJECT && root->as.object.count > 0) {
		return soml__write_members(writer, root, 0, 1);
	}

	return soml__write_value(writer, root, 0, 1) && soml__append_byte(writer, '\n');
}

/*
Validate one tree without formatting scalars. Temporary key orders are released before walking children, so nested validation does not hold every parent's sort buffer at once.
*/
[[nodiscard]] static bool soml__validate_value(soml__writer *writer, const soml_value *value, size_t level) {
	if ((value->type == SOML_TYPE_ARRAY || value->type == SOML_TYPE_OBJECT) && level > soml__maximum_depth) {
		return soml__writer_fail(writer, SOML_ERROR_TOO_DEEP, "The value is nested more than 100 levels deep, which is the most a SOML reader accepts");
	}

	switch (value->type) {
		case SOML_TYPE_STRING: {
			bool needs_escapes;
			return soml__check_string(writer, value->as.string, &needs_escapes);
		}

		case SOML_TYPE_FLOAT: {
			return !soml__is_nan(value->as.number) || soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "NaN is not a SOML value");
		}

		case SOML_TYPE_INSTANT: {
			return soml__check_instant(writer, value->as.instant);
		}

		case SOML_TYPE_ARRAY: {
			for (size_t index = 0; index < value->as.array.count; index++) {
				if (!soml__validate_value(writer, value->as.array.items[index], level + 1)) {
					return false;
				}
			}

			return true;
		}

		case SOML_TYPE_OBJECT: {
			size_t *temporary;

			if (!soml__prepare_order(writer, value, &temporary)) {
				return false;
			}

			soml__release_scratch(writer->allocator, temporary, value->as.object.count, 2 * sizeof(*temporary));

			for (size_t index = 0; index < value->as.object.count; index++) {
				const soml__member *member = &value->as.object.members[index];
				bool needs_escapes;

				if (!soml__check_string(writer, member->key, &needs_escapes) || !soml__validate_value(writer, member->value, level + 1)) {
					return false;
				}
			}

			return true;
		}

		case SOML_TYPE_INT:
		case SOML_TYPE_BOOL:
		case SOML_TYPE_NULL:
		case SOML_TYPE_DURATION: {
			return true;
		}

		case SOML_TYPE_NONE: {
			return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "A value has an unknown type");
		}
	}

	return soml__writer_fail(writer, SOML_ERROR_INVALID_VALUE, "A value has an unknown type");
}

static void soml__set_writer_error(soml_error *error, const soml__writer *writer) {
	soml__set_error(error, writer->error_code, writer->error_message);
}

#ifdef SOML__HAS_ADDRESS_SANITIZER
constexpr size_t soml__output_prefix_size = 8;
#else
constexpr size_t soml__output_prefix_size = 0;
#endif

// The text that `soml_write` and `soml_write_canonical` collect, in a block from the allocator.
typedef struct soml__text {
	soml__writer *writer;
	soml__chunk *chunk;
	size_t length;
	size_t capacity;
} soml__text;

[[nodiscard]] static bool soml__append_to_text(void *context, const char *bytes, size_t length) {
	soml__text *text = context;
	size_t needed;
	soml__chunk *chunk = ckd_add(&needed, text->length, length) || ckd_add(&needed, needed, sizeof(*chunk) + soml__output_prefix_size) || ckd_add(&needed, needed, 1) ? nullptr : soml__grow(text->writer->allocator, text->chunk, &text->capacity, 1, needed);

	// The writer keeps its first error, so this one is reported, not the failure of the callback.
	if (chunk == nullptr) {
		return soml__writer_fail(text->writer, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
	}

	memcpy((char *)chunk->data + soml__output_prefix_size + text->length, bytes, length);
	text->chunk = chunk;
	text->length += length;
	return true;
}

/*
Initializes all writer metadata. The output buffer is filled before it is sent, so clearing its 4 KB would be unnecessary work on every write.
*/
static void soml__initialize_writer(soml__writer *writer, const soml_allocator *allocator, bool (*write)(void *context, const char *bytes, size_t length), void *context, bool is_canonical) {
	writer->allocator = allocator;
	writer->write = write;
	writer->context = context;
	writer->is_canonical = is_canonical;
	writer->error_code = SOML_ERROR_NONE;
	writer->error_message = nullptr;
	writer->length = 0;
}

SOMLDEF bool soml_validate(const soml_document *document, soml_error *error) {
	if (document == nullptr) {
		soml__set_error(error, SOML_ERROR_INVALID_ARGUMENT, "The document is nullptr");
		return false;
	}

	soml__writer writer;
	soml__initialize_writer(&writer, &document->allocator, nullptr, nullptr, false);
	const soml_value *root = soml__check_document(&writer, document);

	if (root == nullptr || !soml__validate_value(&writer, root, 1)) {
		soml__set_writer_error(error, &writer);
		return false;
	}

	soml__set_error(error, SOML_ERROR_NONE, nullptr);
	return true;
}

/*
Copy one value without round-tripping through text. Reserve containers once, then use the ordinary attachment operations so the copied ownership follows the same rules as a builder.
*/
[[nodiscard]] static soml_value *soml__clone_value(soml_document *document, const soml_value *source, size_t level) {
	bool is_container = source->type == SOML_TYPE_ARRAY || source->type == SOML_TYPE_OBJECT;

	if (is_container && level > soml__maximum_depth) {
		soml__record_failure(document, SOML_ERROR_TOO_DEEP, "The value is nested more than 100 levels deep, which is the most a SOML reader accepts");
		return nullptr;
	}

	switch (source->type) {
		case SOML_TYPE_STRING: {
			return soml_new_string(document, source->as.string.data, source->as.string.length);
		}

		case SOML_TYPE_ARRAY: {
			soml_value *copy = soml_new_array(document);

			if (!soml_array_reserve(document, copy, source->as.array.count)) {
				return nullptr;
			}

			for (size_t index = 0; index < source->as.array.count; index++) {
				soml_value *item = soml__clone_value(document, source->as.array.items[index], level + 1);

				if (!soml_array_append(document, copy, item)) {
					return nullptr;
				}
			}

			return copy;
		}

		case SOML_TYPE_OBJECT: {
			soml_value *copy = soml_new_object(document);
			size_t count = source->as.object.count;

			if (!soml_object_reserve(document, copy, count)) {
				return nullptr;
			}

			for (size_t index = 0; index < count; index++) {
				const soml__member *member = &source->as.object.members[index];
				soml_value *value = soml__clone_value(document, member->value, level + 1);

				if (!soml_object_add(document, copy, member->key.data, member->key.length, value)) {
					return nullptr;
				}
			}

			if (source->is_sorted && source->as.object.sorted != nullptr) {
				size_t *order = soml__allocate_array(document, count, sizeof(*order));

				if (order == nullptr) {
					soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
					return nullptr;
				}

				memcpy(order, source->as.object.sorted, count * sizeof(*order));
				copy->as.object.sorted = order;
				copy->is_sorted = true;
			}

			return copy;
		}

		case SOML_TYPE_INT:
		case SOML_TYPE_FLOAT:
		case SOML_TYPE_BOOL:
		case SOML_TYPE_NULL:
		case SOML_TYPE_INSTANT:
		case SOML_TYPE_DURATION: {
			return soml__build_value(document, (soml_value){.type = source->type, .as = source->as});
		}

		case SOML_TYPE_NONE: {
			break;
		}
	}

	soml__record_failure(document, SOML_ERROR_INVALID_VALUE, "A value has an unknown type");
	return nullptr;
}

SOMLDEF soml_value *soml_clone_value(soml_document *document, const soml_value *value) {
	if (document == nullptr || value == nullptr) {
		soml__record_failure(document, SOML_ERROR_INVALID_ARGUMENT, "The destination document or source value is nullptr");
		return nullptr;
	}

	return soml__clone_value(document, value, 1);
}

SOMLDEF soml_document *soml_document_clone(const soml_document *source, const soml_allocator *allocator, soml_error *error) {
	if (!soml_validate(source, error)) {
		return nullptr;
	}

	soml_document *copy = soml_document_create(allocator);

	if (copy == nullptr) {
		soml__set_error(error, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
		return nullptr;
	}

	if (!soml_document_set_root(copy, soml_clone_value(copy, source->root))) {
		soml__set_error(error, copy->failure, copy->failure_message);
		soml_document_free(copy);
		return nullptr;
	}

	soml__set_error(error, SOML_ERROR_NONE, nullptr);
	return copy;
}

SOMLDEF bool soml_object_index(soml_document *document, soml_value *object) {
	if (!soml__check_container(document, object, SOML_TYPE_OBJECT, "soml_object_index needs an object")) {
		return false;
	}

	if (object->is_sorted) {
		return true;
	}

	soml__writer writer;
	soml__initialize_writer(&writer, &document->allocator, nullptr, nullptr, false);
	size_t *temporary;

	if (!soml__prepare_order(&writer, object, &temporary)) {
		soml__record_failure(document, writer.error_code, writer.error_message);
		return false;
	}

	size_t count = object->as.object.count;
	size_t *order = nullptr;

	if (temporary != nullptr) {
		order = soml__allocate_array(document, count, sizeof(*order));

		if (order == nullptr) {
			soml__release_scratch(&document->allocator, temporary, count, 2 * sizeof(*temporary));
			soml__record_failure(document, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
			return false;
		}

		memcpy(order, temporary, count * sizeof(*order));
	}

	soml__release_scratch(&document->allocator, temporary, count, 2 * sizeof(*temporary));
	object->as.object.sorted = order;
	object->is_sorted = true;
	return true;
}

typedef struct soml__measure_sink {
	soml__writer *writer;
	size_t length;
} soml__measure_sink;

/*
Count output with checked addition instead of retaining the bytes.
*/
[[nodiscard]] static bool soml__count_output(void *context, const char *bytes, size_t length) {
	(void)bytes;
	soml__measure_sink *sink = context;

	if (ckd_add(&sink->length, sink->length, length)) {
		return soml__writer_fail(sink->writer, SOML_ERROR_OUT_OF_MEMORY, soml__out_of_memory_message);
	}

	return true;
}

SOMLDEF bool soml_measure(const soml_document *document, size_t *length, soml_error *error) {
	if (document == nullptr || length == nullptr) {
		soml__set_error(error, SOML_ERROR_INVALID_ARGUMENT, "The document or the output is nullptr");
		return false;
	}

	soml__writer writer;
	soml__measure_sink sink = {.writer = &writer};
	soml__initialize_writer(&writer, &document->allocator, soml__count_output, &sink, false);

	if (!soml__write_document(&writer, document) || !soml__writer_flush(&writer)) {
		soml__set_writer_error(error, &writer);
		return false;
	}

	*length = sink.length;
	soml__set_error(error, SOML_ERROR_NONE, nullptr);
	return true;
}

typedef struct soml__buffer_sink {
	soml__writer *writer;
	char *data;
	size_t capacity;
	size_t length;
} soml__buffer_sink;

/*
Copy only pieces that fit, keeping the caller's published length unchanged until the whole write succeeds.
*/
[[nodiscard]] static bool soml__copy_output(void *context, const char *bytes, size_t length) {
	soml__buffer_sink *sink = context;

	if (length > sink->capacity - sink->length) {
		return soml__writer_fail(sink->writer, SOML_ERROR_WRITE, "The output buffer is too small");
	}

	memcpy(sink->data + sink->length, bytes, length);
	sink->length += length;
	return true;
}

[[nodiscard]] static bool soml__write_buffer(const soml_document *document, bool is_canonical, char *buffer, size_t capacity, size_t *length, soml_error *error) {
	if (document == nullptr || length == nullptr || (buffer == nullptr && capacity > 0)) {
		soml__set_error(error, SOML_ERROR_INVALID_ARGUMENT, "The document, output, or nonempty buffer is nullptr");
		return false;
	}

	if (capacity == 0) {
		soml__set_error(error, SOML_ERROR_WRITE, "The output buffer is too small");
		return false;
	}

	soml__writer writer;
	soml__buffer_sink sink = {.writer = &writer, .data = buffer, .capacity = capacity - 1};
	soml__initialize_writer(&writer, &document->allocator, soml__copy_output, &sink, is_canonical);

	if (!soml__write_document(&writer, document) || !soml__writer_flush(&writer)) {
		soml__set_writer_error(error, &writer);
		return false;
	}

	buffer[sink.length] = '\0';
	*length = sink.length;
	soml__set_error(error, SOML_ERROR_NONE, nullptr);
	return true;
}

SOMLDEF bool soml_write_to_buffer(const soml_document *document, char *buffer, size_t capacity, size_t *length, soml_error *error) {
	return soml__write_buffer(document, false, buffer, capacity, length, error);
}

SOMLDEF bool soml_write_canonical_to_buffer(const soml_document *document, char *buffer, size_t capacity, size_t *length, soml_error *error) {
	return soml__write_buffer(document, true, buffer, capacity, length, error);
}

[[nodiscard]] static bool soml__write_text(soml_document *document, bool is_canonical, soml_string *output, soml_error *error) {
	if (document == nullptr || output == nullptr) {
		soml__set_error(error, SOML_ERROR_INVALID_ARGUMENT, "The document or the output is nullptr");
		return false;
	}

	soml__writer writer;
	soml__text text = {.writer = &writer};
	soml__initialize_writer(&writer, &document->allocator, soml__append_to_text, &text, is_canonical);
	bool is_success = soml__write_document(&writer, document) && soml__writer_flush(&writer);
	// The text grows in a block from the allocator, because a block in the document cannot grow. On success the document takes ownership of that chunk directly, so no final copy or allocation is needed.
	if (!is_success) {
		soml__release(writer.allocator, text.chunk, text.capacity);
		soml__set_writer_error(error, &writer);
		return false;
	}

	soml__chunk *chunk = text.chunk;
	char *data = (char *)chunk->data + soml__output_prefix_size;
	data[text.length] = '\0';
	chunk->size = text.capacity - sizeof(*chunk);
	SOML__POISON(chunk->data, soml__output_prefix_size);
	SOML__POISON(data + text.length + 1, chunk->size - soml__output_prefix_size - text.length - 1);

	// A valid root already has an arena chunk. Keep that chunk's spare space; output chunks are owned but never used for bump allocation.
	chunk->next = document->chunks->next;
	document->chunks->next = chunk;

	*output = (soml_string){.data = data, .length = text.length};
	soml__set_error(error, SOML_ERROR_NONE, nullptr);
	return true;
}

[[nodiscard]] static bool soml__write_to_callback(const soml_document *document, bool is_canonical, bool (*write)(void *context, const char *bytes, size_t length), void *context, soml_error *error) {
	if (document == nullptr || write == nullptr) {
		soml__set_error(error, SOML_ERROR_INVALID_ARGUMENT, "The document or the callback is nullptr");
		return false;
	}

	soml__writer writer;
	soml__initialize_writer(&writer, &document->allocator, write, context, is_canonical);

	if (!soml__write_document(&writer, document) || !soml__writer_flush(&writer)) {
		soml__set_writer_error(error, &writer);
		return false;
	}

	soml__set_error(error, SOML_ERROR_NONE, nullptr);
	return true;
}

SOMLDEF bool soml_write(soml_document *document, soml_string *output, soml_error *error) {
	return soml__write_text(document, false, output, error);
}

SOMLDEF bool soml_write_canonical(soml_document *document, soml_string *output, soml_error *error) {
	return soml__write_text(document, true, output, error);
}

SOMLDEF bool soml_write_with_callback(const soml_document *document, bool (*write)(void *context, const char *bytes, size_t length), void *context, soml_error *error) {
	return soml__write_to_callback(document, false, write, context, error);
}

SOMLDEF bool soml_write_canonical_with_callback(const soml_document *document, bool (*write)(void *context, const char *bytes, size_t length), void *context, soml_error *error) {
	return soml__write_to_callback(document, true, write, context, error);
}

#endif // SOML_IMPLEMENTATION

/*
MIT License

Copyright (c) Sindre Sorhus <sindresorhus@gmail.com> (https://sindresorhus.com)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

---

The high-precision decimal conversion follows Go's strconv/decimal.go and strconv/ftoa.go, under this license:

Copyright 2009 The Go Authors.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

   * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
   * Neither the name of Google LLC nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
