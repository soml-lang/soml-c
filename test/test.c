/*
Unit tests for soml.h. Run them from the repository root with `make unit`, because the float tests read files in `test/fixtures`.

The conformance suite is in `conformance.js`. These tests cover the C API, the builder and writer, float conversion on large corpora, allocation failure, and time complexity.
*/
#define SOML_IMPLEMENTATION
#include "../soml.h"
#include "../fuzz/allocator.h"

#include <fenv.h>
#include <inttypes.h>
#include <limits.h>

#if defined(__SSE2__)
	#include <xmmintrin.h>
#endif

// The small-stack test needs threads, and its frame sizes do not hold under sanitizers, which make frames larger.
#if defined(__has_feature)
	#if __has_feature(address_sanitizer)
		#define HAS_ADDRESS_SANITIZER 1
	#endif
	#if __has_feature(thread_sanitizer)
		#define HAS_THREAD_SANITIZER 1
	#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
	#define HAS_ADDRESS_SANITIZER 1
#endif

#if defined(__SANITIZE_THREAD__)
	#define HAS_THREAD_SANITIZER 1
#endif

#if __has_include(<pthread.h>) && !defined(__wasm__)
	#define HAS_THREADS 1
	#include <pthread.h>
	#if !defined(HAS_ADDRESS_SANITIZER) && !defined(HAS_THREAD_SANITIZER)
		#define HAS_SMALL_STACK_TEST 1
	#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failure_count = 0;
static int check_count = 0;

#define CHECK(condition) check((condition), #condition, __FILE__, __LINE__)

static bool check(bool condition, const char *text, const char *file, int line) {
	check_count++;

	if (!condition) {
		failure_count++;
		fprintf(stderr, "%s:%d: check failed: %s\n", file, line, text);
	}

	return condition;
}

/*
Parses a copy of the text in a block of exactly its size, which is freed before the document is used. So AddressSanitizer reports a read past the end of the text, and a document that still points into it.
*/
static soml_document *parse_text(const char *text, soml_error *error) {
	size_t length = strlen(text);
	char *copy = malloc(length == 0 ? 1 : length);
	memcpy(copy, text, length);
	soml_document *document = soml_parse(copy, length, nullptr, error);
	free(copy);
	return document;
}

static bool is_rejected(const char *text, soml_error_code code) {
	soml_error error;
	soml_document *document = parse_text(text, &error);
	bool is_rejected = document == nullptr;
	soml_document_free(document);
	return is_rejected && error.code == code;
}

/*
The canonical form of a text, or nullptr when it is rejected. The result must be freed.
*/
static char *canonical(const char *text) {
	soml_document *document = parse_text(text, nullptr);
	soml_string output;

	if (document == nullptr || !soml_write_canonical(document, &output, nullptr)) {
		soml_document_free(document);
		return nullptr;
	}

	char *copy = malloc(output.length + 1);
	memcpy(copy, output.data, output.length + 1);
	soml_document_free(document);
	return copy;
}

static bool has_canonical_form(const char *text, const char *expected) {
	char *result = canonical(text);
	bool is_equal = result != nullptr && strcmp(result, expected) == 0;

	if (!is_equal) {
		fprintf(stderr, "Canonical form of:\n%s\nwas:\n%s\nexpected:\n%s\n", text, result == nullptr ? "(rejected)" : result, expected);
	}

	free(result);
	return is_equal;
}

static bool string_equals(soml_string string, const char *expected) {
	return string.data != nullptr && string.length == strlen(expected) && memcmp(string.data, expected, string.length) == 0;
}

static char *read_file(const char *path, size_t *length) {
	FILE *file = fopen(path, "rb");

	if (file == nullptr) {
		fprintf(stderr, "Could not open %s. Run the tests from the repository root.\n", path);
		exit(1);
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	char *buffer = malloc((size_t)size + 1);
	size_t count = fread(buffer, 1, (size_t)size, file);
	buffer[count] = '\0';
	fclose(file);
	*length = count;
	return buffer;
}

// MARK: Reading

static void test_accessors(void) {
	soml_error error;
	soml_document *document = parse_text("name: 'api'\nreplicas: 3\nratio: 3.0\non: true\nowner: null\nat: 2026-09-19T14:00:00.5+07:00\ngrace: 1m30s\nlabels: ['a', 'b']\npostgres: {port: 5432}\n", &error);
	CHECK(document != nullptr);
	soml_value *root = soml_document_root(document);
	CHECK(soml_type_of(root) == SOML_TYPE_OBJECT);
	CHECK(soml_count(root) == 9);

	soml_string name;
	CHECK(soml_get_string(soml_object_get(root, "name"), &name) && string_equals(name, "api"));

	int64_t replicas;
	double ratio;
	CHECK(soml_get_int(soml_object_get(root, "replicas"), &replicas) && replicas == 3);
	CHECK(!soml_get_float(soml_object_get(root, "replicas"), &ratio));
	CHECK(soml_get_float(soml_object_get(root, "ratio"), &ratio) && ratio == 3.0);
	CHECK(!soml_get_int(soml_object_get(root, "ratio"), &replicas));

	bool on;
	CHECK(soml_get_bool(soml_object_get(root, "on"), &on) && on);
	CHECK(soml_is_null(soml_object_get(root, "owner")));
	CHECK(!soml_is_null(soml_object_get(root, "missing")));

	soml_instant instant = {};
	CHECK(soml_get_instant(soml_object_get(root, "at"), &instant));
	CHECK(instant.seconds == 1'789'801'200 && instant.nanoseconds == 500'000'000);

	int64_t grace;
	CHECK(soml_get_duration(soml_object_get(root, "grace"), &grace) && grace == 90'000'000'000);

	soml_value *labels = soml_object_get(root, "labels");
	soml_string label;
	CHECK(soml_count(labels) == 2);
	CHECK(soml_get_string(soml_array_at(labels, 1), &label) && string_equals(label, "b"));
	CHECK(soml_array_at(labels, 2) == nullptr);

	int64_t port;
	CHECK(soml_get_int(soml_object_get(soml_object_get(root, "postgres"), "port"), &port) && port == 5432);

	// A missing member, and every accessor given nullptr.
	CHECK(soml_object_get(soml_object_get(root, "missing"), "port") == nullptr);
	CHECK(soml_type_of(nullptr) == SOML_TYPE_NONE);
	CHECK(soml_count(nullptr) == 0);
	CHECK(!soml_get_int(nullptr, &port));
	CHECK(soml_object_key_at(nullptr, 0).data == nullptr);
	CHECK(soml_object_value_at(root, 99) == nullptr);
	CHECK(soml_document_root(nullptr) == nullptr);
	CHECK(soml_object_get(root, nullptr) == nullptr);
	soml_document_free(nullptr);

	// A getter for another type returns false and leaves its output unchanged, and a count or an item of a scalar is 0 or nullptr.
	soml_value *number = soml_object_get(root, "replicas");
	soml_string unchanged_string = {.data = "x", .length = 1};
	bool unchanged_bool = true;
	soml_instant unchanged_instant = {.seconds = 7, .nanoseconds = 7};
	int64_t unchanged_duration = 7;
	CHECK(!soml_get_string(number, &unchanged_string) && unchanged_string.length == 1);
	CHECK(!soml_get_bool(number, &unchanged_bool) && unchanged_bool);
	CHECK(!soml_get_instant(number, &unchanged_instant) && unchanged_instant.seconds == 7);
	CHECK(!soml_get_duration(number, &unchanged_duration) && unchanged_duration == 7);
	CHECK(!soml_get_int(soml_object_get(root, "name"), &replicas));
	CHECK(!soml_is_null(number));
	CHECK(soml_count(number) == 0);
	CHECK(soml_array_at(number, 0) == nullptr);
	CHECK(soml_array_at(root, 0) == nullptr);
	CHECK(soml_object_get(labels, "a") == nullptr);
	CHECK(soml_object_key_at(labels, 0).data == nullptr);
	CHECK(!soml_get_int(number, nullptr));

	// Every type and nullptr must accept only its matching getter, leave failed outputs unchanged, and reject a nullptr output.
	const char marker[] = "sentinel";

	for (size_t index = 0; index <= soml_count(root); index++) {
		soml_value *value = soml_object_value_at(root, index);
		soml_type type = soml_type_of(value);
		soml_string string = {.data = marker, .length = SIZE_MAX};
		int64_t integer = 7;
		double floating = 7;
		bool boolean = true;
		soml_instant timestamp = {.seconds = 7, .nanoseconds = 7};
		int64_t duration = 7;
		CHECK(soml_get_string(value, &string) == (type == SOML_TYPE_STRING));
		CHECK(soml_get_int(value, &integer) == (type == SOML_TYPE_INT));
		CHECK(soml_get_float(value, &floating) == (type == SOML_TYPE_FLOAT));
		CHECK(soml_get_bool(value, &boolean) == (type == SOML_TYPE_BOOL));
		CHECK(soml_get_instant(value, &timestamp) == (type == SOML_TYPE_INSTANT));
		CHECK(soml_get_duration(value, &duration) == (type == SOML_TYPE_DURATION));
		CHECK(soml_is_null(value) == (type == SOML_TYPE_NULL));
		CHECK(type == SOML_TYPE_STRING || (string.data == marker && string.length == SIZE_MAX));
		CHECK(type == SOML_TYPE_INT || integer == 7);
		CHECK(type == SOML_TYPE_FLOAT || floating == 7);
		CHECK(type == SOML_TYPE_BOOL || boolean);
		CHECK(type == SOML_TYPE_INSTANT || (timestamp.seconds == 7 && timestamp.nanoseconds == 7));
		CHECK(type == SOML_TYPE_DURATION || duration == 7);
		CHECK(!soml_get_string(value, nullptr));
		CHECK(!soml_get_int(value, nullptr));
		CHECK(!soml_get_float(value, nullptr));
		CHECK(!soml_get_bool(value, nullptr));
		CHECK(!soml_get_instant(value, nullptr));
		CHECK(!soml_get_duration(value, nullptr));
	}

	soml_document_free(document);
}

static void test_member_order(void) {
	// Members are in source order.
	soml_document *document = parse_text("z: 1\na: {b: 2, d: 4}\nc: 3\n'': 5\n", nullptr);
	soml_value *root = soml_document_root(document);
	CHECK(soml_count(root) == 4);
	CHECK(string_equals(soml_object_key_at(root, 0), "z"));
	CHECK(string_equals(soml_object_key_at(root, 1), "a"));
	CHECK(string_equals(soml_object_key_at(root, 2), "c"));
	CHECK(string_equals(soml_object_key_at(root, 3), ""));

	soml_value *nested = soml_object_value_at(root, 1);
	CHECK(string_equals(soml_object_key_at(nested, 0), "b"));
	CHECK(string_equals(soml_object_key_at(nested, 1), "d"));

	// Lookup works whatever the order.
	int64_t value;
	CHECK(soml_get_int(soml_object_get(root, ""), &value) && value == 5);
	CHECK(soml_get_int(soml_object_get(root, "z"), &value) && value == 1);
	CHECK(soml_get_int(soml_object_get(nested, "d"), &value) && value == 4);
	soml_document_free(document);
}

static void test_strings_with_nul(void) {
	soml_document *document = parse_text("\"a\\u{0}b\": \"x\\u{0}y\"\n", nullptr);
	soml_value *root = soml_document_root(document);
	CHECK(soml_object_get(root, "a") == nullptr);
	soml_value *value = soml_object_get_with_length(root, "a\0b", 3);
	soml_string string;
	CHECK(soml_get_string(value, &string) && string.length == 3 && memcmp(string.data, "x\0y", 3) == 0 && string.data[3] == '\0');
	CHECK(soml_object_key_at(root, 0).length == 3);
	soml_document_free(document);
}

static void test_text_needs_no_terminator(void) {
	// The text is read by length, so bytes after it do not matter.
	const char text[] = "a: 1\nb: 2";
	soml_document *document = soml_parse(text, 4, nullptr, nullptr);
	CHECK(soml_count(soml_document_root(document)) == 1);
	soml_document_free(document);

	soml_error error;
	CHECK(soml_parse(nullptr, 0, nullptr, &error) == nullptr && error.code == SOML_ERROR_SYNTAX);
	CHECK(soml_parse(nullptr, 5, nullptr, &error) == nullptr && error.code == SOML_ERROR_INVALID_ARGUMENT);

	// A NUL byte is a raw control character, not the end of the text.
	CHECK(soml_parse("a: 1\0", 5, nullptr, &error) == nullptr && error.code == SOML_ERROR_SYNTAX && error.offset == 4);
}

static void test_size_overflow(void) {
	// The NUL terminator cannot fit. Rejection must happen before the bytes are read.
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_array(document);
	CHECK(soml_document_set_root(document, root));
	CHECK(soml_new_string(document, "", SIZE_MAX) == nullptr);
	soml_string output = {.data = "unchanged", .length = 9};
	soml_error error;
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY);
	CHECK(string_equals(output, "unchanged"));
	soml_document_free(document);

	document = soml_document_create(nullptr);
	root = soml_new_object(document);
	CHECK(soml_document_set_root(document, root));
	CHECK(!soml_object_add(document, root, "", SIZE_MAX, soml_new_null(document)));
	CHECK(soml_count(root) == 0);
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY);
	soml_document_free(document);

	// Out-of-range indexes must not wrap into valid indexes on either word size.
	document = parse_text("a: [1]", nullptr);
	root = soml_document_root(document);
	CHECK(soml_array_at(soml_object_get(root, "a"), SIZE_MAX) == nullptr);
	CHECK(soml_object_key_at(root, SIZE_MAX).data == nullptr);
	CHECK(soml_object_value_at(root, SIZE_MAX) == nullptr);
	soml_document_free(document);
}

static void test_allocation_size_overflow(void) {
	// Exercise overflowing capacities and byte sizes directly, without asking the system allocator for huge blocks. Rejection must preserve existing capacity and bytes, and must happen before the allocator is called.
	static const size_t element_sizes[] = {sizeof(size_t), sizeof(soml_value), sizeof(soml__member)};
	constexpr size_t count = 16;

	for (size_t element = 0; element < sizeof(element_sizes) / sizeof(element_sizes[0]); element++) {
		size_t element_size = element_sizes[element];
		size_t size = count * element_size;
		unsigned char *expected = malloc(size);

		for (size_t index = 0; index < size; index++) {
			expected[index] = (unsigned char)(index % 251);
		}

		fuzz_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
		soml_document *document = soml_document_create(&allocator);
		void *scratch = fuzz_reallocate(&state, nullptr, 0, size);
		void *arena = soml__allocate_array(document, count, element_size);
		CHECK(scratch != nullptr && arena != nullptr);
		memcpy(scratch, expected, size);
		memcpy(arena, expected, size);
		size_t scratch_capacity = count;
		size_t arena_capacity = count;
		size_t allocations = state.allocations;
		CHECK(soml__grow(&allocator, scratch, &scratch_capacity, element_size, count) == scratch);
		CHECK(soml__grow_in_document(document, arena, count, &arena_capacity, element_size, count) == arena);
		CHECK(soml__allocate_scratch(&allocator, 0, element_size) == nullptr && state.allocations == allocations);
		const size_t overflowing_counts[] = {SIZE_MAX, SIZE_MAX / 2 + 1, SIZE_MAX / element_size + 1};

		for (size_t index = 0; index < sizeof(overflowing_counts) / sizeof(overflowing_counts[0]); index++) {
			size_t needed = overflowing_counts[index];
			CHECK(soml__grow(&allocator, scratch, &scratch_capacity, element_size, needed) == nullptr && scratch_capacity == count);
			CHECK(soml__grow_in_document(document, arena, count, &arena_capacity, element_size, needed) == nullptr && arena_capacity == count);
			CHECK(soml__allocate_scratch(&allocator, needed, element_size) == nullptr);
			CHECK(soml__allocate_array(document, needed, element_size) == nullptr);
			CHECK(state.allocations == allocations);
			CHECK(memcmp(scratch, expected, size) == 0 && memcmp(arena, expected, size) == 0);
		}

		CHECK(soml__allocate(document, SIZE_MAX) == nullptr && state.allocations == allocations);
		CHECK(soml__allocate_bytes(document, SIZE_MAX) == nullptr && state.allocations == allocations);
		CHECK(memcmp(scratch, expected, size) == 0 && memcmp(arena, expected, size) == 0);
		CHECK(fuzz_reallocate(&state, scratch, size, 0) == nullptr);
		soml_document_free(document);
		CHECK(state.live_blocks == 0);
		free(expected);
	}
}

static void test_accessor_bounds(void) {
	// Exercise every index bit, including high bits that must not be narrowed away, on every type and nullptr.
	soml_document *document = parse_text("[[1, 2, 3], {a: 1, b: 2, c: 3}, [], {}, 'text', 1, 1.5, true, null, 1970-01-01T00:00:00Z, 1s]", nullptr);
	soml_value *root = soml_document_root(document);
	CHECK(document != nullptr && soml_count(root) == 11);

	for (size_t value_index = 0; value_index <= soml_count(root); value_index++) {
		soml_value *value = soml_array_at(root, value_index);
		size_t count = soml_count(value);

		for (size_t bit = 0; bit < sizeof(size_t) * CHAR_BIT; bit++) {
			for (size_t low = 0; low < 3; low++) {
				size_t index = ((size_t)1 << bit) | low;

				if (index < count) {
					continue;
				}

				soml_string key = soml_object_key_at(value, index);
				CHECK(soml_array_at(value, index) == nullptr);
				CHECK(key.data == nullptr && key.length == 0);
				CHECK(soml_object_value_at(value, index) == nullptr);
			}
		}
	}

	soml_document_free(document);
}

static void test_arena_redzones(void) {
	#ifdef HAS_ADDRESS_SANITIZER
	// A later arena allocation must not make the bytes before a string or value, or after its end, readable.
	char bytes[8193];
	memset(bytes, 'a', sizeof(bytes));
	soml_document *document = soml_document_create(nullptr);
	soml_value *values[36];

	for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
		size_t length = index < 33 ? index : 8191 + index - 33;
		values[index] = soml_new_string(document, bytes, length);
	}

	for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
		size_t length = index < 33 ? index : 8191 + index - 33;
		soml_string string;
		CHECK(soml_get_string(values[index], &string) && string.length == length && memcmp(string.data, bytes, length) == 0 && string.data[length] == '\0');
		CHECK(__asan_address_is_poisoned(string.data - 1));
		CHECK(__asan_address_is_poisoned(string.data + length + 1));
		CHECK(__asan_address_is_poisoned((const unsigned char *)values[index] - 1));
		CHECK(__asan_address_is_poisoned((const unsigned char *)values[index] + sizeof(*values[index])));
	}

	soml_document_free(document);

	// The first value in a fresh chunk must also have a leading redzone.
	document = soml_document_create(nullptr);
	soml_value *value = soml_new_null(document);
	CHECK(value != nullptr && __asan_address_is_poisoned((const unsigned char *)value - 1));
	soml_document_free(document);
	#endif
}

static void test_error_positions(void) {
	soml_error error;
	CHECK(parse_text("a: 1\nb: 'é' x\n", &error) == nullptr);
	CHECK(error.code == SOML_ERROR_SYNTAX && error.line == 2 && error.column == 8 && error.offset == 13);
	CHECK(error.message != nullptr && strstr(error.message, "Expected a line break") != nullptr);

	CHECK(parse_text("a: 1\na: 2\n", &error) == nullptr);
	CHECK(error.line == 2 && error.column == 1 && strcmp(error.message, "Duplicate key") == 0);

	CHECK(parse_text("x: '😀😀' 1", &error) == nullptr);
	CHECK(error.line == 1 && error.column == 9 && error.offset == 14);

	CHECK(soml_parse("a: '\xC3", 5, nullptr, &error) == nullptr);
	CHECK(error.offset == 4 && strstr(error.message, "Incomplete") != nullptr);
	CHECK(soml_parse("a: '\xC3\x28'", 7, nullptr, &error) == nullptr);
	CHECK(error.offset == 4 && strcmp(error.message, "Invalid UTF-8") == 0);

	// An encoded surrogate and an overlong form are invalid UTF-8.
	CHECK(soml_parse("a: '\xED\xA0\x80'", 8, nullptr, &error) == nullptr && error.offset == 4);
	CHECK(soml_parse("a: '\xC0\xAF'", 7, nullptr, &error) == nullptr && error.offset == 4);

	// Line breaks reset the column; tabs and two-, three-, and four-byte UTF-8 sequences each count as one scalar. Syntax and malformed UTF-8 errors must agree at the same byte boundary.
	for (size_t lines = 0; lines < 4; lines++) {
		for (size_t tabs = 0; tabs < 8; tabs++) {
			for (size_t groups = 0; groups < 9; groups++) {
				char text[256];
				size_t length = 0;

				for (size_t index = 0; index < lines; index++) {
					memcpy(text + length, "# é€😀\n", 12);
					length += 12;
				}

				memset(text + length, '\t', tabs);
				length += tabs;
				memcpy(text + length, "a: '", 4);
				length += 4;

				for (size_t index = 0; index < groups; index++) {
					memcpy(text + length, "é€😀", 9);
					length += 9;
				}

				memcpy(text + length, "' ", 2);
				length += 2;

				for (unsigned kind = 0; kind < 3; kind++) {
					text[length] = kind == 0 ? '?' : kind == 1 ? (char)0xFF : (char)0xF0;
					text[length + 1] = (char)0x9F;
					size_t size = length + (kind == 2 ? 2 : 1);
					char *copy = malloc(size);
					memcpy(copy, text, size);
					soml_document *document = soml_parse(copy, size, nullptr, &error);
					free(copy);
					CHECK(document == nullptr && error.code == SOML_ERROR_SYNTAX && error.offset == length && error.line == lines + 1 && error.column == tabs + groups * 3 + 7);
					soml_document_free(document);
				}
			}
		}
	}
}

/*
Whether the text is rejected with a message that holds `part`.
*/
static bool has_error_message_part(const char *text, const char *part) {
	soml_error error;
	soml_document *document = parse_text(text, &error);
	bool is_rejected = document == nullptr;
	soml_document_free(document);
	return is_rejected && strstr(error.message, part) != nullptr;
}

static bool is_rejected_without_part(const char *text, const char *part) {
	return is_rejected(text, SOML_ERROR_SYNTAX) && !has_error_message_part(text, part);
}

/*
Whether the text is rejected at this line and column with a message that holds `part`.
*/
static bool has_error_at(const char *text, size_t line, size_t column, const char *part) {
	soml_error error;
	soml_document *document = parse_text(text, &error);
	bool is_rejected = document == nullptr;
	soml_document_free(document);
	bool is_match = is_rejected && error.line == line && error.column == column && strstr(error.message, part) != nullptr;

	if (!is_match) {
		fprintf(stderr, "Error for:\n%s\nwas: %zu:%zu %s\nexpected: %zu:%zu %s\n", text, error.line, error.column, is_rejected ? error.message : "(accepted)", line, column, part);
	}

	return is_match;
}

/*
Whether the text is rejected with exactly this message.
*/
static bool has_error_reason(const char *text, const char *reason) {
	soml_error error;
	soml_document *document = parse_text(text, &error);
	bool is_rejected = document == nullptr;
	soml_document_free(document);
	bool is_match = is_rejected && strcmp(error.message, reason) == 0;

	if (!is_match) {
		fprintf(stderr, "Error for:\n%s\nwas: %s\nexpected: %s\n", text, is_rejected ? error.message : "(accepted)", reason);
	}

	return is_match;
}

static void test_missing_value_before_key(void) {
	// A member whose value was left out reads the key on the next line as its value, and that is reported at the key, whatever the key's spelling.
	CHECK(has_error_at("a:\n'b': 1", 2, 1, "found a key"));
	CHECK(has_error_at("a:\n\"b\": 1", 2, 1, "found a key"));
	CHECK(has_error_at("a:\n'b':1", 2, 1, "found a key"));
	CHECK(has_error_at("a:\nb: 1", 2, 1, "found a key"));
	CHECK(has_error_at("a:\nb:1", 2, 1, "found a key"));
	CHECK(has_error_at("a:\n404: 'x'", 2, 1, "found a key"));
	CHECK(has_error_at("a:\n-1: 2", 2, 1, "found a key"));
	CHECK(has_error_at("a:\ntrue: 1", 2, 1, "found a key"));
	CHECK(has_error_at("a:\ninfinity:\t1", 2, 1, "found a key"));
	CHECK(has_error_at("a:\nb:", 2, 1, "found a key"));
	CHECK(has_error_at("{a:\n'b': 1}", 2, 1, "found a key"));
	CHECK(has_error_at("{a:\n'b':}", 2, 1, "found a key"));
	CHECK(has_error_at("a: # comment\n'b': 1", 2, 1, "found a key"));
	CHECK(has_error_at("a:\n\n  'b': 1", 3, 3, "found a key"));

	// On the same line, or across a line break inside a block comment, which does not separate, the `:` is an error after a value.
	CHECK(has_error_at("a: 'b': 1", 1, 7, "after a value"));
	CHECK(has_error_at("a: /*\n*/ 'b': 1", 2, 7, "after a value"));
	CHECK(has_error_at("a: true: 1", 1, 8, "after a value"));

	// An array item is not a member, so its `:` is an error after a value.
	CHECK(has_error_at("[1,\n'b': 1]", 2, 4, "after a value"));

	// Something that is not a key followed by `:` keeps its own error.
	CHECK(has_error_at("a:\n'b' : 1", 2, 5, "Expected a line break"));
	CHECK(has_error_at("a:\n'b'.: 1", 2, 4, "after a value"));
	CHECK(has_error_at("a:\n'b'.c: 1", 2, 4, "after a value"));
	CHECK(has_error_at("a:\nb.c: 1", 2, 1, "Unexpected word"));
	CHECK(has_error_at("a:\n1.5: 2", 2, 1, "Invalid number"));
	CHECK(has_error_at("a:\n\"\\q\": 1", 2, 2, "Unknown escape"));
	CHECK(has_error_at("a:\nb.'c'x: 1", 2, 1, "Unexpected word"));

	// A bare key's `:` needs whitespace or the end after it, because an instant has colons too, and its own error is more precise.
	CHECK(has_error_at("a:\n2026-09-19T25:00:00Z", 2, 1, "the hour must be"));

	// The value on the next line is still read when it is one.
	CHECK(has_canonical_form("a:\n'b'", "a: 'b'\n"));
	CHECK(has_canonical_form("a:\n\ttrue\nb: 1", "a: true\nb: 1\n"));
}

static void test_first_collision(void) {
	// An object with several collisions reports the first in the source: the entry that collides with one before it and comes first, as a reader that checks each entry as it reads would.
	CHECK(has_error_at("b: 1\na: 1\nc: 1\na: 2\nb: 2", 4, 1, "Duplicate key"));
	CHECK(has_error_at("c: 1\nb: 1\na: 1\nc: 2\nb: 2\na: 2", 4, 1, "Duplicate key"));
	CHECK(has_error_at("a: 1\nb: 1\na: 2\na: 3\nb: 2", 3, 1, "Duplicate key"));
	CHECK(has_error_at("'a': 1\n\"\\u{61}\": 2\na: 3", 2, 1, "Duplicate key"));
	CHECK(has_error_at("{a: {b: 1, c: 2, b: 3}}", 1, 18, "Duplicate key"));

	// A quoted key that holds a dot is one key, so only an equal quoted key repeats it, and it is not the same as a nested object.
	CHECK(has_error_at("'a.b': 1\n\"a.b\": 2", 2, 1, "Duplicate key"));
	CHECK(has_canonical_form("'a.b': 1\na: {b: 2}", "a: {\n\tb: 2\n}\n'a.b': 1\n"));

	// Collisions that are already first between neighbors in key order.
	CHECK(has_error_at("a: 1\nb: 1\nb: 2\na: 2", 3, 1, "Duplicate key"));
	CHECK(has_error_at("a: 1\nb: 1\na: 2\nb: 2", 3, 1, "Duplicate key"));
	CHECK(has_error_at("a: 1\na: 2\na: 3", 2, 1, "Duplicate key"));

	// A collision before another error is the first error, also when the other error keeps the object from closing.
	CHECK(has_error_at("c: 70\nc: 36\nd: True", 2, 1, "Duplicate key"));
	CHECK(has_error_at("a: 1\na: 2\nb: [1 2]", 2, 1, "Duplicate key"));
	CHECK(has_error_at("a: 1\na: 2\nb: 1 c: 2", 2, 1, "Duplicate key"));
	CHECK(has_error_at("a: 1\na: 2\nb: 1,", 2, 1, "Duplicate key"));
	CHECK(has_error_at("a: 1\na: 2 /* x", 2, 1, "Duplicate key"));
	CHECK(has_error_at("a: 1\na: 2\nc: {d: 1 e: 2}", 2, 1, "Duplicate key"));
	CHECK(has_error_at("{a: 1, a: 2, b: }", 1, 8, "Duplicate key"));
	CHECK(has_error_at("{a: 1, a: 2", 1, 8, "Duplicate key"));
	CHECK(has_error_at("{a: 1, a: 2, b: 9223372036854775808}", 1, 8, "Duplicate key"));
	CHECK(has_error_at("[{a: 1, a: 2, b: ?}]", 1, 9, "Duplicate key"));

	// An open object's collisions come before those of an object open inside it, and those come before the error that stopped both.
	CHECK(has_error_at("x: {a: 1, a: 2, b: {c: 1, c: 2, d: nope}}", 1, 11, "Duplicate key"));
	CHECK(has_error_at("a: 1\nb: {a: 1, c: 2, c: 3, x: ?}", 2, 17, "Duplicate key"));
	CHECK(has_error_at("a: 1\nb: {c: 1, c: 2\na: 2", 2, 11, "Duplicate key"));
	CHECK(has_error_at("a: 1\nx: [{y: 1, y: 2}, ?]\na: 2", 2, 12, "Duplicate key"));
	CHECK(has_error_at("a: 1\na: 2\nb: {c: 1, c: 2, ?}", 2, 1, "Duplicate key"));

	// An entry whose value fails is not read yet, so it collides with nothing, and the error in its value is first.
	CHECK(has_error_at("a: 1\na: {b: nope}", 2, 8, "Unexpected word"));
	CHECK(has_error_at("a: 1\nb: 2\nc: nope", 3, 4, "Unexpected word"));
	CHECK(has_error_at("a: 1\na: [1 2]", 2, 7, "after an array item"));
}

static void test_dot_after_key(void) {
	// A key is never a path, so a `.` after a key is an error at the `.`, which suggests quoting and braces with an example for a bare key whose dots each have a word on both sides and that a `:` follows.
	static const char bare_message[] = "A bare key cannot contain “.”. Quote it, as in 'example.com', or use braces to nest, as in example: {com: …}";
	static const char other_message[] = "A key cannot contain “.” unless it is quoted. Quote the whole key, or use braces to nest, as in a: {b: …}";
	CHECK(has_error_at("a.b: 1", 1, 2, bare_message));
	CHECK(has_error_at("example.com: 1", 1, 8, bare_message));
	CHECK(has_error_at("3.14: 'x'", 1, 2, bare_message));
	CHECK(has_error_at("1.5s: 1", 1, 2, bare_message));
	CHECK(has_error_at("a.b.c: 1", 1, 2, bare_message));
	CHECK(has_error_at("{a.b: 1}", 1, 3, bare_message));
	CHECK(has_error_at("x: {a.b: 1}", 1, 6, bare_message));
	CHECK(has_error_at("[{a.b: 1}]", 1, 4, bare_message));
	CHECK(has_error_at("x: 1\n\ta.b: 2", 2, 3, bare_message));
	CHECK(has_error_at("'a'.b: 1", 1, 4, other_message));
	CHECK(has_error_at("a.'b': 1", 1, 2, other_message));
	CHECK(has_error_at("a.: 1", 1, 2, other_message));
	CHECK(has_error_at("a..b: 1", 1, 2, other_message));
	CHECK(has_error_at("a.b : 1", 1, 2, other_message));
	CHECK(has_error_at("a.$b: 1", 1, 2, other_message));
	CHECK(has_error_at("a.b", 1, 2, other_message));
	CHECK(has_error_at(".a: 1", 1, 1, "Expected a key. A key that contains characters other than letters, digits, “_”, and “-” must be quoted"));
	CHECK(has_error_at(".env: 1", 1, 1, "Expected a key. A key that contains characters other than letters, digits, “_”, and “-” must be quoted"));
	CHECK(has_error_at("files: {.eslintrc.json: 1}", 1, 9, "Expected a key. A key that contains characters other than letters, digits, “_”, and “-” must be quoted"));

	// The example is given only for a key of up to 1,000 characters, as in the reference implementation.
	char text[1100];
	memset(text, 'a', sizeof(text));
	text[1] = '.';
	memcpy(text + 1000, ": 1", 4);
	CHECK(has_error_at(text, 1, 2, bare_message));
	memcpy(text + 1000, "a: 1", 5);
	CHECK(has_error_at(text, 1, 2, other_message));

	// A quoted key may hold a dot.
	CHECK(has_canonical_form("'a.b': 1", "'a.b': 1\n"));
	CHECK(has_canonical_form("\"example.com\": 1", "'example.com': 1\n"));
}

static void test_error_hints(void) {
	// A key that starts with, or holds, a character a bare key cannot hold is quoted, but a character with a meaning in the grammar, whitespace, or a key that is quoted already gets no hint.
	CHECK(has_error_message_part("$schema: 1", "must be quoted"));
	CHECK(has_error_message_part("a$: 1", "must be quoted"));
	CHECK(has_error_message_part("\xC3\xA9: 1", "must be quoted"));
	CHECK(has_error_message_part("a\xF0\x9F\x98\x80: 1", "must be quoted"));
	CHECK(is_rejected_without_part("'a'$: 1", "must be quoted"));
	CHECK(is_rejected_without_part("a: 1\n}", "must be quoted"));
	CHECK(is_rejected_without_part("a{: 1", "must be quoted"));
	CHECK(is_rejected_without_part("x: 1\ny/*", "must be quoted"));
	CHECK(is_rejected_without_part("\xC2\xA0" "a: 1", "must be quoted"));
	CHECK(is_rejected_without_part("\xE2\x80\xA8" "a: 1", "must be quoted"));
	CHECK(is_rejected_without_part("\xC2\x85" "a: 1", "must be quoted"));
	CHECK(is_rejected_without_part("a: 1\n\xE2\x80\x8B" "b: 1", "must be quoted"));
	CHECK(is_rejected_without_part("a: 1\n\xEF\xBB\xBF" "b: 1", "must be quoted"));

	// A leading zero is usually meant as octal or as an identifier, which only digits alone can be.
	CHECK(has_error_message_part("mode: 0644", "prefix 0o"));
	CHECK(is_rejected_without_part("zip: 08901", "prefix 0o") && has_error_message_part("zip: 08901", "ZIP code"));
	CHECK(is_rejected_without_part("a: 01.5", "ZIP code"));
	CHECK(is_rejected_without_part("a: -01", "ZIP code"));

	// A number longer than 1,000 characters gets no octal suggestion, as in the reference implementation, also when the digit after its first 1,000 characters is octal.
	static const char long_zero_endings[] = {'8', '7'};

	for (size_t index = 0; index < sizeof(long_zero_endings); index++) {
		char long_zeros[1200];
		size_t length = (size_t)snprintf(long_zeros, sizeof(long_zeros), "a: 0");
		memset(long_zeros + length, '0', 1100);
		snprintf(long_zeros + length + 1100, sizeof(long_zeros) - length - 1100, "%c", long_zero_endings[index]);
		CHECK(has_error_at(long_zeros, 1, 4, "Leading zeros are not allowed in a decimal number. Write an identifier, such as a ZIP code, as a string") && is_rejected_without_part(long_zeros, "prefix 0o"));
	}

	// A block comment between a key and its `:` is named, when only spaces and tabs come between the comment and the `:`.
	CHECK(has_error_at("a /* x\ny */: 1", 1, 3, "A comment is not allowed"));
	CHECK(has_error_at("a/* c */: 1", 1, 2, "A comment is not allowed between a key and its"));
	CHECK(has_error_at("a /* c */: 1", 1, 3, "A comment is not allowed"));
	CHECK(has_error_at("a /* c */ : 1", 1, 3, "A comment is not allowed"));
	CHECK(has_error_at("{a /* x */: 1}", 1, 4, "A comment is not allowed"));
	CHECK(has_error_at("'a'/**/:1", 1, 4, "A comment is not allowed"));
	CHECK(has_error_at("x: 1\na /* c */: 2", 2, 3, "A comment is not allowed"));
	CHECK(has_error_at("a /* c */ x: 1", 1, 3, "Expected “:” after the key"));
	CHECK(has_error_at("a /* c", 1, 3, "Expected “:” after the key"));
	CHECK(has_error_at("a /* c */\n: 1", 1, 3, "Expected “:” after the key"));
	CHECK(has_error_at("a # c\n: 1", 1, 3, "Expected “:” after the key"));

	// The four-digit form of JSON is named only when four hexadecimal digits follow, and an uppercase digit is named before too many digits.
	CHECK(has_error_at("a: \"\\u1234\"", 1, 5, "The four-digit"));
	CHECK(has_error_at("a: \"\\uABCD\"", 1, 5, "The four-digit"));
	CHECK(has_error_at("a: \"\\u00e9\"", 1, 5, "The four-digit"));

	// A literal string for backslashes is ''' in a block, which a '...' string cannot be, and '...' on one line and in a key, which a block string cannot be.
	CHECK(has_error_at("a: \"\"\"\n\tC:\\Users\n\t\"\"\"", 2, 4, "use a ''' string for literal backslashes"));
	CHECK(has_error_at("a: \"\"\"\n\tx\\\n\ty\n\t\"\"\"", 2, 3, "or a ''' string"));
	CHECK(has_error_at("a: \"C:\\Users\"", 1, 7, "use a '...' string for literal backslashes"));
	CHECK(has_error_at("a: \"x\\", 1, 6, "or a '...' string"));
	CHECK(has_error_at("\"C:\\Users\": 1", 1, 4, "use a '...' string for literal backslashes"));
	CHECK(has_error_at("{\"x\\q\": 1}", 1, 4, "use a '...' string"));
	CHECK(is_rejected_without_part("\"C:\\Users\": 1", "'''"));
	CHECK(is_rejected_without_part("a: \"x\\", "'''"));

	// The suggested escape must be valid, so a four-digit surrogate pair is one escape for its character, and a lone surrogate and a carriage return have no escape.
	CHECK(has_error_at("a: \"\\uD83D\\uDE00\"", 1, 5, "a surrogate pair is one character. Write it as one \\u{…} escape"));
	CHECK(has_error_at("a: \"\\ud83d\\ude00\"", 1, 5, "a surrogate pair is one character"));
	CHECK(has_error_at("a: \"\\uD800\\uDC00\"", 1, 5, "a surrogate pair is one character"));
	CHECK(has_error_at("a: \"\\uDBFF\\uDFFF\"", 1, 5, "a surrogate pair is one character"));
	CHECK(has_error_at("a: \"\\uD83D\"", 1, 5, "a lone surrogate is not a Unicode scalar value"));
	CHECK(has_error_at("a: \"\\uDE00\"", 1, 5, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\uDE00\\uD83D\"", 1, 5, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\uD83D\\uD83D\"", 1, 5, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\uD83D\\u0041\"", 1, 5, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\uD83D\\u{de00}\"", 1, 5, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\uD83D\\uDE0\"", 1, 5, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\uD83Dx\"", 1, 5, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\uD83D", 1, 5, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\uD7FF\"", 1, 5, "Write \\u{…} with lowercase"));
	CHECK(has_error_at("a: \"\\uE000\"", 1, 5, "Write \\u{…} with lowercase"));
	CHECK(has_error_at("a: \"\\u000D\"", 1, 5, "a carriage return (U+000D) cannot be represented"));
	CHECK(has_error_at("a: \"\\u000d\"", 1, 5, "a carriage return"));
	CHECK(has_error_at("a: \"\\u000C\"", 1, 5, "Write \\u{…} with lowercase"));
	CHECK(has_error_at("\"\\u000D\": 1", 1, 2, "a carriage return"));
	CHECK(has_error_at("a: \"\"\"\n\\uD83D\n\"\"\"", 2, 1, "a lone surrogate"));
	CHECK(has_error_at("a: \"\\u1\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\\u12\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\\u123\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\\uA\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\\u123g\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\\u\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\\u{1234567A}\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{E9}\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{e9F}\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{1234567}\"", 1, 5, "at most six"));
	CHECK(has_error_at("a: \"\\u{}\"", 1, 5, "one to six"));
	CHECK(has_error_at("a: \"\\u{g}\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\\u{aG}\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\\u{12\"", 1, 5, "A Unicode escape is written"));
	CHECK(has_error_at("a: \"\"\"\n\\u12\n\"\"\"", 2, 1, "A Unicode escape is written"));

	// The value is checked before the spelling, so that the leading zeros error never suggests an escape that is not allowed either.
	CHECK(has_error_at("a: \"\\u{0d}\"", 1, 5, "carriage return"));
	CHECK(has_error_at("a: \"\\u{0000d}\"", 1, 5, "carriage return"));
	CHECK(has_error_at("a: \"\\u{0d800}\"", 1, 5, "surrogate"));
	CHECK(has_error_at("a: \"\\u{0dfff}\"", 1, 5, "surrogate"));
	CHECK(has_error_at("\"\\u{0d}\": 1", 1, 2, "carriage return"));
	CHECK(has_error_at("a: \"\"\"\n\\u{00d}\n\"\"\"", 2, 1, "carriage return"));
	CHECK(has_error_at("a: \"\"\"\n\tx\\u{0d800}\n\t\"\"\"", 2, 3, "surrogate"));
	CHECK(has_error_at("a: \"\\u{d}\"", 1, 5, "carriage return"));
	CHECK(has_error_at("a: \"\\u{d800}\"", 1, 5, "surrogate"));
	CHECK(has_error_at("a: \"\\u{110000}\"", 1, 5, "above U+10FFFF"));

	// The same holds for uppercase digits: lowercasing them must give an escape that is allowed.
	CHECK(has_error_at("a: \"\\u{D}\"", 1, 5, "carriage return"));
	CHECK(has_error_at("a: \"\\u{00D}\"", 1, 5, "carriage return"));
	CHECK(has_error_at("a: \"\\u{D800}\"", 1, 5, "surrogate"));
	CHECK(has_error_at("a: \"\\u{dFfF}\"", 1, 5, "surrogate"));
	CHECK(has_error_at("a: \"\\u{11FFFF}\"", 1, 5, "above U+10FFFF"));
	CHECK(has_error_at("\"\\u{D83D}\": 1", 1, 2, "surrogate"));
	CHECK(has_error_at("a: \"\"\"\n\\u{DE00}\n\"\"\"", 2, 1, "surrogate"));
	CHECK(has_error_at("a: \"\\u{E9}\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{0A}\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{10FFFF}\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{D7FF}\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{E000}\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{D800\"", 1, 5, "lowercase"));
	CHECK(has_error_at("a: \"\\u{D800000}\"", 1, 5, "lowercase"));

	// An escape whose value is allowed gets the leading zeros error, because removing the zeros makes it valid.
	CHECK(has_error_at("a: \"\\u{00}\"", 1, 5, "leading zeros"));
	CHECK(has_error_at("a: \"\\u{0e9}\"", 1, 5, "leading zeros"));
	CHECK(has_error_at("a: \"\\u{0d7ff}\"", 1, 5, "leading zeros"));
	CHECK(has_error_at("a: \"\\u{0e000}\"", 1, 5, "leading zeros"));
	CHECK(has_error_at("a: \"\\u{010fff}\"", 1, 5, "leading zeros"));
	CHECK(has_error_at("a: \"\\u{0c}\"", 1, 5, "leading zeros"));

	// A character that looks like whitespace, or is invisible whitespace, is said to be one wherever an error points at it, because the line looks right.
	static const char *const lookalike_spaces[] = {"\xC2\x85", "\xC2\xA0", "\xE1\x9A\x80", "\xE2\x80\x80", "\xE2\x80\x8A", "\xE2\x80\x8B", "\xE2\x80\xA8", "\xE2\x80\xA9", "\xE2\x80\xAF", "\xE2\x81\x9F", "\xE3\x80\x80", "\xEF\xBB\xBF"};

	for (size_t index = 0; index < sizeof(lookalike_spaces) / sizeof(lookalike_spaces[0]); index++) {
		char text[32];
		snprintf(text, sizeof(text), "a: 1%s", lookalike_spaces[index]);
		CHECK(has_error_at(text, 1, 5, "only space, tab, and line feed are whitespace"));
	}

	CHECK(has_error_at("a: 1\xC2\xA0", 1, 5, "Unexpected character after a value"));
	CHECK(has_error_at("a:\xC2\xA0" "1", 1, 3, "Expected a value. The character there looks like whitespace"));
	CHECK(has_error_at("\xC2\xA0" "a: 1", 1, 1, "Expected a key. The character there looks like whitespace"));
	CHECK(has_error_at("a: 1\n\xC2\xA0\nb: 2", 2, 1, "Expected a key. The character there looks like whitespace"));
	CHECK(has_error_at("a\xC2\xA0: 1", 1, 2, "Expected “:” after the key. The character there looks like whitespace"));
	CHECK(has_error_at("a: 1 \xC2\xA0" "b: 2", 1, 6, "Expected a line break before the next entry. The character there looks like whitespace"));
	CHECK(has_error_at("[1 /*\n*/\xC2\xA0" "2]", 2, 3, "only space, tab, and line feed are whitespace"));
	CHECK(has_error_at("[1 /*\n*/\xC2\xA0" "2]", 2, 3, "A line break inside a block comment does not separate items"));
	CHECK(has_error_at("{a: 1 /* c */\xC2\xA0}", 1, 14, "only space, tab, and line feed are whitespace"));
	CHECK(has_error_at("[]\xC2\xA0", 1, 3, "Unexpected content after the end of the document. The character there looks like whitespace"));

	// Other invisible characters, and visible ones, are not whitespace that someone would mistake for a space.
	CHECK(is_rejected_without_part("a: 1\xC2\xAD", "whitespace"));
	CHECK(is_rejected_without_part("a: 1\xE2\x80\x8D", "whitespace"));
	CHECK(is_rejected_without_part("a: 1x", "whitespace"));
	CHECK(is_rejected_without_part("a: 1 b: 2", "whitespace"));

	// A word and a `:` where a value should be are a key, except when the word is the value right after its member's `:` and a space. Then they are an unquoted string, such as a URL or a message with a colon.
	CHECK(has_error_at("url: https://example.com", 1, 6, "Unexpected word"));
	CHECK(has_error_at("a: localhost:8080", 1, 4, "Unexpected word"));
	CHECK(has_error_at("msg: Error: file not found", 1, 6, "Unexpected word"));
	CHECK(has_error_at("a: b: 1", 1, 4, "Unexpected word"));
	CHECK(has_error_at("a:\tb: 1", 1, 4, "Unexpected word"));
	CHECK(has_error_at("{a: b:}", 1, 5, "Unexpected word"));
	CHECK(has_error_at("a: b:", 1, 4, "Unexpected word"));
	CHECK(has_error_at("a: b:c", 1, 4, "Unexpected word"));
	CHECK(has_error_at("a:\nhttps://x", 2, 1, "found a key"));
	CHECK(has_error_at("a:\nb:1\nc: 2", 2, 1, "found a key"));
	CHECK(has_error_at("[a:1]", 1, 2, "found a key"));
	CHECK(has_error_at("x: [a:b, c]", 1, 5, "found a key"));
	CHECK(has_error_at("a: [https://x, true]", 1, 5, "found a key"));
	CHECK(has_error_at("[a: 1]", 1, 2, "found a key"));
	CHECK(has_error_at("[1, a: 1]", 1, 5, "found a key"));
	CHECK(has_error_at("a: /* c */ b: 1", 1, 12, "found a key"));

	// The look back for the `:` stops at the start of the text, where a whole document that is a word is read as a value.
	CHECK(has_error_at(" b c", 1, 4, "Expected “:” after the key"));
	CHECK(has_error_at("\tb:c d", 1, 4, "Unexpected word"));

	// `M` is not a month, and with a number alone it is more often a size.
	CHECK(has_error_message_part("memory: 512M", "A size"));
	CHECK(has_error_message_part("a: 1h6M", "month") && is_rejected_without_part("a: 1h6M", "A size"));
	CHECK(has_error_message_part("a: 1MS", "lowercase"));
}

static void test_suggestions(void) {
	// A suggestion to quote an unquoted string quotes the text to the end of the line, a separator, a closing bracket, or a comment after a space, so it has no example when that text holds a `'`, which a '...' string cannot hold.
	CHECK(has_error_at("name: John Smith", 1, 7, "Unexpected word. A string value must be quoted, as in 'text'"));
	CHECK(has_error_at("a: hello world # it's", 1, 4, "as in 'text'"));
	CHECK(has_error_at("a: it's here", 1, 4, "Unexpected word. A string value must be quoted") && is_rejected_without_part("a: it's here", "as in"));

	// A keyword hint is only for the word on its own, because following it would leave text behind or change the value.
	CHECK(has_error_at("a: Yes", 1, 4, "Booleans are written true and false"));
	CHECK(has_error_at("a: None # Unset", 1, 4, "Null is written null"));
	CHECK(has_error_at("a: [NaN, 1]", 1, 5, "NaN is not representable"));
	CHECK(has_error_at("a: Yes please", 1, 4, "Unexpected word"));
	CHECK(has_error_at("status: On hold", 1, 9, "Unexpected word"));
	CHECK(has_error_at("name: Nan Goldin", 1, 7, "Unexpected word"));
	CHECK(has_error_at("a: [Inf loop, 1]", 1, 5, "Unexpected word"));

	// A value that starts with "." is only a number when a digit follows.
	CHECK(has_error_at("a: .5e3", 1, 4, "A number cannot begin with “.”"));
	CHECK(has_error_at("a: .env", 1, 4, "Unexpected word"));
	CHECK(has_error_at("files: [.env, .git]", 1, 9, "Unexpected word"));
	CHECK(has_error_at("a: .", 1, 4, "Unexpected word"));

	// The octal suggestion is only for the number on its own, and only when it is in the 64-bit range.
	CHECK(is_rejected_without_part("phone: 0412 345 678", "prefix 0o") && has_error_at("phone: 0412 345 678", 1, 8, "ZIP code"));
	CHECK(is_rejected_without_part("date: 01/02/2026", "prefix 0o") && has_error_message_part("date: 01/02/2026", "ZIP code"));
	CHECK(has_error_message_part("a: 0777777777777777777777", "prefix 0o"));
	CHECK(has_error_message_part("a: 0000777777777777777777777", "prefix 0o"));
	CHECK(is_rejected_without_part("a: 01000000000000000000000", "prefix 0o") && has_error_message_part("a: 01000000000000000000000", "ZIP code"));

	// The uppercase hexadecimal spelling is only suggested when it is valid: a misplaced underscore is reported first, and a value outside the 64-bit range gets no example.
	CHECK(has_error_at("a: 0xff_ff", 1, 4, "Hexadecimal digits are uppercase, as in"));
	CHECK(has_error_message_part("a: 0x0000_7fff_ffff_ffff_ffff", "Hexadecimal digits are uppercase, as in"));
	CHECK(has_error_at("a: 0x_ff", 1, 4, "An underscore in a number must be between two digits"));
	CHECK(has_error_message_part("a: 0xff_", "An underscore in a number must be between two digits"));
	CHECK(has_error_message_part("a: 0xf__f", "An underscore in a number must be between two digits"));
	CHECK(has_error_message_part("a: 0xffffffffffffffff", "Hexadecimal digits are uppercase") && is_rejected_without_part("a: 0xffffffffffffffff", "as in"));
	CHECK(has_error_message_part("a: 0x8000000000000000a", "Hexadecimal digits are uppercase") && is_rejected_without_part("a: 0x8000000000000000a", "as in"));

	// The whole token is checked, also when the lowercase digits come after its first 1,000 characters.
	static const char *const long_hex_endings[] = {"ff", "f_f", "f__f"};
	static const char *const long_hex_reasons[] = {"Hexadecimal digits are uppercase", "Hexadecimal digits are uppercase", "An underscore in a number must be between two digits"};

	for (size_t index = 0; index < sizeof(long_hex_endings) / sizeof(long_hex_endings[0]); index++) {
		char long_hex[1200];
		size_t length = (size_t)snprintf(long_hex, sizeof(long_hex), "a: 0x");
		memset(long_hex + length, '1', 1100);
		snprintf(long_hex + length + 1100, sizeof(long_hex) - length - 1100, "%s", long_hex_endings[index]);
		CHECK(has_error_at(long_hex, 1, 4, long_hex_reasons[index]) && is_rejected_without_part(long_hex, "as in"));
	}

	// A missing `:` is only explained as a key with spaces when whitespace or the end follows the `:`, because quoting the words before the `:` in `server localhost:8080` gives another meaning. A `=` after a key was meant as the `:`, so it is no reason to quote the key.
	CHECK(has_error_at("server localhost:8080", 1, 8, "Expected “:” after the key"));
	CHECK(has_error_at("the name:1", 1, 5, "Expected “:” after the key"));
	CHECK(has_error_at("the name:\t1", 1, 1, "A bare key cannot contain spaces"));
	CHECK(has_error_at("the name:", 1, 1, "A bare key cannot contain spaces"));
	CHECK(has_error_at("name=foo", 1, 5, "Expected “:” after the key") && is_rejected_without_part("name=foo", "must be quoted"));
	CHECK(has_error_at("{a=1}", 1, 3, "Expected “:” after the key") && is_rejected_without_part("{a=1}", "must be quoted"));

	// A key that contains a `:` ends at the first one, and the rest is read as its value.
	CHECK(has_error_at("12:30: 'lunch'", 1, 1, "A key that contains “:” must be quoted"));
	CHECK(has_error_at("{2026-09-19T14:00:00Z: 1}", 1, 2, "A key that contains “:” must be quoted"));
	CHECK(has_error_at("a:b", 1, 3, "Unexpected word"));
	CHECK(has_error_at("a:{b: nope}", 1, 7, "Unexpected word"));

	// The checks of a bad number follow the order of the reference implementation: only a number ends with its exponent marker, so a token that only ends with an “e” is named by its sign and its dots, and underscores before a point count as its digits.
	CHECK(has_error_at("a: -e", 1, 4, "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_at("a: 1.2.e", 1, 4, "several dots"));
	CHECK(has_error_at("a: -infe", 1, 4, "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_at("a: [-e-]", 1, 5, "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_at("a: -_.", 1, 4, "A decimal point must be followed by a digit"));
	CHECK(has_error_at("a: 1.2.3", 1, 4, "several dots"));

	// A time of day, and other digits with colons, are strings.
	CHECK(has_error_at("a: 12:00", 1, 4, "A time of day is a string"));
	CHECK(has_error_at("a: 10:30 PM", 1, 4, "A time of day is a string"));
	CHECK(has_error_at("a:\n\t07:32:00", 2, 2, "A time of day is a string"));
	CHECK(has_error_at("a: 8080:80", 1, 4, "A value that contains “:” must be quoted"));

	// A value that starts with "#" is a comment.
	CHECK(has_error_at("color: #FFF\nsize: 1", 2, 1, "found a key. “#” starts a comment"));
	CHECK(has_error_at("{color: #FFF\nsize: 1}", 2, 1, "found a key. “#” starts a comment"));
	CHECK(has_error_at("color: #FFF", 1, 12, "reached the end of the document. “#” starts a comment"));
	CHECK(has_error_at("color: #FFF, size: 1", 1, 21, "reached the end of the document. “#” starts a comment"));
	CHECK(has_error_at("{color: #FFF\n}", 2, 1, "Expected a value. “#” starts a comment"));
	CHECK(has_error_at("color: # The color\nsize: 1", 2, 1, "Was a value left out?"));

	// Indentation does not nest objects, and a "-" list is not an array.
	CHECK(has_error_at("server:\n  port: 80", 2, 3, "Indentation does not nest objects"));
	CHECK(has_error_at("'the server':\n  'the port': 80", 2, 3, "Indentation does not nest objects"));
	CHECK(has_error_at("{\n\ta:\n\tb: 1\n}", 3, 2, "Was a value left out?"));
	CHECK(has_error_at("ports:\n  - 80\n  - 443", 2, 3, "a “-” list"));
	CHECK(has_error_at("ports:\n- 80", 2, 1, "a “-” list"));

	// A unit after a number and a space, when only the end of the value follows it, which is also reported before a collision of its entry.
	CHECK(has_error_at("a: 512 MiB", 1, 8, "A unit cannot follow a number after a space"));
	CHECK(has_error_at("a: 1.5 h # note", 1, 8, "A unit cannot follow a number after a space"));
	CHECK(has_error_at("a: [100 ms]", 1, 9, "A unit cannot follow a number after a space"));
	CHECK(has_error_at("a: {b: 100 ms}", 1, 12, "A unit cannot follow a number after a space"));
	CHECK(has_error_at("a: 1\na: 2 s", 2, 6, "A unit cannot follow a number after a space"));
	CHECK(has_error_at("a: [1 true]", 1, 7, "after an array item"));
	CHECK(has_error_at("a: 5 and more", 1, 6, "Expected a line break before the next entry"));
	CHECK(has_error_at("a: 'x' MiB", 1, 8, "Expected a line break before the next entry"));

	// Microseconds written with the micro sign or the Greek letter mu, and a doubled quote in a '...' string.
	CHECK(has_error_at("a: 1.5\xC2\xB5s", 1, 7, "The unit for microseconds is written us"));
	CHECK(has_error_at("a: 3\xCE\xBCs", 1, 5, "The unit for microseconds is written us"));
	CHECK(has_error_at("a: 0x10\xC2\xB5s", 1, 8, "The unit for microseconds is written us"));
	CHECK(has_error_at("a: 'it''s'", 1, 8, "There is no '' escape in a '...' string"));

	// A `//` comment, and a YAML block scalar.
	CHECK(has_error_at("// note\na: 1", 1, 1, "Expected a key, but found “/”. A comment starts with “#”"));
	CHECK(has_error_at("a: 1 // note", 1, 6, "Expected a line break before the next entry, but found “/”. A comment starts with “#”"));
	CHECK(has_error_at("a: [1 // note\n]", 1, 7, "after an array item, but found “/”. A comment starts with “#”"));
	CHECK(has_error_at("a: {b: 1 // note\n}", 1, 10, "after an object member, but found “/”. A comment starts with “#”"));
	CHECK(has_error_at("a: [1, // note\n2]", 1, 8, "Expected a value, but found “/”. A comment starts with “#”"));
	CHECK(is_rejected_without_part("a: 1 / 2", "comment"));
	CHECK(has_error_at("key: |\n  text", 1, 6, "Write a multiline string as a block string"));
	CHECK(has_error_at("key: |-\n  text", 1, 6, "Write a multiline string as a block string"));
	CHECK(is_rejected_without_part("key: >-\n  text", "block string"));
	CHECK(is_rejected_without_part("key: | x", "block string"));

	// A table header, only where a table could be written instead, and only for a name that is a valid key.
	CHECK(has_error_at("[server]\nport: 1", 1, 2, "There are no table headers"));
	CHECK(has_error_at("# The server.\n\t[server]\nport: 1", 2, 3, "There are no table headers"));
	CHECK(has_error_at("[[servers]]\nport: 1", 1, 3, "There are no table headers"));
	CHECK(has_error_at("[server.http]\nport: 1", 1, 2, "Unexpected word"));
	CHECK(has_error_at("name: 'x'\n\n[server]\nport: 1", 3, 1, "There are no table headers"));
	CHECK(has_error_at("a: {\n\t[server]\n}", 2, 2, "There are no table headers"));
	CHECK(has_error_at("a: [server]", 1, 5, "Unexpected word"));
	CHECK(has_error_at("roles: [\n\t[admin]\n]", 2, 3, "Unexpected word"));
	CHECK(has_error_at("[\n\t[[x]]\n]", 2, 4, "Unexpected word"));
	CHECK(has_error_at("a:\n[server]\nport: 1", 2, 2, "Unexpected word"));
	CHECK(has_error_at("[a..b]\nport: 1", 1, 2, "Unexpected word"));
	CHECK(has_error_at("x: 1\n[a.]\nport: 1", 2, 1, "Expected a key") && is_rejected_without_part("x: 1\n[a.]\nport: 1", "table"));

	// A content line that ends with the delimiter of an unterminated block string, but not a line of only quotes, which is content.
	CHECK(has_error_at("a: '''\nWelcome!'''", 1, 4, "Unterminated block string. Its closing delimiter must start a line"));
	CHECK(has_error_at("a: \"\"\"\n\tHello\n\tWelcome!\"\"\"  \nb: 1", 1, 4, "Its closing delimiter must start a line"));
	CHECK(has_error_at("a: '''\nx", 1, 4, "Unterminated block string") && is_rejected_without_part("a: '''\nx", "closing delimiter"));
	CHECK(is_rejected_without_part("a: '''\n''''", "closing delimiter"));

	// A comma as a thousands separator, which makes a leading zero, suggests the number only when it is a valid int.
	CHECK(has_error_at("a: [1,000]", 1, 7, "A comma separates items, so a number with commas between its digits is not one number. Write it with underscores"));
	CHECK(has_error_at("a: [-1,000]", 1, 8, "Write it with underscores"));
	CHECK(has_error_at("a: [12,345,000]", 1, 12, "Write it with underscores"));
	CHECK(has_error_message_part("a: [9,223,372,036,854,775,807]", "Write it with underscores"));
	CHECK(has_error_message_part("a: [-9,223,372,036,854,775,808]", "Write it with underscores"));
	CHECK(has_error_message_part("a: [9,223,372,036,854,775,808]", "A comma separates items") && is_rejected_without_part("a: [9,223,372,036,854,775,808]", "Write it"));
	CHECK(has_error_message_part("a: [-9,223,372,036,854,775,809]", "A comma separates items") && is_rejected_without_part("a: [-9,223,372,036,854,775,809]", "Write it"));
	CHECK(has_error_message_part("a: [0,000]", "A comma separates items") && is_rejected_without_part("a: [0,000]", "Write it"));
	CHECK(has_error_message_part("a: [0x1,000]", "prefix 0o"));
	CHECK(has_error_message_part("a: [1, 000]", "prefix 0o"));

	// The diagnostic also handles groups whose first digits start at byte zero, and refuses one with no number before it, without wrapping a backward lookup.
	soml__parser parser = {.text = (const unsigned char *)"123,000", .length = 7};
	const char *message = soml__describe_thousands_separator(&parser, 4, 3);
	CHECK(message != nullptr && strstr(message, "Write it with underscores") != nullptr);
	parser = (soml__parser){.text = (const unsigned char *)"1,000", .length = 5};
	message = soml__describe_thousands_separator(&parser, 2, 3);
	CHECK(message != nullptr && strstr(message, "Write it with underscores") != nullptr);
	parser = (soml__parser){.text = (const unsigned char *)",000", .length = 4};
	CHECK(soml__describe_thousands_separator(&parser, 1, 3) == nullptr);
	CHECK(is_rejected("000", SOML_ERROR_SYNTAX));

	// A date and a time with a space between them are only shown as an instant when it is valid, and a local time is described as one.
	CHECK(has_error_at("a: 1979-05-27 07:32:00Z", 1, 4, "not a space, as in"));
	CHECK(has_error_at("a: [1979-05-27 07:32:00.5+02:00]", 1, 5, "not a space, as in"));
	CHECK(has_error_message_part("a: 2024-02-29 14:00:00", "for UTC"));
	CHECK(has_error_message_part("a: 2026-02-30 14:00:00Z", "not a space. An instant is written"));
	CHECK(has_error_message_part("a: 2026-01-01 14:00:00.1234567890Z", "not a space. An instant is written"));
	CHECK(has_error_message_part("a: 2026-01-01 14:00:00-00:00", "not a space. An instant is written"));
	CHECK(has_error_message_part("a: 0001-01-01 00:00:00+01:00", "not a space. An instant is written"));
	CHECK(has_error_message_part("a: 2026-02-30 14:00:00", "not a space. An instant is written"));
	CHECK(has_error_message_part("a: 0000-01-01 00:00:00", "not a space. An instant is written"));
	CHECK(has_error_message_part("a: 1979-05-27 07:32:00,5Z", "not a space. An instant is written"));
	CHECK(has_error_message_part("a: 1979-05-27 07:32", "not a space. An instant is written"));
	CHECK(has_error_message_part("a: 1979-05-27 # note", "A date is not an instant"));

	// A date alone is only shown as an instant when the date exists.
	CHECK(has_error_message_part("a: 2024-02-29", "An instant needs a time and an offset"));
	CHECK(has_error_message_part("a: 2026-02-30", "A date is not an instant") && is_rejected_without_part("a: 2026-02-30", "An instant needs"));
	CHECK(is_rejected_without_part("a: 2026-13-01", "An instant needs"));
	CHECK(is_rejected_without_part("a: 0000-01-01", "An instant needs"));
	CHECK(has_error_message_part("a: 2026-09-19T14:00:00.5", "A date and time without an offset is a local time"));
	CHECK(has_error_message_part("a: 2026-09-19T14:00:00[Europe/Oslo]", "An instant needs an offset") && is_rejected_without_part("a: 2026-09-19T14:00:00[Europe/Oslo]", "local time"));

	// A date and time without an offset is only told to add one when that gives an instant, so one whose date or time does not exist gets that error, at the token.
	CHECK(has_error_at("a: 2026-09-19T25:00:00", 1, 4, "the hour must be 00 to 23"));
	CHECK(has_error_at("a: 2026-02-30T14:00:00", 1, 4, "the day does not exist in that month"));
	CHECK(has_error_at("a: 2026-13-01T14:00:00.5", 1, 4, "the month must be 01 to 12"));
	CHECK(has_error_at("a: 0000-01-01T00:00:00", 1, 4, "the year must be 0001 to 9999"));
	CHECK(has_error_at("a: 2026-09-19T14:60:00", 1, 4, "the minute must be 00 to 59"));
	CHECK(has_error_at("a: 2026-09-19T14:00:60 # leap", 1, 4, "the second must be 00 to 59"));
	CHECK(has_error_at("a: 2026-09-19T14:00:00.1234567890", 1, 4, "at most nine digits"));
	CHECK(has_error_at("[2026-09-19T24:00:00[Europe/Oslo]]", 1, 2, "the hour must be 00 to 23"));
	CHECK(has_error_at("a: 2024-02-29T23:59:59.999999999", 1, 4, "or add the offset it was meant in"));
	CHECK(has_error_at("a: 9999-12-31T23:59:59", 1, 4, "or add the offset it was meant in"));
	CHECK(has_error_at("a: 0001-01-01T00:00:00", 1, 4, "or add the offset it was meant in"));

	// The same holds after a space, as TOML writes a date and time.
	CHECK(is_rejected_without_part("a: 2026-09-19 25:00:00", "local time"));
}

/*
Errors that point to a fix that would not help. The messages are the reference implementation's, without the text it quotes.
*/
static void test_errors_that_do_not_mislead(void) {
	// An offset after a space is the offset an instant was meant in, so the instant is not a local time to quote as a string.
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 Z", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 +02:00", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00.5 -07:30", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00\t\tZ # note", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: [2026-09-19T14:00:00 Z, 1]", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: {b: 2026-09-19T14:00:00 Z}", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: [\n\t2026-09-19T14:00:00 Z\n]", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: 2026-02-30T14:00:00 Z", "An instant's offset follows its time directly, without a space"));
	CHECK(has_error_reason("a: 0001-01-01T00:00:00 +00:01", "An instant's offset follows its time directly, without a space"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 +24:00", "An instant's offset follows its time directly, without a space"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 +02:00 # é", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 # Z", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 Zulu", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 +02:00:00", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 +0200", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00\nZ: 1", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00 Z", "Invalid instant. An instant is written as 2026-09-19T14:00:00Z, with an optional fraction of up to nine digits and an offset of Z or ±HH:MM"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00Z Z", "Expected a line break before the next entry"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 Montréal", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 é", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: [2026-09-19 😀😀]", "A date is not an instant. Write a date as a string, as in '2026-09-19'. An instant needs a time and an offset, as in 2026-09-19T00:00:00Z"));
	CHECK(has_error_reason("a: 2026-09-19 Montréal", "A date is not an instant. Write a date as a string, as in '2026-09-19'. An instant needs a time and an offset, as in 2026-09-19T00:00:00Z"));
	CHECK(has_error_reason("a: 2026-09-19T14:00 x日本", "Invalid instant. An instant is written as 2026-09-19T14:00:00Z, with an optional fraction of up to nine digits and an offset of Z or ±HH:MM"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 Zé", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 +02:0é", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 1979-05-27 07:32:00 Z", "The date and time separator in an instant is an uppercase “T”, not a space, and an instant needs the offset it was meant in, as in 2026-09-19T14:00:00Z for UTC. A date and time without an offset is a local time, which is not an instant, so write it as a string, as in '2026-09-19 14:00:00'"));

	// A value with a “:” that starts with a zero, such as a MAC address, is not a number with a leading zero.
	CHECK(has_error_reason("mac: 00:1A:2B:3C:4D:5E", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("mac: 01:23:45:67:89:AB", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 0123:abcd::1", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: [00:1A:2B, 1]", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: -01:30", "Invalid number"));
	CHECK(has_error_reason("a: 01", "Leading zeros are not allowed in a decimal number. Write an octal number, such as a file mode, with the prefix 0o, and an identifier, such as a ZIP code, as a string"));
	CHECK(has_error_reason("a: 01A", "Leading zeros are not allowed in a decimal number"));
	CHECK(has_error_reason("a: 00.5", "Leading zeros are not allowed in a decimal number"));
	CHECK(has_error_reason("a: 00:11:22:33:44:55", "Invalid number. A value that contains “:” must be quoted"));
	CHECK(has_error_reason("a: 08:30", "A time of day is a string, so it must be quoted"));
	CHECK(has_error_reason("a: 1:", "Invalid number"));
	CHECK(has_error_reason("a: 2001:db8::1", "Invalid number. A string value must be quoted"));

	// A duration in years is reported as one, as one in days or weeks is, and a word that only starts with a year unit is a string to quote.
	CHECK(has_error_reason("a: 1y", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1y, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 2years", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [2years, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 1year", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1year, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 3yr", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [3yr, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 3yrs", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [3yrs, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 1Y", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1Y, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 1YEAR", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1YEAR, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 1.5y", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1.5y, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 1_000y", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1_000y, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: -1y", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [-1y, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 1y6m", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1y6m, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 1h1y", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1h1y, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 1y1y", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: [1y1y, 1]", "Invalid duration: there is no year unit, because a year is not a fixed length. Write 8760h for a fixed 365 days"));
	CHECK(has_error_reason("a: 100yen", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1yard", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 5yo", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1yrsx", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1yearsx", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1y.5", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1 y", "A unit cannot follow a number after a space. Write a duration without the space, as in 10s, and anything else, such as a size, as a string, as in '512 MiB'"));

	// Only a number ends with its exponent marker, so a word that ends with an “e” has no exponent.
	CHECK(has_error_reason("a: 7zip-bin-name", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1byte", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 3-phase", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1-time", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: [100-pre, 1]", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 2e-e", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: -verbose", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("args: [-recursive]", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("a: 1e", "Expected digits after the exponent marker “e”"));
	CHECK(has_error_reason("a: 1e-", "Expected digits after the exponent marker “e”"));
	CHECK(has_error_reason("a: -1e", "Expected digits after the exponent marker “e”"));
	CHECK(has_error_reason("a: 1.5e", "Expected digits after the exponent marker “e”"));
	CHECK(has_error_reason("a: 1.5e-", "Expected digits after the exponent marker “e”"));
	CHECK(has_error_reason("a: -1_0.5_0e", "Expected digits after the exponent marker “e”"));
	CHECK(has_error_reason("a: 0e", "Expected digits after the exponent marker “e”"));
	CHECK(has_error_reason("a: 0.0e-", "Expected digits after the exponent marker “e”"));

	// A word after “-” is only NaN or infinity when it is the whole word, as for one without the “-”.
	CHECK(has_error_reason("a: -nano", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("a: -nanny", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("a: -info", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("a: -INFO", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("args: [-inform, PEM]", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("a: -infinite", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("a: -infinityx", "Expected a digit or “infinity” after “-”"));
	CHECK(has_error_reason("a: nano", "Unexpected word. A string value must be quoted, as in 'text'"));
	CHECK(has_error_reason("a: info", "Unexpected word. A string value must be quoted, as in 'text'"));
	CHECK(has_error_reason("a: -nan", "NaN is not representable. Use null for a missing value"));
	CHECK(has_error_reason("a: -NaN", "NaN is not representable. Use null for a missing value"));
	CHECK(has_error_reason("a: -NAN", "NaN is not representable. Use null for a missing value"));
	CHECK(has_error_reason("a: -inf", "Negative infinity is written -infinity"));
	CHECK(has_error_reason("a: -Inf", "Negative infinity is written -infinity"));
	CHECK(has_error_reason("a: -INF", "Negative infinity is written -infinity"));
	CHECK(has_error_reason("a: -Infinity", "Negative infinity is written -infinity"));
	CHECK(has_error_reason("a: -INFINITY", "Negative infinity is written -infinity"));
	CHECK(has_error_reason("a: [-inf, 1]", "Negative infinity is written -infinity"));
	CHECK(has_error_reason("a: [-Infinity, 1]", "Negative infinity is written -infinity"));

	// A “/” only ends a value for a suggestion when it starts a comment, so a suggestion never leaves out the text after it.
	CHECK(has_error_reason("speed: 100 km/h", "Expected a line break before the next entry"));
	CHECK(has_error_reason("a: 5 m/s", "Expected a line break before the next entry"));
	CHECK(has_error_reason("a: [5 m/s]", "Expected “,”, a line break, or “]” after an array item"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00/2026-09-20T15:00:00", "An instant needs an offset: Z or ±HH:MM"));
	CHECK(has_error_reason("a: [2026-09-19T14:00:00/P1D]", "An instant needs an offset: Z or ±HH:MM"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 Z/x", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 5 m /* c */", "A unit cannot follow a number after a space. Write a duration without the space, as in 10s, and anything else, such as a size, as a string, as in '512 MiB'"));
	CHECK(has_error_reason("a: 5 m/* c */", "A unit cannot follow a number after a space. Write a duration without the space, as in 10s, and anything else, such as a size, as a string, as in '512 MiB'"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00/* c */", "An instant needs an offset: Z or ±HH:MM. A date and time without an offset is a local time, which is not an instant, so write it as a string, or add the offset it was meant in"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 Z/* c */", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 Z /* c */", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));
	CHECK(has_error_reason("timeout: 5 s // seconds", "A unit cannot follow a number after a space. Write a duration without the space, as in 10s, and anything else, such as a size, as a string, as in '512 MiB'"));
	CHECK(has_error_reason("a: 5 m//c", "A unit cannot follow a number after a space. Write a duration without the space, as in 10s, and anything else, such as a size, as a string, as in '512 MiB'"));
	CHECK(has_error_reason("a: 2026-09-19T14:00:00 Z//c", "An instant's offset follows its time directly, without a space, as in 2026-09-19T14:00:00Z"));

	// An underscore next to a letter is part of a word, not a misplaced separator in a number.
	CHECK(has_error_reason("a: 4k_video", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 2x_speed", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: [5k_run, 2]", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1_", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1__0", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1_.5", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1._5", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1_e5", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1e_5", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1e5_", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1.5_", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: -1_", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1_000_", "An underscore in a number must be between two digits"));
	CHECK(has_error_reason("a: 1e-_5", "An underscore in a number must be between two digits"));

	// An uppercase “E” is only an exponent marker where one goes, so in a word such as `5EUR` it is not.
	CHECK(has_error_reason("price: 5EUR", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("disk: 10EB", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1.5Ex", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: [3ED, 1]", "Invalid number. A string value must be quoted"));
	CHECK(has_error_reason("a: 1E5", "An exponent marker is a lowercase “e”"));
	CHECK(has_error_reason("a: 1E10", "An exponent marker is a lowercase “e”"));
	CHECK(has_error_reason("a: 1.5E-3", "An exponent marker is a lowercase “e”"));
	CHECK(has_error_reason("a: -2E7", "An exponent marker is a lowercase “e”"));
	CHECK(has_error_reason("a: 1_0E1_0", "An exponent marker is a lowercase “e”"));
	CHECK(has_error_reason("a: 1E", "An exponent marker is a lowercase “e”"));
	CHECK(has_error_reason("a: 2E-", "An exponent marker is a lowercase “e”"));
	CHECK(has_error_reason("a: 1.5E", "An exponent marker is a lowercase “e”"));
}

static void test_characters_at_every_offset(void) {
	// The check for characters that are errors anywhere reads eight bytes at a time, so each kind is placed at every offset of a word, inside a string of 40 characters.
	static const unsigned char controls[] = {0x00, 0x01, 0x08, 0x0B, 0x0C, 0x0D, 0x1F, 0x7F};

	for (size_t position = 0; position < 40; position++) {
		char text[64];
		// The closing quote is at `length - 2`, before the line feed.
		size_t length = (size_t)snprintf(text, sizeof(text), "a: '%s'\n", "0123456789012345678901234567890123456789");
		size_t offset = 4 + position;
		soml_error error;

		for (size_t index = 0; index < sizeof(controls); index++) {
			text[offset] = (char)controls[index];
			CHECK(soml_parse(text, length, nullptr, &error) == nullptr && error.offset == offset && strstr(error.message, controls[index] == 0x0D ? "carriage return" : "raw control character") != nullptr);
		}

		// A tab is allowed in a string, and a control character after it is still found.
		text[offset] = '\t';
		soml_document *document = soml_parse(text, length, nullptr, nullptr);
		CHECK(document != nullptr);
		soml_document_free(document);

		if (offset + 1 < length - 2) {
			text[offset + 1] = 0x01;
			CHECK(soml_parse(text, length, nullptr, &error) == nullptr && error.offset == offset + 1);
			text[offset + 1] = '1';
		}

		// The first control character is reported, also when another follows in the same word.
		text[offset] = 0x02;
		text[length - 2] = 0x03;
		CHECK(soml_parse(text, length, nullptr, &error) == nullptr && error.offset == offset);
		text[length - 2] = '\'';

		// Invalid UTF-8 is reported before a control character, even one earlier in the text.
		text[offset] = 0x01;
		text[length - 2] = (char)0xFF;
		CHECK(soml_parse(text, length, nullptr, &error) == nullptr && error.offset == length - 2 && strcmp(error.message, "Invalid UTF-8") == 0);
		text[length - 2] = '\'';

		// A character above ASCII ends the eight-byte steps, and a control character after it is still found.
		if (offset + 2 < length - 2) {
			memcpy(text + offset, "\xC3\xA9", 2);
			document = soml_parse(text, length, nullptr, nullptr);
			CHECK(document != nullptr);
			soml_document_free(document);
			text[offset + 2] = 0x7F;
			CHECK(soml_parse(text, length, nullptr, &error) == nullptr && error.offset == offset + 2);
		}
	}
}

static void test_rules(void) {
	CHECK(is_rejected("a: 9223372036854775808", SOML_ERROR_SYNTAX));
	CHECK(has_canonical_form("a: -9223372036854775808", "a: -9223372036854775808\n"));
	CHECK(has_canonical_form("a: 0x7FFF_FFFF_FFFF_FFFF", "a: 9223372036854775807\n"));
	CHECK(is_rejected("a: 0x8000000000000000", SOML_ERROR_SYNTAX));
	CHECK(has_canonical_form("a: 0o644\nb: 0b1010\nc: 0x00FF", "a: 420\nb: 10\nc: 255\n"));
	CHECK(is_rejected("a: 1e999", SOML_ERROR_SYNTAX));

	// A float too large to be finite is pointed to the infinity of its own sign.
	CHECK(has_error_at("a: 1e999", 1, 4, "Use infinity if you mean it"));
	CHECK(has_error_at("a: -1e999", 1, 4, "Use -infinity if you mean it"));
	CHECK(has_error_at("a: [-1.7976931348623159e308]", 1, 5, "Use -infinity if you mean it"));
	CHECK(has_error_at("a: 1.7976931348623159e308", 1, 4, "Use infinity if you mean it"));
	CHECK(is_rejected("a: 1e-400", SOML_ERROR_SYNTAX));
	CHECK(has_canonical_form("a: 5e-324\nb: -0.0\nc: 0.0e5", "a: 5e-324\nb: 0.0\nc: 0.0\n"));
	CHECK(has_canonical_form("a: [infinity, -infinity, 1e21, 1e20, 1e-7, 0.000001, 123.456]", "a: [\n\tinfinity\n\t-infinity\n\t1e21\n\t100000000000000000000.0\n\t1e-7\n\t0.000001\n\t123.456\n]\n"));

	// A nonzero literal far below the smallest subnormal, and far above the largest float, with a huge exponent.
	CHECK(is_rejected("a: 1e-100000000000000000000", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 1e100000000000000000000", SOML_ERROR_SYNTAX));
	CHECK(has_canonical_form("a: 0.0e100000000000000000000", "a: 0.0\n"));

	CHECK(has_canonical_form("a: 1h30m\nb: 1.5h\nc: 1500ms\nd: 250us\ne: -5m\nf: 0s", "a: 1h30m\nb: 1h30m\nc: 1.5s\nd: 0.00025s\ne: -5m\nf: 0s\n"));
	CHECK(has_canonical_form("a: -9223372036854775808ns\nb: 9223372036854775807ns", "a: -2562047h47m16.854775808s\nb: 2562047h47m16.854775807s\n"));
	CHECK(is_rejected("a: 9223372036854775808ns", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 0.5ns", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 1.0000000001s", SOML_ERROR_SYNTAX));
	CHECK(has_canonical_form("a: 0.0000000001h\nb: 1.000000000000000000000s", "a: 0.00000036s\nb: 1s\n"));
	CHECK(is_rejected("a: -0s", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 1d", SOML_ERROR_SYNTAX));

	CHECK(has_canonical_form("a: 2026-09-19T14:00:00.123456789-07:30", "a: 2026-09-19T21:30:00.123456789Z\n"));
	CHECK(has_canonical_form("a: 0001-01-01T00:00:00Z\nb: 9999-12-31T23:59:59.999999999Z", "a: 0001-01-01T00:00:00Z\nb: 9999-12-31T23:59:59.999999999Z\n"));
	CHECK(is_rejected("a: 0001-01-01T00:00:00+00:01", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 9999-12-31T23:59:59-00:01", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 2023-02-29T00:00:00Z", SOML_ERROR_SYNTAX));
	CHECK(has_canonical_form("a: 2024-02-29T00:00:00Z", "a: 2024-02-29T00:00:00Z\n"));

	CHECK(has_canonical_form("a: '''\n\t\tone\n\n\t\t  two\n\t\t'''", "a: \"one\\n\\n  two\"\n"));

	// The invalid side of each check on a duration's fraction: not whole after the division, more than 13 significant digits, and too many digits for the unit.
	CHECK(is_rejected("a: 0.000000000002h", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 0.00000000002m", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 0.00000000000025h", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 0.000000000005m", SOML_ERROR_SYNTAX));

	// A negative offset with a zero hour is not -00:00, and instants before 1970 keep their fraction, also across the epoch.
	CHECK(has_canonical_form("a: 2026-01-01T00:00:00-00:30", "a: 2026-01-01T00:30:00Z\n"));
	CHECK(has_canonical_form("a: 1969-12-31T23:59:59.5Z", "a: 1969-12-31T23:59:59.5Z\n"));
	CHECK(has_canonical_form("a: 1970-01-01T00:59:59.25+01:00", "a: 1969-12-31T23:59:59.25Z\n"));

	// Rule 11 layouts with more than one digit.
	CHECK(has_canonical_form("a: 0.0000012345", "a: 0.0000012345\n"));
	CHECK(has_canonical_form("a: 0.00000012345", "a: 1.2345e-7\n"));
	CHECK(has_canonical_form("a: 12340000000000000000000.0", "a: 1.234e22\n"));

	// A lone slash ends a value but is not a comment, and U+FEFF after the start is content in a key.
	CHECK(is_rejected("a: 1 /", SOML_ERROR_SYNTAX));
	CHECK(has_canonical_form("'\xEF\xBB\xBF': 1", "'\xEF\xBB\xBF': 1\n"));
	CHECK(has_canonical_form("a: \"\"\"\n\tx\\ty\n\t\"\"\"", "a: \"x\\ty\"\n"));
}

static void test_separators(void) {
	// Inside brackets, a comma, a line break, or both separate items.
	CHECK(has_canonical_form("[\n80\n443\n]", "[\n\t80\n\t443\n]\n"));
	CHECK(has_canonical_form("{\na: 1\nb: 2\n}", "a: 1\nb: 2\n"));
	CHECK(has_canonical_form("[1, 2\n3]", "[\n\t1\n\t2\n\t3\n]\n"));
	CHECK(has_canonical_form("[1,\n2]", "[\n\t1\n\t2\n]\n"));
	CHECK(has_canonical_form("[1, # c\n2]", "[\n\t1\n\t2\n]\n"));
	CHECK(has_canonical_form("[1 /* a\n */, 2]", "[\n\t1\n\t2\n]\n"));
	CHECK(has_canonical_form("a: {b: 1\n\n\tc: [2\n\t\t3]}", "a: {\n\tb: 1\n\tc: [\n\t\t2\n\t\t3\n\t]\n}\n"));
	CHECK(has_canonical_form("[1 # x\n2]", "[\n\t1\n\t2\n]\n"));
	CHECK(has_canonical_form("{a:\n1}", "a: 1\n"));

	// A trailing comma is allowed, on the line of the last item.
	CHECK(has_canonical_form("[1,]", "[\n\t1\n]\n"));
	CHECK(has_canonical_form("[1,\n]", "[\n\t1\n]\n"));
	CHECK(has_canonical_form("{a: 1,\n}", "a: 1\n"));

	// A comma must be on the line of the item before it, so the comma-first style is an error, reported at the comma.
	soml_error comma_error;
	CHECK(parse_text("[1\n, 2]", &comma_error) == nullptr && comma_error.offset == 3 && strstr(comma_error.message, "A comma must be on the same line as the item before it") != nullptr);
	CHECK(has_error_message_part("[1\n,\n2]", "A comma must be on the same line"));
	CHECK(has_error_message_part("[1\n,]", "A comma must be on the same line"));
	CHECK(has_error_message_part("[\n\t1\n\t,\n]", "A comma must be on the same line"));
	CHECK(has_error_message_part("{a: 1\n, b: 2}", "A comma must be on the same line"));
	CHECK(has_error_message_part("[1 # c\n, 2]", "A comma must be on the same line"));

	// Two items on one line need a comma, and a line break inside a block comment does not separate.
	CHECK(has_error_message_part("[1 2]", "Expected “,”, a line break, or “]”"));
	CHECK(has_error_message_part("{a: 1 b: 2}", "Expected “,”, a line break, or “}”"));
	CHECK(has_error_message_part("[1 /*\n*/ 2]", "after an array item. A line break inside a block comment does not separate items"));
	CHECK(has_error_message_part("{a: 1 /*\n*/ b: 2}", "after an object member. A line break inside a block comment does not separate items"));
	CHECK(!has_error_message_part("[1 /* x */ 2]", "A line break inside a block comment"));

	// A comma needs an item before it.
	CHECK(is_rejected("[,1]", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("[1,,2]", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("[1,\n,2]", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("[,]", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("{,}", SOML_ERROR_SYNTAX));

	// A missing value is still missing when a line break follows the `:`, and an unterminated container is reported where it opens.
	CHECK(is_rejected("{a:\nb: 2}", SOML_ERROR_SYNTAX));
	soml_error error;
	CHECK(parse_text("a: [1\n2\n", &error) == nullptr && error.offset == 3 && strstr(error.message, "Unterminated array") != nullptr);

	// The top level still takes line breaks only, so a comma there is explained wherever it is: on the line of the entry, at the start of the next line, or alone on a line after the last entry.
	CHECK(has_error_message_part("a: 1,\nb: 2", "not commas"));
	CHECK(has_error_at("a: 1, b: 2", 1, 5, "not commas"));
	CHECK(has_error_at("a: 1\n, b: 2", 2, 1, "not commas"));
	CHECK(has_error_at("a: 1\n,b: 2", 2, 1, "not commas"));
	CHECK(has_error_at("a: 1\n,\nb: 2", 2, 1, "not commas"));
	CHECK(has_error_at("a: 1\n\n, b: 2", 3, 1, "not commas"));
	CHECK(has_error_at("a: 1 # c\n, b: 2", 2, 1, "not commas"));
	CHECK(has_error_at("a: 1 /* x\n*/, b: 2", 2, 3, "not commas"));
	CHECK(has_error_at("a: [1]\n,", 2, 1, "not commas"));
	CHECK(has_error_at("a: 1\n,", 2, 1, "not commas"));
	CHECK(has_error_at("a: 1\n\t,\n", 2, 2, "not commas"));

	// Anything else that is not a key after a line break is still a missing key.
	CHECK(has_error_at("a: 1\n]", 2, 1, "Expected a key"));
	CHECK(has_error_at("a: 1\n:", 2, 1, "Expected a key"));
}

static void test_depth(void) {
	char text[1024];
	size_t length = 0;

	// 100 nested arrays are the limit, and 101 are too deep.
	for (int index = 0; index < 100; index++) {
		text[length++] = '[';
	}

	for (int index = 0; index < 100; index++) {
		text[length++] = ']';
	}

	soml_document *document = soml_parse(text, length, nullptr, nullptr);
	CHECK(document != nullptr);
	soml_string output;
	CHECK(soml_write(document, &output, nullptr));
	soml_document_free(document);

	memmove(text + 1, text, length);
	text[0] = '[';
	text[length + 1] = ']';
	soml_error error;
	CHECK(soml_parse(text, length + 2, nullptr, &error) == nullptr && error.code == SOML_ERROR_TOO_DEEP);

	// A brace-less top-level object is the first level.
	length = (size_t)snprintf(text, sizeof(text), "a: ");

	for (int index = 0; index < 99; index++) {
		text[length++] = '[';
	}

	for (int index = 0; index < 99; index++) {
		text[length++] = ']';
	}

	document = soml_parse(text, length, nullptr, nullptr);
	CHECK(document != nullptr);
	soml_document_free(document);
	memmove(text + 4, text + 3, length - 3);
	text[3] = '[';
	text[length + 1] = ']';
	CHECK(soml_parse(text, length + 2, nullptr, &error) == nullptr && error.code == SOML_ERROR_TOO_DEEP);

	// Hostile depth is an error, not a stack overflow.
	size_t hostile_length = 1'000'000;
	char *hostile = malloc(hostile_length);
	memset(hostile, '[', hostile_length);
	CHECK(soml_parse(hostile, hostile_length, nullptr, &error) == nullptr && error.code == SOML_ERROR_TOO_DEEP);
	free(hostile);
}

#ifdef HAS_SMALL_STACK_TEST
/*
Parses and writes documents at the nesting limit, in every form, and counts the successes.
*/
static void *parse_deep_documents(void *context) {
	static const char *const openings[] = {"[", "{a: "};
	int *successes = context;
	char text[1024];

	for (size_t form = 0; form < sizeof(openings) / sizeof(openings[0]); form++) {
		size_t length = 0;
		size_t opening_length = strlen(openings[form]);

		// 100 levels: 100 arrays, or 99 braced objects around one that holds a slow float, which exercises the larger decimal-conversion frame while the parser's frames are on the stack.
		for (int level = 0; level < (form == 0 ? 100 : 99); level++) {
			memcpy(text + length, openings[form], opening_length);
			length += opening_length;
		}

		if (form == 1) {
			memcpy(text + length, "{b: 5e-324}", 11);
			length += 11;
		}

		for (int level = 0; level < (form == 0 ? 100 : 99); level++) {
			text[length++] = form == 0 ? ']' : '}';
		}

		soml_document *document = soml_parse(text, length, nullptr, nullptr);
		soml_string output;

		if (document != nullptr && soml_write(document, &output, nullptr)) {
			(*successes)++;
		}

		soml_document_free(document);
	}

	return nullptr;
}
#endif

static void test_small_stack(void) {
	// The parser and the writer recurse once per level, with small frames, so the deepest documents fit in a 64 KB stack, which threads and embedded systems often have. A native build may set up a large frame only on the path that needs it, which hides a large frame here; `make wasm32`, whose stack is a fixed 64 KB, is what found one.
	#ifdef HAS_SMALL_STACK_TEST
	pthread_attr_t attributes;
	pthread_t thread;
	int successes = 0;
	pthread_attr_init(&attributes);
	CHECK(pthread_attr_setstacksize(&attributes, 64 * 1024) == 0);
	CHECK(pthread_create(&thread, &attributes, parse_deep_documents, &successes) == 0);
	pthread_join(thread, nullptr);
	pthread_attr_destroy(&attributes);
	CHECK(successes == 2);
	#endif
}

// MARK: Writing

static char *write_to_string(soml_document *document, soml_error *error) {
	soml_string output;

	if (!soml_write(document, &output, error)) {
		return nullptr;
	}

	char *copy = malloc(output.length + 1);
	memcpy(copy, output.data, output.length + 1);
	return copy;
}

static void test_string_scanning(void) {
	// Every byte at every position around the word boundaries must still be validated, escaped, and quoted correctly.
	static const size_t lengths[] = {1, 7, 8, 9, 15, 16, 17, 31, 32, 33};

	for (size_t size = 0; size < sizeof(lengths) / sizeof(lengths[0]); size++) {
		size_t length = lengths[size];
		char *bytes = malloc(length);

		for (size_t offset = 0; offset < length; offset++) {
			for (unsigned byte = 0; byte <= 255; byte++) {
				memset(bytes, 'a', length);
				bytes[offset] = (char)byte;
				soml_document *document = soml_document_create(nullptr);
				soml_value *root = soml_new_array(document);
				CHECK(soml_array_append(document, root, soml_new_string(document, bytes, length)));
				CHECK(soml_document_set_root(document, root));
				soml_string output;
				soml_error error;
				bool is_valid = byte < 0x80 && byte != '\r';
				bool written = soml_write(document, &output, &error);
				CHECK(written == is_valid);

				if (written) {
					bool needs_escapes = byte == '\'' || byte < 0x20 || byte == 0x7F;
					CHECK(output.data[3] == (needs_escapes ? '"' : '\''));
					soml_document *parsed = soml_parse(output.data, output.length, nullptr, nullptr);
					soml_string string;
					CHECK(soml_get_string(soml_array_at(soml_document_root(parsed), 0), &string) && string.length == length && memcmp(string.data, bytes, length) == 0);
					soml_document_free(parsed);
				} else {
					CHECK(error.code == SOML_ERROR_INVALID_VALUE);
				}

				soml_document_free(document);
			}
		}

		free(bytes);
	}

	// UTF-8 sequences can cross a word boundary or end at the last byte. Literal strings must still end on their starting line.
	for (size_t offset = 0; offset < 24; offset++) {
		char text[64];
		memcpy(text, "['", 2);
		memset(text + 2, 'a', offset);
		memcpy(text + 2 + offset, "é€😀']", 12);
		soml_document *document = parse_text(text, nullptr);
		soml_string output;
		CHECK(document != nullptr && soml_write(document, &output, nullptr));
		soml_document_free(document);
		memmove(text + 3 + offset, text + 2 + offset, 12);
		text[2 + offset] = '\n';
		CHECK(is_rejected(text, SOML_ERROR_SYNTAX));
		text[2 + offset] = '\0';
		CHECK(is_rejected(text, SOML_ERROR_SYNTAX));
	}
}

static void test_utf8_sequences(void) {
	// The endpoints of every multi-byte range in Unicode Table 3-7, including shortest forms, surrogate boundaries, and U+10FFFF.
	static const struct {
		size_t length;
		unsigned char bytes[4];
	} sequences[] = {
		{2, {0xC2, 0x80}}, {2, {0xDF, 0xBF}},
		{3, {0xE0, 0xA0, 0x80}}, {3, {0xE0, 0xBF, 0xBF}},
		{3, {0xE1, 0x80, 0x80}}, {3, {0xEC, 0xBF, 0xBF}},
		{3, {0xED, 0x80, 0x80}}, {3, {0xED, 0x9F, 0xBF}},
		{3, {0xEE, 0x80, 0x80}}, {3, {0xEF, 0xBF, 0xBF}},
		{4, {0xF0, 0x90, 0x80, 0x80}}, {4, {0xF0, 0xBF, 0xBF, 0xBF}},
		{4, {0xF1, 0x80, 0x80, 0x80}}, {4, {0xF3, 0xBF, 0xBF, 0xBF}},
		{4, {0xF4, 0x80, 0x80, 0x80}}, {4, {0xF4, 0x8F, 0xBF, 0xBF}},
	};
	static const size_t padding[] = {0, 5, 6, 7, 13, 14, 15};
	static const uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};

	for (size_t sequence = 0; sequence < sizeof(sequences) / sizeof(sequences[0]); sequence++) {
		size_t length = sequences[sequence].length;

		for (size_t position = 0; position < sizeof(padding) / sizeof(padding[0]); position++) {
			size_t offset = 2 + padding[position];
			char *text = malloc(offset + length + 2);
			memcpy(text, "['", 2);
			memset(text + 2, 'a', padding[position]);
			memcpy(text + offset, sequences[sequence].bytes, length);
			memcpy(text + offset + length, "']", 2);

			// Each incomplete sequence ends at its allocation boundary. The leading byte, rather than a missing quote, must be reported.
			for (size_t cut = 1; cut < length; cut++) {
				char *prefix = malloc(offset + cut);
				memcpy(prefix, text, offset + cut);
				soml_error error;
				soml_document *document = soml_parse(prefix, offset + cut, nullptr, &error);
				free(prefix);
				CHECK(document == nullptr && error.code == SOML_ERROR_SYNTAX && error.offset == offset && strcmp(error.message, "Incomplete UTF-8 sequence at the end of the input") == 0);
				soml_document_free(document);

				// A builder accepts incomplete UTF-8, but the writer must refuse it in a key too.
				document = soml_document_create(nullptr);
				soml_value *root = soml_new_object(document);
				CHECK(soml_object_add(document, root, text + 2, padding[position] + cut, soml_new_null(document)));
				CHECK(soml_document_set_root(document, root));
				soml_string output = {.data = "unchanged", .length = 9};
				CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE && string_equals(output, "unchanged"));
				CHECK(!soml_write_canonical(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE && string_equals(output, "unchanged"));
				soml_document_free(document);
			}

			for (size_t changed = 0; changed < length; changed++) {
				for (unsigned byte = 0; byte <= 255; byte++) {
					unsigned char *bytes = (unsigned char *)text + offset;
					memcpy(bytes, sequences[sequence].bytes, length);
					bytes[changed] = (unsigned char)byte;

					// Decode the scalar independently of the validator's second-byte ranges, then check its shortest form and scalar range.
					bool is_valid = ((unsigned)bytes[0] >> (7 - length)) == ((1U << (length + 1)) - 2);
					uint32_t scalar = bytes[0] & (0x7FU >> length);

					for (size_t index = 1; index < length; index++) {
						is_valid = is_valid && (bytes[index] & 0xC0) == 0x80;
						scalar = scalar << 6 | (bytes[index] & 0x3FU);
					}

					is_valid = is_valid && scalar >= minimum[length] && scalar <= 0x10FFFF && (scalar < 0xD800 || scalar > 0xDFFF);
					soml_error error;
					soml_document *document = soml_parse(text, offset + length + 2, nullptr, &error);
					CHECK((document != nullptr) == is_valid);

					if (document != nullptr) {
						soml_string string;
						CHECK(soml_get_string(soml_array_at(soml_document_root(document), 0), &string) && string.length == padding[position] + length && memcmp(string.data, text + 2, string.length) == 0);
					} else {
						CHECK(error.code == SOML_ERROR_SYNTAX && strcmp(error.message, "Invalid UTF-8") == 0);
					}

					soml_document_free(document);
					// The same independent scalar check applies to strings and keys.
					for (unsigned form = 0; form < 2; form++) {
						document = soml_document_create(nullptr);
						soml_value *root = form == 0 ? soml_new_array(document) : soml_new_object(document);

						if (form == 0) {
							CHECK(soml_array_append(document, root, soml_new_string(document, text + 2, padding[position] + length)));
						} else {
							CHECK(soml_object_add(document, root, text + 2, padding[position] + length, soml_new_null(document)));
						}

						CHECK(soml_document_set_root(document, root));
						soml_string output = {.data = "unchanged", .length = 9};
						CHECK(soml_write(document, &output, &error) == is_valid && error.code == (is_valid ? SOML_ERROR_NONE : SOML_ERROR_INVALID_VALUE));
						CHECK(is_valid || string_equals(output, "unchanged"));
						CHECK(soml_write_canonical(document, &output, &error) == is_valid && error.code == (is_valid ? SOML_ERROR_NONE : SOML_ERROR_INVALID_VALUE));
						CHECK(is_valid || string_equals(output, "unchanged"));
						soml_document_free(document);
					}
				}
			}

			free(text);
		}
	}
}

static void test_quote_bytes(void) {
	// Every byte inside either quote form, including a closing quote or escape at each position around a word boundary.
	static const char quotes[] = {'\'', '"'};

	for (size_t form = 0; form < sizeof(quotes); form++) {
		for (size_t offset = 0; offset < 17; offset++) {
			size_t length = offset + 6;
			char *text = malloc(length);
			text[0] = '[';
			text[1] = quotes[form];
			memset(text + 2, 'a', offset + 2);
			text[length - 2] = quotes[form];
			text[length - 1] = ']';

			for (unsigned byte = 0; byte <= 255; byte++) {
				text[2 + offset] = (char)byte;
				bool is_valid = (byte == '\t' || (byte >= 0x20 && byte < 0x7F)) && byte != (unsigned char)quotes[form] && !(quotes[form] == '"' && byte == '\\');
				soml_document *document = soml_parse(text, length, nullptr, nullptr);
				CHECK((document != nullptr) == is_valid);

				if (document != nullptr) {
					soml_string string;
					CHECK(soml_get_string(soml_array_at(soml_document_root(document), 0), &string) && string.length == offset + 2 && memcmp(string.data, text + 2, string.length) == 0);
				}

				soml_document_free(document);
			}

			free(text);
		}
	}
}

static void test_builder_copies_text(void) {
	// Binary strings and keys, including empty text and arena-size boundaries, must outlive their caller's mutable, unterminated buffer.
	static const size_t lengths[] = {0, 1, 7, 8, 9, 31, 32, 33, 4095, 4096, 4097};

	for (size_t size = 0; size < sizeof(lengths) / sizeof(lengths[0]); size++) {
		size_t length = lengths[size];
		char *text = malloc(length == 0 ? 1 : length);

		for (size_t index = 0; index < length; index++) {
			text[index] = (char)(index % 256);
		}

		soml_document *document = soml_document_create(nullptr);
		soml_value *string = soml_new_string(document, text, length);

		for (size_t index = 0; index < length; index++) {
			CHECK((unsigned char)text[index] == index % 256);
		}

		soml_value *object = soml_new_object(document);
		CHECK(string != nullptr && soml_object_add(document, object, text, length, string));

		for (size_t index = 0; index < length; index++) {
			CHECK((unsigned char)text[index] == index % 256);
		}

		memset(text, 0xFF, length);
		free(text);
		soml_string value = {};
		soml_string key = soml_object_key_at(object, 0);
		CHECK(soml_get_string(string, &value) && value.data != nullptr && value.length == length && value.data[length] == '\0');
		CHECK(key.data != nullptr && key.length == length && key.data[length] == '\0');

		for (size_t index = 0; index < length; index++) {
			CHECK((unsigned char)value.data[index] == index % 256 && (unsigned char)key.data[index] == index % 256);
		}

		CHECK(soml_object_get_with_length(object, value.data, value.length) == string);
		soml_document_free(document);
	}
}

static void test_builder(void) {
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_object(document);
	soml_value *pool = soml_new_object(document);
	soml_value *labels = soml_new_array(document);
	CHECK(soml_object_add(document, root, "name", 4, soml_new_string(document, "api's", 5)));
	CHECK(soml_object_add(document, root, "replicas", 8, soml_new_int(document, 3)));
	CHECK(soml_object_add(document, root, "ratio", 5, soml_new_float(document, -0.0)));
	CHECK(soml_object_add(document, root, "pool", 4, pool));
	CHECK(soml_object_add(document, pool, "max", 3, soml_new_int(document, 16)));
	CHECK(soml_object_add(document, pool, "min", 3, soml_new_int(document, 2)));
	CHECK(soml_object_add(document, root, "labels", 6, labels));
	CHECK(soml_array_append(document, labels, soml_new_string(document, "a\tb", 3)));
	CHECK(soml_array_append(document, labels, soml_new_null(document)));
	CHECK(soml_array_append(document, labels, soml_new_bool(document, false)));
	CHECK(soml_array_append(document, labels, soml_new_object(document)));
	CHECK(soml_object_add(document, root, "the key", 7, soml_new_duration(document, 5'400'000'000'000)));
	CHECK(soml_object_add(document, root, "at", 2, soml_new_instant(document, (soml_instant){.seconds = 0, .nanoseconds = 1})));
	CHECK(soml_document_set_root(document, root));

	// Lookup on an object whose keys were not added in order.
	int64_t replicas;
	CHECK(soml_get_int(soml_object_get(root, "replicas"), &replicas) && replicas == 3);
	CHECK(soml_object_get(root, "nope") == nullptr);

	// Members are written in the order they were added.
	char *text = write_to_string(document, nullptr);
	const char *expected = "name: \"api's\"\nreplicas: 3\nratio: 0.0\npool: {\n\tmax: 16\n\tmin: 2\n}\nlabels: [\n\t\"a\\tb\"\n\tnull\n\tfalse\n\t{}\n]\n'the key': 1h30m\nat: 1970-01-01T00:00:00.000000001Z\n";
	CHECK(text != nullptr && strcmp(text, expected) == 0);

	// The output reads back as the same document, in the same order.
	soml_document *parsed = parse_text(text, nullptr);
	soml_string again;
	CHECK(parsed != nullptr && soml_write(parsed, &again, nullptr) && string_equals(again, expected));
	soml_document_free(parsed);

	// Canonical form sorts the members by key, and reads back to the same canonical form.
	const char *expected_canonical = "at: 1970-01-01T00:00:00.000000001Z\nlabels: [\n\t\"a\\tb\"\n\tnull\n\tfalse\n\t{}\n]\nname: \"api's\"\npool: {\n\tmax: 16\n\tmin: 2\n}\nratio: 0.0\nreplicas: 3\n'the key': 1h30m\n";
	CHECK(soml_write_canonical(document, &again, nullptr) && string_equals(again, expected_canonical));
	char *canonical_again = canonical(text);
	CHECK(canonical_again != nullptr && strcmp(canonical_again, expected_canonical) == 0);
	free(canonical_again);
	free(text);
	soml_document_free(document);

	// An empty top-level object keeps its braces, and an empty top-level array is written too.
	document = soml_document_create(nullptr);
	CHECK(soml_document_set_root(document, soml_new_object(document)));
	text = write_to_string(document, nullptr);
	CHECK(text != nullptr && strcmp(text, "{}\n") == 0);
	free(text);
	CHECK(soml_document_set_root(document, soml_new_array(document)));
	text = write_to_string(document, nullptr);
	CHECK(text != nullptr && strcmp(text, "[]\n") == 0);
	free(text);
	soml_document_free(document);
}

static soml_error_code write_error(soml_value *(*make)(soml_document *document)) {
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_object(document);
	soml_object_add(document, root, "a", 1, make(document));
	soml_document_set_root(document, root);
	soml_string output;
	soml_error error;
	bool is_written = soml_write(document, &output, &error);
	soml_document_free(document);
	return is_written ? SOML_ERROR_NONE : error.code;
}

static soml_value *make_nan(soml_document *document) {
	return soml_new_float(document, strtod("nan", nullptr));
}

static soml_value *make_carriage_return(soml_document *document) {
	return soml_new_string(document, "a\rb", 3);
}

static soml_value *make_invalid_utf8(soml_document *document) {
	return soml_new_string(document, "a\xFF", 2);
}

static soml_value *make_surrogate(soml_document *document) {
	return soml_new_string(document, "\xED\xA0\x80", 3);
}

static soml_value *make_early_instant(soml_document *document) {
	return soml_new_instant(document, (soml_instant){.seconds = -62'135'596'801, .nanoseconds = 0});
}

static soml_value *make_late_instant(soml_document *document) {
	return soml_new_instant(document, (soml_instant){.seconds = 253'402'300'800, .nanoseconds = 0});
}

static soml_value *make_negative_nanoseconds(soml_document *document) {
	return soml_new_instant(document, (soml_instant){.seconds = 0, .nanoseconds = -1});
}

static soml_value *make_negative_nan(soml_document *document) {
	uint64_t bits = 0xFFF8000000000000;
	double value;
	memcpy(&value, &bits, sizeof(value));
	return soml_new_float(document, value);
}

static soml_value *make_bad_nanoseconds(soml_document *document) {
	return soml_new_instant(document, (soml_instant){.seconds = 0, .nanoseconds = 1'000'000'000});
}

static soml_value *make_nothing(soml_document *document) {
	(void)document;
	return nullptr;
}

static soml_value *make_duplicate_keys(soml_document *document) {
	soml_value *object = soml_new_object(document);
	soml_object_add(document, object, "b", 1, soml_new_int(document, 1));
	soml_object_add(document, object, "a", 1, soml_new_int(document, 2));
	soml_object_add(document, object, "b", 1, soml_new_int(document, 3));
	return object;
}

static soml_value *make_duplicate_sorted_keys(soml_document *document) {
	soml_value *object = soml_new_object(document);
	soml_object_add(document, object, "a", 1, soml_new_int(document, 1));
	soml_object_add(document, object, "a", 1, soml_new_int(document, 2));
	return object;
}

static void test_builder_canonical_forms(void) {
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_object(document);
	soml_object_add(document, root, "before", 6, soml_new_instant(document, (soml_instant){.seconds = -1, .nanoseconds = 500'000'000}));
	soml_object_add(document, root, "up", 2, soml_new_float(document, soml__infinity()));
	soml_object_add(document, root, "down", 4, soml_new_float(document, -soml__infinity()));

	// Keys that look like other values are bare, because a key is always a string. An empty key and a key with a quote are quoted.
	static const char *const keys[] = {"404", "true", "-1", "1e5", "", "a'b"};

	for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
		soml_object_add(document, root, keys[index], strlen(keys[index]), soml_new_int(document, (int64_t)index));
	}

	soml_document_set_root(document, root);
	soml_string output;
	CHECK(soml_write_canonical(document, &output, nullptr) && string_equals(output, "'': 4\n-1: 2\n1e5: 3\n404: 0\n\"a'b\": 5\nbefore: 1969-12-31T23:59:59.5Z\ndown: -infinity\ntrue: 1\nup: infinity\n"));
	soml_document_free(document);

	// A chain of 101 objects is too deep, as a chain of arrays is.
	document = soml_document_create(nullptr);
	soml_value *chain = soml_new_object(document);

	for (int index = 1; index < 101; index++) {
		soml_value *outer = soml_new_object(document);
		soml_object_add(document, outer, "a", 1, chain);
		chain = outer;
	}

	soml_document_set_root(document, chain);
	soml_error error;
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_TOO_DEEP);
	soml_document_free(document);
}

static void test_nan_payloads(void) {
	// Signaling NaNs and the largest payload, with either sign, must not be mistaken for finite values or infinity.
	static const uint64_t patterns[] = {0x7FF0000000000001, 0xFFF0000000000001, 0x7FFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF};

	for (size_t index = 0; index < sizeof(patterns) / sizeof(patterns[0]); index++) {
		double value;
		memcpy(&value, &patterns[index], sizeof(value));
		soml_document *document = soml_document_create(nullptr);
		soml_value *root = soml_new_array(document);
		soml_value *made = soml_new_float(document, value);
		double actual = 0;
		CHECK(soml_get_float(made, &actual));
		uint64_t actual_bits;
		memcpy(&actual_bits, &actual, sizeof(actual_bits));
		CHECK((actual_bits & 0x7FFFFFFFFFFFFFFF) > 0x7FF0000000000000);
		CHECK(soml_array_append(document, root, made));
		CHECK(soml_document_set_root(document, root));
		soml_string output;
		soml_error error;
		CHECK(!soml_write_canonical(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
		soml_document_free(document);
	}
}

static void test_built_key_order(void) {
	// An unsorted built object uses the writer's temporary sort. NUL is part of a key, Unicode is ordered by scalar value rather than UTF-16, and normalization is not performed.
	static const soml_string keys[] = {
		{"", 0},
		{"\0", 1},
		{"a", 1},
		{"a\0", 2},
		{"a\0z", 3},
		{"e\xCC\x81", 3},
		{"\xC3\xA9", 2},
		{"\xEE\x80\x80", 3},
		{"\xF0\x90\x80\x80", 4},
	};
	constexpr size_t count = sizeof(keys) / sizeof(keys[0]);
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_object(document);

	for (size_t index = count; index > 0; index--) {
		soml_string key = keys[index - 1];
		CHECK(soml_object_add(document, root, key.data, key.length, soml_new_int(document, (int64_t)(index - 1))));
	}

	CHECK(soml_document_set_root(document, root));
	soml_string output;
	const char *expected = "'': 0\n\"\\u{0}\": 1\na: 2\n\"a\\u{0}\": 3\n\"a\\u{0}z\": 4\n'e\xCC\x81': 5\n'\xC3\xA9': 6\n'\xEE\x80\x80': 7\n'\xF0\x90\x80\x80': 8\n";
	CHECK(soml_write_canonical(document, &output, nullptr) && string_equals(output, expected));
	soml_document *parsed = soml_parse(output.data, output.length, nullptr, nullptr);
	CHECK(parsed != nullptr);

	for (size_t index = 0; index < count; index++) {
		soml_string key = keys[index];
		int64_t value;
		CHECK(soml_get_int(soml_object_get_with_length(root, key.data, key.length), &value) && value == (int64_t)index);
		CHECK(soml_get_int(soml_object_get_with_length(soml_document_root(parsed), key.data, key.length), &value) && value == (int64_t)index);
		soml_string parsed_key = soml_object_key_at(soml_document_root(parsed), index);
		CHECK(parsed_key.length == key.length && memcmp(parsed_key.data, key.data, key.length) == 0);
	}

	soml_document_free(parsed);
	soml_document_free(document);
}

static void test_write_errors(void) {
	// NaN must be refused even with -ffast-math.
	CHECK(write_error(make_nan) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_negative_nan) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_carriage_return) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_invalid_utf8) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_surrogate) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_early_instant) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_late_instant) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_bad_nanoseconds) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_negative_nanoseconds) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_nothing) == SOML_ERROR_INVALID_ARGUMENT);
	CHECK(write_error(make_duplicate_keys) == SOML_ERROR_INVALID_VALUE);
	CHECK(write_error(make_duplicate_sorted_keys) == SOML_ERROR_INVALID_VALUE);

	// A root that is not a collection, and no root.
	soml_document *document = soml_document_create(nullptr);
	soml_string output;
	soml_error error;
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	CHECK(soml_document_set_root(document, soml_new_int(document, 1)));
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	soml_document_free(document);

	// A key with a line feed is written with the string rules, and a key with a carriage return is refused.
	document = soml_document_create(nullptr);
	soml_value *root = soml_new_object(document);
	soml_object_add(document, root, "a\nb", 3, soml_new_int(document, 1));
	soml_document_set_root(document, root);
	CHECK(soml_write(document, &output, nullptr) && string_equals(output, "\"a\\nb\": 1\n"));
	soml_object_add(document, root, "\r", 1, soml_new_int(document, 1));
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	soml_document_free(document);

	// Builder functions accept nullptr.
	CHECK(soml_new_int(nullptr, 1) == nullptr);
	CHECK(!soml_array_append(nullptr, nullptr, nullptr));
	CHECK(!soml_object_add(nullptr, nullptr, "a", 1, nullptr));
	CHECK(!soml_write(nullptr, &output, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
}

static void test_changing_parsed_documents(void) {
	soml_document *document = parse_text("b: 1\nd: [1]\n", nullptr);
	soml_value *root = soml_document_root(document);

	// A key that sorts after every other keeps lookup a binary search, and one that does not makes it a scan. Both are found.
	CHECK(soml_object_add(document, root, "e", 1, soml_new_int(document, 5)));
	CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 0)));
	CHECK(soml_array_append(document, soml_object_get(root, "d"), soml_new_int(document, 2)));
	int64_t value;
	CHECK(soml_get_int(soml_object_get(root, "a"), &value) && value == 0);
	CHECK(soml_get_int(soml_object_get(root, "e"), &value) && value == 5);
	CHECK(soml_get_int(soml_object_get(root, "b"), &value) && value == 1);
	CHECK(string_equals(soml_object_key_at(root, 3), "a"));

	// Added members are written after the parsed ones, in the order they were added, and canonical form sorts them.
	soml_string output;
	CHECK(soml_write_canonical(document, &output, nullptr) && string_equals(output, "a: 0\nb: 1\nd: [\n\t1\n\t2\n]\ne: 5\n"));
	CHECK(soml_write(document, &output, nullptr) && string_equals(output, "b: 1\nd: [\n\t1\n\t2\n]\ne: 5\na: 0\n"));

	// Writing twice gives the same text.
	soml_string again;
	CHECK(soml_write(document, &again, nullptr) && again.length == output.length && memcmp(again.data, output.data, output.length) == 0);

	// A duplicate of a parsed key is reported when writing.
	CHECK(soml_object_add(document, root, "b", 1, soml_new_int(document, 2)));
	soml_error error;
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	soml_document_free(document);
}

static void test_growing_empty_parsed_collections(void) {
	// Empty objects before and after the parser has allocated entries, including a nonzero entry base.
	static const char *const inputs[] = {"[{}, {}]", "[{}, {before: 0, empty: {}}]"};

	for (size_t index = 0; index < sizeof(inputs) / sizeof(inputs[0]); index++) {
		soml_document *document = parse_text(inputs[index], nullptr);
		CHECK(document != nullptr);
		soml_value *root = soml_document_root(document);
		soml_value *first = soml_array_at(root, 0);
		soml_value *second = soml_array_at(root, 1);

		if (index == 1) {
			second = soml_object_get(second, "empty");
		}

		CHECK(soml_type_of(first) == SOML_TYPE_OBJECT && soml_count(first) == 0);
		CHECK(soml_type_of(second) == SOML_TYPE_OBJECT && soml_count(second) == 0);
		CHECK(soml_object_add(document, first, "z", 1, soml_new_int(document, 1)));
		CHECK(soml_object_add(document, second, "b", 1, soml_new_int(document, 2)));
		CHECK(soml_object_add(document, second, "a", 1, soml_new_int(document, 3)));
		int64_t value;
		CHECK(soml_get_int(soml_object_get(second, "a"), &value) && value == 3);
		CHECK(soml_count(first) == 1 && soml_count(second) == 2);
		soml_string output;
		const char *expected = index == 0 ? "[\n\t{\n\t\tz: 1\n\t}\n\t{\n\t\ta: 3\n\t\tb: 2\n\t}\n]\n" : "[\n\t{\n\t\tz: 1\n\t}\n\t{\n\t\tbefore: 0\n\t\tempty: {\n\t\t\ta: 3\n\t\t\tb: 2\n\t\t}\n\t}\n]\n";
		CHECK(soml_write_canonical(document, &output, nullptr) && string_equals(output, expected));
		soml_document_free(document);
	}

	soml_document *document = parse_text("[]", nullptr);
	CHECK(soml_array_append(document, soml_document_root(document), soml_new_int(document, 1)));
	soml_string output;
	CHECK(soml_write(document, &output, nullptr) && string_equals(output, "[\n\t1\n]\n"));
	soml_document_free(document);
}

static void test_values_from_another_document(void) {
	soml_document *first = soml_document_create(nullptr);
	soml_document *second = soml_document_create(nullptr);
	soml_value *root = soml_new_array(first);
	soml_value *foreign = soml_new_int(second, 1);
	CHECK(!soml_array_append(first, root, foreign));
	CHECK(!soml_document_set_root(second, root));

	soml_value *object = soml_new_object(first);
	CHECK(!soml_object_add(first, object, "a", 1, foreign));
	CHECK(!soml_object_add(second, object, "a", 1, foreign));

	// The first failure is what writing reports.
	soml_document_set_root(first, root);
	soml_string output;
	soml_error error;
	CHECK(!soml_write(first, &output, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && strstr(error.message, "another document") != nullptr);
	soml_document_free(second);
	soml_document_free(first);
}

static soml_value *make_nested_arrays(soml_document *document, int count) {
	soml_value *value = soml_new_array(document);

	for (int index = 1; index < count; index++) {
		soml_value *outer = soml_new_array(document);
		soml_array_append(document, outer, value);
		value = outer;
	}

	return value;
}

static void test_write_depth(void) {
	// 99 arrays inside the top-level object make 100 levels, the limit.
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_object(document);
	soml_object_add(document, root, "a", 1, make_nested_arrays(document, 99));
	soml_document_set_root(document, root);
	soml_string output;
	soml_error error;
	CHECK(soml_write(document, &output, &error));

	soml_object_add(document, root, "b", 1, make_nested_arrays(document, 100));
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_TOO_DEEP);
	soml_document_free(document);

	// An array root is the first level too, so 100 arrays write, and 101 do not.
	document = soml_document_create(nullptr);
	CHECK(soml_document_set_root(document, make_nested_arrays(document, 100)));
	CHECK(soml_write(document, &output, &error) && soml_write_canonical(document, &output, &error));
	CHECK(soml_document_set_root(document, make_nested_arrays(document, 101)));
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_TOO_DEEP);
	CHECK(!soml_write_canonical(document, &output, &error) && error.code == SOML_ERROR_TOO_DEEP);

	// So is an empty object root, written with braces.
	CHECK(soml_document_set_root(document, soml_new_object(document)));
	CHECK(soml_write(document, &output, &error) && strcmp(output.data, "{}\n") == 0);
	soml_document_free(document);
}

static void test_one_place_per_value(void) {
	// A value in several places would be written once for each, which grows exponentially with nesting, so a value can be in one place only.
	soml_document *document = soml_document_create(nullptr);
	soml_value *shared = soml_new_int(document, 1);
	soml_value *first = soml_new_array(document);
	soml_value *second = soml_new_object(document);
	CHECK(soml_array_append(document, first, shared));
	CHECK(!soml_array_append(document, first, shared));
	CHECK(!soml_object_add(document, second, "a", 1, shared));
	CHECK(!soml_document_set_root(document, shared));
	soml_string output;
	soml_error error;
	CHECK(soml_document_set_root(document, first));
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && strstr(error.message, "one place") != nullptr);
	soml_document_free(document);

	// A cycle cannot be attached under the root, because every value in it is already in a container.
	document = soml_document_create(nullptr);
	soml_value *outer = soml_new_array(document);
	soml_value *inner = soml_new_array(document);
	CHECK(soml_array_append(document, outer, inner));
	CHECK(soml_array_append(document, inner, outer));
	CHECK(!soml_document_set_root(document, outer));
	soml_value *root = soml_new_array(document);
	CHECK(soml_document_set_root(document, root));
	CHECK(!soml_array_append(document, root, outer));
	CHECK(!soml_array_append(document, root, root));
	soml_document_free(document);

	// Setting a new root frees the old one to be added somewhere.
	document = soml_document_create(nullptr);
	soml_value *old_root = soml_new_array(document);
	soml_value *new_root = soml_new_array(document);
	CHECK(soml_document_set_root(document, old_root));
	CHECK(soml_document_set_root(document, old_root));
	CHECK(soml_document_set_root(document, new_root));
	CHECK(soml_array_append(document, new_root, old_root));
	CHECK(soml_write(document, &output, nullptr) && string_equals(output, "[\n\t[]\n]\n"));
	soml_document_free(document);

	// The values of a parsed document are attached too.
	document = parse_text("a: [1]\n", nullptr);
	soml_value *parsed_root = soml_document_root(document);
	CHECK(!soml_object_add(document, parsed_root, "b", 1, soml_object_get(parsed_root, "a")));
	CHECK(!soml_array_append(document, soml_object_get(parsed_root, "a"), parsed_root));
	soml_document_free(document);
}

typedef struct collector {
	char *text;
	size_t length;
	size_t calls;
	size_t fail_after;
} collector;

static bool collect(void *context, const char *bytes, size_t length) {
	collector *target = context;

	if (target->calls++ >= target->fail_after) {
		return false;
	}

	target->text = realloc(target->text, target->length + length + 1);
	memcpy(target->text + target->length, bytes, length);
	target->length += length;
	target->text[target->length] = '\0';
	return true;
}

static void test_byte_masks(void) {
	// Every target byte against rotating, mixed-byte words, including high bits and adjacent values. The expected mask is assembled byte by byte, independently of the word arithmetic and host byte order.
	for (unsigned pattern = 0; pattern < 256; pattern++) {
		unsigned char bytes[8];

		for (size_t index = 0; index < sizeof(bytes); index++) {
			bytes[index] = (unsigned char)((pattern + index * 17) % 256);
		}

		uint64_t word;
		memcpy(&word, bytes, sizeof(word));

		for (unsigned target = 0; target < 256; target++) {
			unsigned char expected_bytes[8];

			for (size_t index = 0; index < sizeof(bytes); index++) {
				expected_bytes[index] = bytes[index] == target ? 0x80 : 0;
			}

			uint64_t expected;
			memcpy(&expected, expected_bytes, sizeof(expected));
			CHECK(soml__equal_byte_mask(word, (unsigned char)target) == expected);
		}
	}
}

static void test_string_scanner_boundaries(void) {
	static constexpr size_t lengths[] = {0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33};
	static constexpr unsigned char quotes[] = {'\'', '"'};

	// Every byte at every position around word boundaries and at every alignment. Allocations end at the scanner's limit, so a word load past it is visible to ASan.
	for (size_t alignment = 0; alignment < alignof(max_align_t); alignment++) {
		for (size_t sample = 0; sample < sizeof(lengths) / sizeof(lengths[0]); sample++) {
			size_t length = lengths[sample];
			unsigned char *allocation = malloc(alignment + length + (length == 0 ? 1 : 0));
			unsigned char *text = allocation + alignment;
			memset(text, 'a', length);

			for (size_t kind = 0; kind < sizeof(quotes); kind++) {
				unsigned char quote = quotes[kind];
				CHECK(soml__string_chunk_end(text, length, 0, quote) == length);
				CHECK(soml__string_chunk_end(text, length, length, quote) == length);

				for (size_t position = 0; position < length; position++) {
					for (unsigned byte = 0; byte < 256; byte++) {
						text[position] = (unsigned char)byte;
						size_t expected = 0;

						while (expected < length && text[expected] != quote && text[expected] != '\n' && (quote != '"' || text[expected] != '\\')) {
							expected++;
						}

						CHECK(soml__string_chunk_end(text, length, 0, quote) == expected);
					}

					text[position] = 'a';
				}
			}

			free(allocation);
		}
	}
}

static void test_unescaped_string_allocations(void) {
	static constexpr size_t lengths[] = {0, 1, 7, 8, 9, 4095, 4096, 4097, 8192};

	// Double quotes without escapes need no more allocations than single quotes, even across scratch and arena chunk boundaries. Both documents own their strings after the exact-size input is freed.
	for (size_t sample = 0; sample < sizeof(lengths) / sizeof(lengths[0]); sample++) {
		size_t length = lengths[sample];
		char *input = malloc(length + 4);
		input[0] = '[';
		input[1] = '\'';
		memset(input + 2, 'a', length);
		input[length + 2] = '\'';
		input[length + 3] = ']';
		fuzz_allocator literal_state = {.fail_at = SIZE_MAX};
		soml_allocator literal_allocator = {.reallocate = fuzz_reallocate, .context = &literal_state};
		soml_document *literal = soml_parse(input, length + 4, &literal_allocator, nullptr);
		input[1] = '"';
		input[length + 2] = '"';
		fuzz_allocator escaped_state = {.fail_at = SIZE_MAX};
		soml_allocator escaped_allocator = {.reallocate = fuzz_reallocate, .context = &escaped_state};
		soml_document *escaped = soml_parse(input, length + 4, &escaped_allocator, nullptr);
		free(input);
		CHECK(literal != nullptr && escaped != nullptr);
		CHECK(literal_state.allocations == escaped_state.allocations);
		soml_string left = {0};
		soml_string right = {0};
		CHECK(soml_get_string(soml_array_at(soml_document_root(literal), 0), &left));
		CHECK(soml_get_string(soml_array_at(soml_document_root(escaped), 0), &right));
		CHECK(left.length == length && right.length == length && memcmp(left.data, right.data, length) == 0);
		CHECK(left.data[length] == '\0' && right.data[length] == '\0');
		soml_document_free(literal);
		soml_document_free(escaped);
		CHECK(literal_state.live_blocks == 0 && escaped_state.live_blocks == 0);
	}
}

static void test_ordered_writer_allocation_budget(void) {
	// These keys cover empty, embedded NUL, non-ASCII and prefix comparisons. In-order building and writing must need no sort allocation, including after appending to a parsed object.
	static const soml_string keys[] = {{"", 0}, {"a", 1}, {"a\0", 2}, {"aa", 2}, {"b", 1}, {"é", 2}};
	const char *expected = "'': 0\na: 1\n\"a\\u{0}\": 2\naa: 3\nb: 4\n'é': 5\n";

	for (unsigned parsed = 0; parsed < 2; parsed++) {
		fuzz_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
		soml_document *document = parsed ? soml_parse("{'': 0, a: 1}", 13, &allocator, nullptr) : soml_document_create(&allocator);
		soml_value *root = parsed ? soml_document_root(document) : soml_new_object(document);

		for (size_t index = parsed ? 2 : 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
			CHECK(soml_object_add(document, root, keys[index].data, keys[index].length, soml_new_int(document, (int64_t)index)));
		}

		CHECK(parsed || soml_document_set_root(document, root));
		state.fail_at = state.allocations;
		size_t allocations = state.allocations;

		for (unsigned canonical = 0; canonical < 2; canonical++) {
			collector output = {.fail_after = SIZE_MAX};
			soml_error error;
			CHECK(canonical ? soml_write_canonical_with_callback(document, collect, &output, &error) : soml_write_with_callback(document, collect, &output, &error));
			CHECK(error.code == SOML_ERROR_NONE && state.allocations == allocations);
			CHECK(output.text != nullptr && strcmp(output.text, expected) == 0);
			free(output.text);
		}

		soml_document_free(document);
		CHECK(state.live_blocks == 0);
	}
}

static void test_indented_block_string_budget(void) {
	// The allocator cannot supply a large source-sized string block. Discarded indentation must not consume the document budget, and blank boundary/interior lines must retain their specified behavior.
	constexpr size_t indentation = 1024;
	constexpr size_t lines = 20;
	size_t capacity = (indentation + 2) * (lines + 3) + 16;
	char *text = malloc(capacity);
	size_t length = 0;
	memcpy(text + length, "['''\n", 5);
	length += 5;

	for (size_t line = 0; line < lines + 2; line++) {
		memset(text + length, ' ', indentation);
		length += indentation;

		if (line > 0 && line <= lines && line != 10) {
			text[length++] = 'x';
		}

		text[length++] = '\n';
	}

	memset(text + length, ' ', indentation);
	length += indentation;
	memcpy(text + length, "''']", 4);
	length += 4;
	fuzz_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
	// A 4 KB chunk already exists; the decoded string fits, whereas the source span would require another allocation.
	soml_document *document = soml_document_create(&allocator);
	CHECK(soml_new_null(document) != nullptr);
	state.fail_at = state.allocations;
	soml__parser parser = {.text = (const unsigned char *)text, .length = length, .position = 1, .document = document};
	soml_string string = {.data = nullptr};
	bool is_parsed = soml__parse_block_string(&parser, '\'', &string);
	CHECK(is_parsed);
	free(text);

	if (!is_parsed) {
		soml_document_free(document);
		return;
	}
	CHECK(string.length == lines * 2 - 2);
	size_t index = 0;

	for (size_t line = 1; line <= lines; line++) {
		if (line > 1) {
			CHECK(string.data[index++] == '\n');
		}

		if (line != 10) {
			CHECK(string.data[index++] == 'x');
		}
	}

	CHECK(string.data[index] == '\0');
	soml_document_free(document);
	CHECK(state.live_blocks == 0);
}

static void test_parsed_ordering_mutations(void) {
	// Appending to either source order must preserve existing keys, including a nonidentity parsed index. The first duplicate in the source still wins over a later parse failure.
	static const char *const inputs[] = {"a: 1\nb: 2\n", "b: 2\na: 1\n"};

	for (size_t sample = 0; sample < sizeof(inputs) / sizeof(inputs[0]); sample++) {
		soml_document *document = parse_text(inputs[sample], nullptr);
		soml_value *root = soml_document_root(document);
		CHECK(soml_object_add(document, root, "c", 1, soml_new_int(document, 3)));

		for (int64_t index = 1; index <= 3; index++) {
			char key = (char)('a' + index - 1);
			int64_t actual = -1;
			CHECK(soml_get_int(soml_object_get_with_length(root, &key, 1), &actual) && actual == index);
		}

		soml_string output;
		CHECK(soml_write_canonical(document, &output, nullptr) && string_equals(output, "a: 1\nb: 2\nc: 3\n"));
		CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 99)));
		int64_t first = -1;
		CHECK(soml_get_int(soml_object_get(root, "a"), &first) && first == 1);
		soml_error error;
		CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
		soml_document_free(document);
	}

	static const char *const duplicates[] = {"{a: 1, a: 2, b: [}", "{z: 0, a: 1, a: 2, b: [}", "{a: 1, a: 2, b: {c: 1, c: 2, d: [}}"};

	for (size_t sample = 0; sample < sizeof(duplicates) / sizeof(duplicates[0]); sample++) {
		soml_error error;
		soml_document *document = parse_text(duplicates[sample], &error);
		const char *duplicate = strstr(duplicates[sample], "a: 2");
		CHECK(document == nullptr && error.code == SOML_ERROR_SYNTAX && error.offset == (size_t)(duplicate - duplicates[sample]));
		CHECK(strcmp(error.message, "Duplicate key") == 0);
		soml_document_free(document);
	}
}

static void test_write_callback(void) {
	// Long enough to be written in several pieces.
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_array(document);

	for (int index = 0; index < 5000; index++) {
		char text[32];
		int length = snprintf(text, sizeof(text), "item %d", index);
		soml_array_append(document, root, soml_new_string(document, text, (size_t)length));
	}

	// Longer than the writer's buffer, so it goes to the callback in one piece.
	static char long_text[10'000];
	memset(long_text, 'a', sizeof(long_text));
	soml_array_append(document, root, soml_new_string(document, long_text, sizeof(long_text)));
	soml_array_append(document, root, soml_new_string(document, "x", 1));
	soml_document_set_root(document, root);
	soml_string expected;
	CHECK(soml_write(document, &expected, nullptr));

	// The text reads back with the long string whole.
	soml_document *reread = parse_text(expected.data, nullptr);
	soml_string long_string = {};
	CHECK(soml_get_string(soml_array_at(soml_document_root(reread), 5000), &long_string) && long_string.length == sizeof(long_text) && memcmp(long_string.data, long_text, sizeof(long_text)) == 0);
	soml_document_free(reread);

	collector target = {.fail_after = SIZE_MAX};
	CHECK(soml_write_with_callback(document, collect, &target, nullptr));
	CHECK(target.calls > 1 && target.length == expected.length && memcmp(target.text, expected.data, expected.length) == 0);
	free(target.text);

	collector failing = {.fail_after = 1};
	soml_error error;
	CHECK(!soml_write_with_callback(document, collect, &failing, &error) && error.code == SOML_ERROR_WRITE);
	CHECK(failing.calls == 2 && failing.length > 0 && failing.length < expected.length);
	free(failing.text);

	// A callback failure does not prevent retrying the write, and success clears the old error.
	collector retry = {.fail_after = SIZE_MAX};
	CHECK(soml_write_with_callback(document, collect, &retry, &error) && error.code == SOML_ERROR_NONE);
	CHECK(retry.length == expected.length && memcmp(retry.text, expected.data, expected.length) == 0);
	free(retry.text);
	soml_document_free(document);
}

static bool writes(soml_document *document, bool is_canonical, const char *expected) {
	soml_string output;
	collector target = {.fail_after = SIZE_MAX};
	bool is_written;

	if (is_canonical) {
		is_written = soml_write_canonical(document, &output, nullptr) && soml_write_canonical_with_callback(document, collect, &target, nullptr);
	} else {
		is_written = soml_write(document, &output, nullptr) && soml_write_with_callback(document, collect, &target, nullptr);
	}

	bool is_equal = is_written && string_equals(output, expected) && target.text != nullptr && strcmp(target.text, expected) == 0;
	free(target.text);
	return is_equal;
}

#ifdef HAS_THREADS
typedef struct concurrent_reader {
	const soml_document *document;
	soml_string output[2];
} concurrent_reader;

/*
Reads shared values and streams both writer orders, using only thread-local callback state. Returns the context on failure, so the test counters are only updated after joining the threads.
*/
static void *read_document_concurrently(void *context) {
	const concurrent_reader *reader = context;
	soml_value *root = soml_document_root(reader->document);

	for (unsigned iteration = 0; iteration < 100; iteration++) {
		int64_t integer = 0;
		soml_string string = {};
		soml_value *nested = soml_object_get(root, "a");

		if (soml_count(root) != 2 || !string_equals(soml_object_key_at(root, 0), "z") || !soml_get_int(soml_object_get(root, "z"), &integer) || integer != 1 || !soml_get_int(soml_object_get(nested, "a"), &integer) || integer != 2 || !soml_get_string(soml_object_get(nested, "z"), &string) || !string_equals(string, "text")) {
			return context;
		}

		soml_object_view members = soml_object_members(root);
		size_t measured;

		if (members.count != 2 || members.data[0].value != soml_object_get(root, "z") || !soml_validate(reader->document, nullptr) || !soml_measure(reader->document, &measured, nullptr) || measured != reader->output[0].length) {
			return context;
		}

		for (unsigned order = 0; order < 2; order++) {
			collector target = {.fail_after = SIZE_MAX};
			bool is_written = order == 0 ? soml_write_with_callback(reader->document, collect, &target, nullptr) : soml_write_canonical_with_callback(reader->document, collect, &target, nullptr);
			bool is_equal = is_written && target.length == reader->output[order].length && memcmp(target.text, reader->output[order].data, target.length) == 0;
			free(target.text);

			char buffer[128];
			size_t length;
			bool is_buffered = order == 0 ? soml_write_to_buffer(reader->document, buffer, sizeof(buffer), &length, nullptr) : soml_write_canonical_to_buffer(reader->document, buffer, sizeof(buffer), &length, nullptr);
			is_equal = is_equal && is_buffered && length == reader->output[order].length && memcmp(buffer, reader->output[order].data, length + 1) == 0;

			if (!is_equal) {
				return context;
			}
		}
	}

	return nullptr;
}
#endif

static void test_concurrent_reads(void) {
	// Accessors and callback writers may share one document. Built objects sort keys in temporary allocations, so both nested sorts must remain local to each writer.
	#ifdef HAS_THREADS
	for (unsigned mode = 0; mode < 2; mode++) {
		soml_document *document = mode == 0 ? parse_text("z: 1\na: {z: 'text', a: 2}\n", nullptr) : soml_document_create(nullptr);

		if (mode == 1) {
			soml_value *root = soml_new_object(document);
			soml_value *nested = soml_new_object(document);
			CHECK(soml_object_add(document, root, "z", 1, soml_new_int(document, 1)));
			CHECK(soml_object_add(document, nested, "z", 1, soml_new_string(document, "text", 4)));
			CHECK(soml_object_add(document, nested, "a", 1, soml_new_int(document, 2)));
			CHECK(soml_object_add(document, root, "a", 1, nested));
			CHECK(soml_document_set_root(document, root));
		}

		concurrent_reader reader = {.document = document};
		CHECK(soml_write(document, &reader.output[0], nullptr));
		CHECK(soml_write_canonical(document, &reader.output[1], nullptr));
		pthread_t threads[4];
		size_t started = 0;

		for (; started < sizeof(threads) / sizeof(threads[0]); started++) {
			if (!CHECK(pthread_create(&threads[started], nullptr, read_document_concurrently, &reader) == 0)) {
				break;
			}
		}

		for (size_t index = 0; index < started; index++) {
			void *result = &reader;
			CHECK(pthread_join(threads[index], &result) == 0 && result == nullptr);
		}

		soml_document_free(document);
	}
	#endif
}

static void test_write_member_order(void) {
	// Writing keeps the order of the document, at the top level and nested, and canonical form sorts every object by key. The callback writers write the same text.
	soml_document *document = parse_text("name: 'api'\ndescription: 'The edge'\npool: {min: 2, max: 16}\nhosts: [{port: 80, host: 'a'}]\n", nullptr);
	CHECK(writes(document, false, "name: 'api'\ndescription: 'The edge'\npool: {\n\tmin: 2\n\tmax: 16\n}\nhosts: [\n\t{\n\t\tport: 80\n\t\thost: 'a'\n\t}\n]\n"));
	CHECK(writes(document, true, "description: 'The edge'\nhosts: [\n\t{\n\t\thost: 'a'\n\t\tport: 80\n\t}\n]\nname: 'api'\npool: {\n\tmax: 16\n\tmin: 2\n}\n"));
	soml_document_free(document);

	// A duplicate key is refused in both orders.
	document = soml_document_create(nullptr);
	soml_value *root = soml_new_object(document);
	CHECK(soml_object_add(document, root, "b", 1, soml_new_int(document, 1)));
	CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 2)));
	CHECK(soml_object_add(document, root, "b", 1, soml_new_int(document, 3)));
	CHECK(soml_document_set_root(document, root));
	soml_string output;
	soml_error error;
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	CHECK(!soml_write_canonical(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	collector target = {.fail_after = SIZE_MAX};
	CHECK(!soml_write_with_callback(document, collect, &target, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	CHECK(!soml_write_canonical_with_callback(document, collect, &target, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	free(target.text);
	CHECK(!soml_write_canonical(nullptr, &output, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
	soml_document_free(document);
}

// MARK: Floats

static double parse_float(const char *literal, bool *is_valid) {
	size_t length = strlen(literal);
	char *text = malloc(length + 4);
	memcpy(text, "a: ", 3);
	memcpy(text + 3, literal, length + 1);
	soml_document *document = soml_parse(text, length + 3, nullptr, nullptr);
	free(text);
	double value = 0;
	*is_valid = soml_get_float(soml_object_get(soml_document_root(document), "a"), &value);
	soml_document_free(document);
	return value;
}

/*
Rewrites a decimal literal in any common form, such as `+1.5E+07` or `0012.5`, as `0.<digits>e<exponent>` in SOML syntax.
*/
static char *soml_literal(const char *text) {
	size_t length = strlen(text);
	char *digits = malloc(length + 1);
	size_t digit_count = 0;
	int64_t point = 0;
	bool is_negative = false;
	bool has_seen_point = false;
	bool has_significant_digit = false;
	size_t index = 0;

	if (text[index] == '-' || text[index] == '+') {
		is_negative = text[index] == '-';
		index++;
	}

	for (; index < length && text[index] != 'e' && text[index] != 'E'; index++) {
		if (text[index] == '.') {
			has_seen_point = true;
			continue;
		}

		if (!has_significant_digit && text[index] == '0') {
			if (has_seen_point) {
				point--;
			}

			continue;
		}

		has_significant_digit = true;

		if (!has_seen_point) {
			point++;
		}

		digits[digit_count++] = text[index];
	}

	digits[digit_count] = '\0';
	int64_t exponent = index < length ? strtoll(text + index + 1, nullptr, 10) : 0;
	char *result = malloc(digit_count + 64);

	if (digit_count == 0) {
		snprintf(result, digit_count + 64, "0.0");
	} else {
		snprintf(result, digit_count + 64, "%s0.%se%" PRId64, is_negative ? "-" : "", digits, point + exponent);

		// The exponent zero is written e0, and is never negative.
		char *exponent_text = strrchr(result, 'e');

		if (strcmp(exponent_text, "e-0") == 0) {
			memcpy(exponent_text, "e0", 3);
		}
	}

	free(digits);
	return result;
}

static void check_parse_corpus(const char *cases_path, const char *expected_path) {
	size_t cases_length;
	size_t expected_length;
	char *cases = read_file(cases_path, &cases_length);
	char *expected = read_file(expected_path, &expected_length);
	char *case_line = cases;
	char *expected_line = expected;
	size_t count = 0;
	size_t failures = 0;

	while (*case_line != '\0' && *expected_line != '\0') {
		char *case_end = strchr(case_line, '\n');
		char *expected_end = strchr(expected_line, '\n');
		*case_end = '\0';
		*expected_end = '\0';

		// The expected bits are little-endian bytes in hexadecimal.
		uint64_t bits = 0;

		for (int byte = 7; byte >= 0; byte--) {
			unsigned value;
			sscanf(expected_line + byte * 2, "%2x", &value);
			bits = (bits << 8) | value;
		}

		char *literal = soml_literal(case_line);
		bool is_valid;
		double value = parse_float(literal, &is_valid);
		uint64_t actual;
		memcpy(&actual, &value, sizeof(actual));
		uint64_t magnitude = bits & ~((uint64_t)1 << 63);
		bool is_correct;

		if (magnitude == 0x7FF0000000000000) {
			// Too large to be finite is an error.
			is_correct = !is_valid;
		} else if (magnitude == 0) {
			// A nonzero literal that rounds to zero is an error.
			is_correct = !is_valid || strcmp(literal, "0.0") == 0;
		} else {
			is_correct = is_valid && actual == bits;
		}

		if (!is_correct && failures++ < 5) {
			fprintf(stderr, "Float %.60s… read as %016" PRIx64 ", expected %016" PRIx64 "\n", literal, actual, bits);
		}

		free(literal);
		count++;
		case_line = case_end + 1;
		expected_line = expected_end + 1;
	}

	CHECK(count > 7000);
	CHECK(*case_line == '\0' && *expected_line == '\0');
	CHECK(failures == 0);
	free(cases);
	free(expected);
}

/*
The shortest digits of a canonical float, and the exponent that makes them its value: `1.5e300` is 15 and 299.
*/
static void shortest_digits(const char *text, char *digits, int64_t *exponent) {
	size_t count = 0;
	int64_t fraction_digits = 0;
	bool is_after_point = false;
	const char *character = text[0] == '-' ? text + 1 : text;

	for (; *character != '\0' && *character != 'e'; character++) {
		if (*character == '.') {
			is_after_point = true;
			continue;
		}

		if (count == 0 && *character == '0') {
			if (is_after_point) {
				fraction_digits++;
			}

			continue;
		}

		digits[count++] = *character;
		fraction_digits += is_after_point;
	}

	int64_t power = *character == 'e' ? strtoll(character + 1, nullptr, 10) : 0;

	while (count > 0 && digits[count - 1] == '0') {
		count--;
		fraction_digits--;
	}

	digits[count] = '\0';
	*exponent = power - fraction_digits;
}

static void test_decimal_shifts(void) {
	// Compare the paired-digit arithmetic against exact 64-bit integers, including carries and odd digit counts, at every supported shift.
	for (unsigned shift = 1; shift <= soml__maximum_shift; shift++) {
		for (uint64_t value = 1; value <= 127; value++) {
			soml__decimal decimal;
			soml__decimal expected;
			soml__decimal_assign(&decimal, value);
			soml__decimal_assign(&expected, value << shift);
			soml__decimal_left_shift(&decimal, shift);
			CHECK(decimal.count == expected.count && decimal.point == expected.point && !decimal.truncated && memcmp(decimal.digits, expected.digits, (size_t)expected.count) == 0);
			soml__decimal_right_shift(&decimal, shift);
			soml__decimal_assign(&expected, value);
			CHECK(decimal.count == expected.count && decimal.point == expected.point && !decimal.truncated && memcmp(decimal.digits, expected.digits, (size_t)expected.count) == 0);
		}
	}

	// Carry and fractional digits at the capacity boundary must preserve the sticky bit used for exact rounding.
	for (int count = soml__decimal_capacity - 2; count <= soml__decimal_capacity; count++) {
		for (int direction = -1; direction <= 1; direction += 2) {
			soml__decimal decimal = {.count = count, .point = count};
			memset(decimal.digits, 9, (size_t)count);
			soml__decimal_shift(&decimal, direction);
			bool truncated = count == soml__decimal_capacity;
			CHECK(decimal.count == count + (truncated ? 0 : 1));
			CHECK(decimal.point == count + (direction > 0 ? 1 : 0));
			CHECK(decimal.truncated == truncated);
			CHECK(decimal.digits[0] == (direction > 0 ? 1 : 4));

			for (int index = 1; index < count; index++) {
				CHECK(decimal.digits[index] == 9);
			}

			if (!truncated) {
				CHECK(decimal.digits[count] == (direction > 0 ? 8 : 5));
			}
		}
	}
}

static void test_float_parsing(void) {
	// The long corpus includes exact underflow, overflow, and subnormal-to-normal midpoints, with a deciding tail beyond the 800-digit capacity.
	check_parse_corpus("test/fixtures/float-parse-cases.txt", "test/fixtures/float-parse-expected.txt");
	check_parse_corpus("test/fixtures/float-parse-long-cases.txt", "test/fixtures/float-parse-long-expected.txt");

	// A halfway value followed by 800 zeros rounds to even, and a tail beyond 800 digits decides a near-halfway value.
	size_t length = 2000;
	char *literal = malloc(length);
	int written = snprintf(literal, length, "9007199254740993.");
	memset(literal + written, '0', 800);
	literal[written + 800] = '\0';
	bool is_valid;
	CHECK(parse_float(literal, &is_valid) == 9007199254740992.0 && is_valid);
	literal[written + 799] = '1';
	CHECK(parse_float(literal, &is_valid) == 9007199254740994.0 && is_valid);
	free(literal);

	// Underscores, and a long run of leading zeros.
	CHECK(parse_float("1_000.000_5", &is_valid) == 1000.0005 && is_valid);
	char zeros[700] = "0.";
	memset(zeros + 2, '0', 600);
	memcpy(zeros + 602, "1e600", 6);
	CHECK(parse_float(zeros, &is_valid) == 0.1 && is_valid);
}

static void test_float_halfway_values(void) {
	// Exact midpoints around 1, from (lower + upper) / 2, including both tie parities and the smaller spacing below a power of two.
	static const struct {
		const char *midpoint;
		uint64_t lower_bits;
	} cases[] = {
		{"0.999999999999999944488848768742172978818416595458984375", 0x3FEFFFFFFFFFFFFF},
		{"1.00000000000000011102230246251565404236316680908203125", 0x3FF0000000000000},
		{"1.00000000000000033306690738754696212708950042724609375", 0x3FF0000000000001},
	};

	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		for (int direction = -1; direction <= 1; direction++) {
			for (int is_negative = 0; is_negative < 2; is_negative++) {
				char literal[1024];
				literal[0] = '-';
				char *magnitude = literal + is_negative;
				size_t length = strlen(cases[index].midpoint);
				memcpy(magnitude, cases[index].midpoint, length);

				// A change in the 900th extra digit lies beyond the conversion's digit capacity. Borrowing makes the just-below value end in 9s.
				memset(magnitude + length, direction < 0 ? '9' : '0', 900);
				magnitude[length + 900] = '\0';

				if (direction < 0) {
					magnitude[length - 1]--;
				} else if (direction > 0) {
					magnitude[length + 899] = '1';
				}

				uint64_t expected = cases[index].lower_bits + (direction == 0 ? cases[index].lower_bits % 2 : (uint64_t)(direction > 0));
				expected |= (uint64_t)is_negative << 63;
				bool is_valid;
				double value = parse_float(literal, &is_valid);
				uint64_t actual;
				memcpy(&actual, &value, sizeof(actual));
				CHECK(is_valid && actual == expected);
			}
		}
	}
}

static void test_float_formatting(void) {
	size_t bits_length;
	size_t expected_length;
	char *bits_text = read_file("test/fixtures/float-shortest-bits.txt", &bits_length);
	char *expected_text = read_file("test/fixtures/float-shortest-expected.txt", &expected_length);
	char *bits_line = bits_text;
	char *expected_line = expected_text;
	size_t count = 0;
	size_t failures = 0;

	while (*bits_line != '\0' && *expected_line != '\0') {
		uint64_t bits = strtoull(bits_line, &bits_line, 16);
		char *rest;
		char *expected_digits = expected_line;
		long long expected_digits_value = strtoll(expected_line, &rest, 10);
		(void)expected_digits_value;
		size_t digits_length = (size_t)(rest - expected_digits);
		int64_t expected_exponent = strtoll(rest, &rest, 10);
		expected_line = rest;

		while (*bits_line == '\n') {
			bits_line++;
		}

		while (*expected_line == '\n' || *expected_line == ' ') {
			expected_line++;
		}

		double value;
		memcpy(&value, &bits, sizeof(value));
		char text[32];
		size_t length = soml__format_float(value, text);
		text[length] = '\0';
		char digits[32];
		int64_t exponent;
		shortest_digits(text, digits, &exponent);

		bool is_correct = strlen(digits) == digits_length && memcmp(digits, expected_digits, digits_length) == 0 && exponent == expected_exponent;

		// The text reads back as the same value.
		bool is_valid;
		is_correct = is_correct && parse_float(text, &is_valid) == value && is_valid;

		if (!is_correct && failures++ < 5) {
			fprintf(stderr, "Float %016" PRIx64 " written as %s, expected %.*s × 10^%" PRId64 "\n", bits, text, (int)digits_length, expected_digits, expected_exponent);
		}

		count++;
	}

	CHECK(count > 30'000);
	CHECK(*bits_line == '\0' && *expected_line == '\0');
	CHECK(failures == 0);
	free(bits_text);
	free(expected_text);
}

// Marsaglia's xorshift64 with a fixed seed, so that every run tries the same values and a failure can be repeated. Its left shifts deliberately discard high bits; all other arithmetic keeps the sanitizer checks.
static uint64_t random_state = 0x9E3779B97F4A7C15;

#if defined(__clang__)
[[clang::no_sanitize("unsigned-shift-base")]]
#endif
static uint64_t next_random(void) {
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return random_state;
}

/*
Whether formatting a finite float other than negative zero preserves its bits when read back. Prints the literal on failure.
*/
static bool float_reads_back(uint64_t bits) {
	double value;
	memcpy(&value, &bits, sizeof(value));
	char text[32];
	size_t length = soml__format_float(value, text);
	text[length] = '\0';
	bool is_valid;
	double read = parse_float(text, &is_valid);
	bool is_equal = is_valid && memcmp(&read, &value, sizeof(value)) == 0;

	if (!is_equal) {
		fprintf(stderr, "Float %016" PRIx64 " written as %s did not read back\n", bits, text);
	}

	return is_equal;
}

static void test_float_round_trip(void) {
	size_t failures = 0;

	for (int index = 0; index < 200'000; index++) {
		uint64_t bits = next_random();

		// Not NaN or infinity, and not negative zero, which is zero.
		if ((bits & 0x7FF0000000000000) == 0x7FF0000000000000 || bits == (uint64_t)1 << 63) {
			continue;
		}

		failures += !float_reads_back(bits);
	}

	CHECK(failures == 0);
}

static void test_float_boundaries(void) {
	// Every power of two and its neighbors, including subnormals. Spacing changes at powers of two, which random mantissas almost never exercise.
	for (int exponent = -1074; exponent <= 1023; exponent++) {
		uint64_t power = exponent < -1022 ? (uint64_t)1 << (exponent + 1074) : (uint64_t)(exponent + 1023) << 52;

		for (uint64_t offset = 0; offset < 3; offset++) {
			uint64_t magnitude = power - 1 + offset;
			CHECK(float_reads_back(magnitude));

			// Negative zero is stored as zero, so it does not preserve its bits.
			if (magnitude != 0) {
				CHECK(float_reads_back(magnitude | (uint64_t)1 << 63));
			}
		}
	}
}

static void test_whole_float_formatting(void) {
	// Whole numbers below 2^53 skip the shortest-digits search, and must give exactly what it gives.
	size_t failures = 0;

	for (int index = 0; index < 200'000; index++) {
		uint64_t random = next_random();
		uint64_t whole = index % 4 == 0 ? random % 1000 * (uint64_t)1'000'000'000 : random >> (11 + random % 53);
		double value = (double)(whole % ((uint64_t)1 << 53));

		if (value == 0.0) {
			continue;
		}

		char text[32];
		size_t length = soml__format_float(value, text);
		uint64_t bits;
		memcpy(&bits, &value, sizeof(bits));
		soml__decimal decimal;
		soml__decimal_shortest(&decimal, bits);
		char expected[32];
		size_t expected_length = 0;

		for (int digit = 0; digit < decimal.count; digit++) {
			expected[expected_length++] = (char)('0' + decimal.digits[digit]);
		}

		for (int digit = decimal.count; digit < decimal.point; digit++) {
			expected[expected_length++] = '0';
		}

		bool is_layout_plain = decimal.point <= 21;

		if (is_layout_plain && (length != expected_length + 2 || memcmp(text, expected, expected_length) != 0 || memcmp(text + expected_length, ".0", 2) != 0) && failures++ < 5) {
			fprintf(stderr, "Whole float %.0f written as %.*s\n", value, (int)length, text);
		}
	}

	CHECK(failures == 0);
	CHECK(has_canonical_form("a: [1.0, 30.0, 9007199254740991.0, 9007199254740992.0, 1e15, 123000.0]", "a: [\n\t1.0\n\t30.0\n\t9007199254740991.0\n\t9007199254740992.0\n\t1000000000000000.0\n\t123000.0\n]\n"));
}

static void test_rounding_modes(void) {
	// Floats read and write the same whatever rounding mode the calling thread has set. A mode that the platform does not support is skipped.
	const char *text = "a: [0.1, 0.3, 123.456, 1e22, 9007199254740993, 2.5e-324, 1.7976931348623157e308, 30.0]";
	char *expected = canonical(text);
	// A platform defines only the modes it supports, and WebAssembly supports only rounding to nearest.
	static const int modes[] = {
	#ifdef FE_DOWNWARD
		FE_DOWNWARD,
	#endif
	#ifdef FE_UPWARD
		FE_UPWARD,
	#endif
	#ifdef FE_TOWARDZERO
		FE_TOWARDZERO,
	#endif
		FE_TONEAREST,
	};

	for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); index++) {
		if (fesetround(modes[index]) != 0) {
			continue;
		}

		char *result = canonical(text);
		fesetround(FE_TONEAREST);
		CHECK(expected != nullptr && result != nullptr && strcmp(result, expected) == 0);
		free(result);
	}

	free(expected);
}

static void test_flush_subnormals(void) {
#if defined(__aarch64__) || defined(__SSE2__)
	const char *text = "a: [5e-324, -5e-324, 2.225073858507201e-308, -2.225073858507201e-308, 2.2250738585072014e-308, 2.4703282292062328e-324, 0.0, -0.0]";
	char *expected = canonical(text);
	soml_document *document = parse_text(text, nullptr);

	// Preserve the thread's register while enabling flush-to-zero (and denormals-are-zero on x86).
	#if defined(__aarch64__)
		uint64_t original;
		__asm__ volatile ("mrs %0, fpcr" : "=r" (original));
		uint64_t flushing = original | (uint64_t)1 << 24;
		__asm__ volatile ("msr fpcr, %0" : : "r" (flushing));
	#else
		unsigned int original = _mm_getcsr();
		_mm_setcsr(original | (1u << 15) | (1u << 6));
	#endif

	soml_document *parsed = parse_text(text, nullptr);
	soml_string output = {0};
	bool is_written = soml_write_canonical(document, &output, nullptr);
	bool is_underflow_rejected = is_rejected("a: 2.4703282292062327e-324", SOML_ERROR_SYNTAX);

	#if defined(__aarch64__)
		__asm__ volatile ("msr fpcr, %0" : : "r" (original));
	#else
		_mm_setcsr(original);
	#endif

	CHECK(parsed != nullptr);
	CHECK(expected != nullptr && is_written && output.length == strlen(expected) && memcmp(output.data, expected, output.length) == 0);
	CHECK(is_underflow_rejected);

	// Check the parsed bits after restoring the mode, so flushing cannot hide a lost subnormal.
	soml_string parsed_output = {0};
	bool is_parsed_written = parsed != nullptr && soml_write_canonical(parsed, &parsed_output, nullptr);
	CHECK(expected != nullptr && is_parsed_written && parsed_output.length == strlen(expected) && memcmp(parsed_output.data, expected, parsed_output.length) == 0);
	soml_document_free(parsed);
	soml_document_free(document);
	free(expected);
#endif
}

static void test_float_fast_path(void) {
	// The fast path must give exactly what the high-precision conversion gives, for every case it takes.
	size_t failures = 0;
	[[maybe_unused]] size_t fast_count = 0;

	for (int index = 0; index < 300'000; index++) {
		uint64_t random = next_random();
		soml__decimal decimal = {.count = 0};
		int digit_count = 1 + (int)(random % 16);
		uint64_t digits = next_random();

		for (int digit = 0; digit < digit_count; digit++) {
			decimal.digits[decimal.count++] = (unsigned char)(digit == 0 ? 1 + digits % 9 : digits % 10);
			digits /= 10;

			if (digits == 0) {
				digits = next_random();
			}
		}

		soml__decimal_trim(&decimal);
		decimal.point = decimal.count + (int)((random >> 8) % 51) - 25;
		soml__decimal exact = decimal;
		double fast;
		double slow;

		if (!soml__parse_short_float(&decimal, &fast)) {
			continue;
		}

		fast_count++;
		bool is_finite = soml__decimal_to_double(&exact, &slow);

		if ((!is_finite || memcmp(&fast, &slow, sizeof(fast)) != 0) && failures++ < 5) {
			fprintf(stderr, "Fast path gave %.17g, the exact conversion %.17g\n", fast, slow);
		}
	}

	// The fast path is off where binary64 arithmetic is not guaranteed.
	#if FLT_EVAL_METHOD == 0 && !defined(__FAST_MATH__)
	CHECK(fast_count > 100'000);
	#endif

	CHECK(failures == 0);
}


/*
Compare the integer float parser with exact high-precision conversion, and optionally exercise the literal normalization that selects it.
*/
static bool check_wide_float_conversion(uint64_t mantissa, int exponent, bool integration) {
	soml__decimal decimal;
	soml__decimal_assign(&decimal, mantissa);
	decimal.point += exponent;
	soml__decimal reference = decimal;
	double expected = 0.0;
	CHECK(soml__decimal_to_double(&reference, &expected));
	double actual = 17.0;
	bool accepted = soml__parse_wide_float(&decimal, &actual);
	int normalized_exponent = decimal.point - decimal.count;
	bool should_accept = decimal.count > 0 && decimal.count <= 19 && normalized_exponent >= -19 && normalized_exponent <= 19;
	CHECK(accepted == should_accept);

	if (accepted) {
		if (!CHECK(memcmp(&actual, &expected, sizeof(actual)) == 0)) {
			fprintf(stderr, "Wide conversion differs for %" PRIu64 "e%d\n", mantissa, exponent);
		}
	} else {
		CHECK(actual == 17.0);
	}

	if (integration) {
		char input[80];
		int length = snprintf(input, sizeof(input), "%" PRIu64 "e%d", mantissa, exponent);
		soml__parser parser = {.text = (const unsigned char *)input, .length = (size_t)length};
		double parsed = 0.0;
		CHECK(soml__parse_float(&parser, (const unsigned char *)input, (size_t)length, 0, &parsed));
		CHECK(memcmp(&parsed, &expected, sizeof(parsed)) == 0);
	}

	return accepted;
}

/*
Compare accepted small-format intervals with the independent decimal algorithm, including its shortest-digit and tie-breaking choice.
*/
static bool check_small_float_conversion(uint64_t bits) {
	soml__decimal actual;

	if (!soml__decimal_shortest_small(&actual, bits)) {
		return false;
	}

	soml__decimal reference;
	soml__decimal_shortest(&reference, bits);
	CHECK(actual.point == reference.point && actual.count == reference.count && actual.truncated == reference.truncated);

	if (actual.count == reference.count) {
		CHECK(memcmp(actual.digits, reference.digits, (size_t)reference.count) == 0);
	}

	return true;
}

/**
Check that a clone owns distinct storage while preserving all scalar values, member order, and any source lookup index.
*/
static void check_cloned_value(const soml_value *source, const soml_value *copy, const soml_document *destination) {
	CHECK(source != copy && source != nullptr && copy != nullptr);

	if (source == nullptr || copy == nullptr) {
		return;
	}

	CHECK(source->type == copy->type && copy->document == destination);

	switch (source->type) {
		case SOML_TYPE_STRING: {
			CHECK(source->as.string.data != copy->as.string.data && source->as.string.length == copy->as.string.length);
			CHECK(memcmp(source->as.string.data, copy->as.string.data, source->as.string.length) == 0);
			CHECK(copy->as.string.data[copy->as.string.length] == '\0');
			break;
		}
		case SOML_TYPE_ARRAY: {
			CHECK(source->as.array.count == copy->as.array.count);

			for (size_t index = 0; index < source->as.array.count; index++) {
				CHECK(copy->as.array.items[index]->is_attached);
				check_cloned_value(source->as.array.items[index], copy->as.array.items[index], destination);
			}

			break;
		}
		case SOML_TYPE_OBJECT: {
			CHECK(source->as.object.count == copy->as.object.count);

			if (source->is_sorted) {
				CHECK(copy->is_sorted);

				if (source->as.object.sorted != nullptr) {
					CHECK(copy->as.object.sorted != nullptr && copy->as.object.sorted != source->as.object.sorted);
					CHECK(memcmp(source->as.object.sorted, copy->as.object.sorted, source->as.object.count * sizeof(size_t)) == 0);
				}
			}

			for (size_t index = 0; index < source->as.object.count; index++) {
				const soml__member *original = &source->as.object.members[index];
				const soml__member *cloned = &copy->as.object.members[index];
				CHECK(original->key.data != cloned->key.data && original->key.length == cloned->key.length);
				CHECK(memcmp(original->key.data, cloned->key.data, original->key.length) == 0);
				CHECK(cloned->value->is_attached);
				check_cloned_value(original->value, cloned->value, destination);
			}

			break;
		}
		case SOML_TYPE_INT: {
			CHECK(source->as.integer == copy->as.integer);
			break;
		}
		case SOML_TYPE_FLOAT: {
			CHECK(memcmp(&source->as.number, &copy->as.number, sizeof(double)) == 0);
			break;
		}
		case SOML_TYPE_BOOL: {
			CHECK(source->as.boolean == copy->as.boolean);
			break;
		}
		case SOML_TYPE_NULL: {
			break;
		}
		case SOML_TYPE_INSTANT: {
			CHECK(source->as.instant.seconds == copy->as.instant.seconds && source->as.instant.nanoseconds == copy->as.instant.nanoseconds);
			break;
		}
		case SOML_TYPE_DURATION: {
			CHECK(source->as.duration == copy->as.duration);
			break;
		}
		case SOML_TYPE_NONE: {
			CHECK(false);
			break;
		}
	}
}

/**
Build a multi-chunk tree with every scalar type and boundary, embedded NULs, empty containers, and unsorted nested objects.
*/
static soml_document *make_clone_document(const soml_allocator *allocator) {
	soml_document *document = soml_document_create(allocator);

	if (!CHECK(document != nullptr)) {
		return nullptr;
	}

	soml_value *root = soml_new_object(document);
	soml_value *array = soml_new_array(document);
	CHECK(soml_object_add(document, root, "z", 1, array));
	CHECK(soml_array_append(document, array, soml_new_int(document, INT64_MIN)));
	CHECK(soml_array_append(document, array, soml_new_int(document, INT64_MAX)));
	CHECK(soml_array_append(document, array, soml_new_duration(document, INT64_MIN)));
	CHECK(soml_array_append(document, array, soml_new_duration(document, INT64_MAX)));
	CHECK(soml_array_append(document, array, soml_new_instant(document, (soml_instant){.seconds = soml__minimum_instant_seconds})));
	CHECK(soml_array_append(document, array, soml_new_instant(document, (soml_instant){.seconds = soml__maximum_instant_seconds, .nanoseconds = 999999999})));
	static constexpr uint64_t patterns[] = {0, 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x3FF0000000000001, 0x7FEFFFFFFFFFFFFF, 0x7FF0000000000000, 0x8000000000000001, 0xFFEFFFFFFFFFFFFF, 0xFFF0000000000000};

	for (size_t index = 0; index < sizeof(patterns) / sizeof(patterns[0]); index++) {
		double number;
		memcpy(&number, &patterns[index], sizeof(number));
		CHECK(soml_array_append(document, array, soml_new_float(document, number)));
	}

	CHECK(soml_array_append(document, array, soml_new_bool(document, true)));
	CHECK(soml_array_append(document, array, soml_new_bool(document, false)));
	CHECK(soml_array_append(document, array, soml_new_null(document)));
	CHECK(soml_array_append(document, array, soml_new_string(document, nullptr, 0)));
	const char nul_bytes[] = {'a', '\0', 'b'};
	CHECK(soml_array_append(document, array, soml_new_string(document, nul_bytes, sizeof(nul_bytes))));
	CHECK(soml_array_append(document, array, soml_new_array(document)));
	CHECK(soml_array_append(document, array, soml_new_object(document)));

	for (size_t index = 0; index < 320; index++) {
		char bytes[512];
		memset(bytes, (int)('a' + index % 26), sizeof(bytes));
		bytes[index % sizeof(bytes)] = '\0';
		CHECK(soml_array_append(document, array, soml_new_string(document, bytes, sizeof(bytes))));
	}

	soml_value *nested = soml_new_object(document);
	CHECK(soml_object_add(document, nested, "z", 1, soml_new_int(document, 1)));
	CHECK(soml_object_add(document, nested, "a", 1, soml_new_int(document, 2)));
	CHECK(soml_object_add(document, root, "a", 1, nested));
	CHECK(soml_object_add(document, root, nul_bytes, sizeof(nul_bytes), soml_new_string(document, nul_bytes, sizeof(nul_bytes))));
	CHECK(soml_document_set_root(document, root));
	return document;
}

/*
A document clone must remain usable after its source is freed. A same-document value clone must be unattached and independent, so the old root can be attached beneath the new root.
*/
static void test_clone_lifetime(void) {
	for (unsigned variant = 0; variant < 3; variant++) {
		fuzz_allocator source_state = {.fail_at = SIZE_MAX};
		fuzz_allocator destination_state = {.fail_at = SIZE_MAX};
		soml_allocator source_allocator = {.reallocate = fuzz_reallocate, .context = &source_state};
		soml_allocator destination_allocator = {.reallocate = fuzz_reallocate, .context = &destination_state};
		const char parsed_text[] = "{z: [null, 1s, 0.1], a: {'z': 1, 'a': 2}, \"a\\u{0}b\": \"x\\u{0}y\"}";
		soml_document *source = variant == 2 ? soml_parse(parsed_text, sizeof(parsed_text) - 1, &source_allocator, nullptr) : make_clone_document(&source_allocator);

		if (!CHECK(source != nullptr)) {
			continue;
		}

		if (variant == 1) {
			CHECK(soml_object_index(source, source->root));
			CHECK(soml_object_index(source, soml_object_get(source->root, "a")));
		}

		soml_string expected;
		CHECK(soml_write(source, &expected, nullptr));
		char *retained = malloc(expected.length);
		memcpy(retained, expected.data, expected.length);
		size_t retained_length = expected.length;
		size_t source_blocks = source_state.live_blocks;
		soml_error error;
		soml_document *copy = soml_document_clone(source, &destination_allocator, &error);

		if (!CHECK(copy != nullptr && error.code == SOML_ERROR_NONE)) {
			free(retained);
			soml_document_free(source);
			soml_document_free(copy);
			continue;
		}
		CHECK(source_state.live_blocks == source_blocks && source->failure == SOML_ERROR_NONE);
		check_cloned_value(source->root, copy->root, copy);
		soml_document_free(source);
		CHECK(source_state.live_blocks == 0);
		soml_string actual;
		CHECK(soml_write(copy, &actual, nullptr));
		CHECK(actual.length == retained_length && memcmp(actual.data, retained, actual.length) == 0);
		const char key[] = {'a', '\0', 'b'};
		CHECK(soml_object_get_with_length(copy->root, key, sizeof(key)) != nullptr);
		free(retained);
		soml_document_free(copy);
		CHECK(destination_state.live_blocks == 0);
	}

	soml_document *source = make_clone_document(nullptr);

	if (!CHECK(source != nullptr)) {
		return;
	}

	soml_value *original = source->root;
	soml_value *copy = soml_clone_value(source, original);

	if (!CHECK(copy != nullptr && original != nullptr && !copy->is_attached && original->is_attached)) {
		soml_document_free(source);
		return;
	}

	check_cloned_value(original, copy, source);
	CHECK(soml_document_set_root(source, copy));
	CHECK(!original->is_attached && copy->is_attached);
	CHECK(soml_object_add(source, copy, "original", 8, original));
	CHECK(soml_validate(source, nullptr));
	soml_document_free(source);
}

/*
Fail each source validation allocation and each destination allocation reached by either clone API. Source ownership and builder state must remain unchanged, and every partial destination must be released.
*/
static void test_clone_failures(void) {
	fuzz_allocator source_state = {.fail_at = SIZE_MAX};
	soml_allocator source_allocator = {.reallocate = fuzz_reallocate, .context = &source_state};
	soml_document *source = make_clone_document(&source_allocator);

	if (!CHECK(source != nullptr)) {
		return;
	}

	size_t original_count = soml_count(source->root);
	bool completed = false;

	for (size_t failure = 0; failure < 64 && !completed; failure++) {
		fuzz_allocator state = {.fail_at = failure};
		soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
		soml_error error;
		soml_document *copy = soml_document_clone(source, &allocator, &error);
		completed = copy != nullptr;
		CHECK(error.code == (completed ? SOML_ERROR_NONE : SOML_ERROR_OUT_OF_MEMORY));
		CHECK(source->failure == SOML_ERROR_NONE && soml_count(source->root) == original_count);
		soml_document_free(copy);
		CHECK(state.live_blocks == 0);
	}

	CHECK(completed);
	completed = false;

	for (size_t failure = 0; failure < 64 && !completed; failure++) {
		fuzz_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
		soml_document *destination = soml_document_create(&allocator);

		if (!CHECK(destination != nullptr)) {
			soml_document_free(source);
			return;
		}

		state.fail_at = state.allocations + failure;
		soml_value *copy = soml_clone_value(destination, source->root);
		completed = copy != nullptr;
		CHECK(destination->failure == (completed ? SOML_ERROR_NONE : SOML_ERROR_OUT_OF_MEMORY));
		CHECK(destination->root == nullptr && source->failure == SOML_ERROR_NONE);
		soml_document_free(destination);
		CHECK(state.live_blocks == 0);
	}

	CHECK(completed);
	completed = false;
	size_t original_blocks = source_state.live_blocks;

	for (size_t failure = 0; failure < 8 && !completed; failure++) {
		source_state.fail_at = source_state.allocations + failure;
		fuzz_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
		soml_error error;
		soml_document *copy = soml_document_clone(source, &allocator, &error);
		completed = copy != nullptr;
		CHECK(error.code == (completed ? SOML_ERROR_NONE : SOML_ERROR_OUT_OF_MEMORY));
		CHECK(source_state.live_blocks == original_blocks && source->failure == SOML_ERROR_NONE);
		soml_document_free(copy);
		CHECK(state.live_blocks == 0);
	}

	CHECK(completed);
	source_state.fail_at = SIZE_MAX;
	soml_document_free(source);
	CHECK(source_state.live_blocks == 0);
}

/*
The hundred-container limit applies to copying. A value clone preserves unrepresentable scalar payloads for later validation, while a document clone rejects them before copying.
*/
static void test_clone_depth_and_invalid_scalars(void) {
	for (size_t depth = 100; depth <= 101; depth++) {
		soml_document *source = soml_document_create(nullptr);
		soml_value *root = soml_new_array(source);
		soml_value *current = root;

		for (size_t level = 1; level < depth; level++) {
			soml_value *next = soml_new_array(source);
			CHECK(soml_array_append(source, current, next));
			current = next;
		}

		CHECK(soml_array_append(source, current, soml_new_int(source, 1)));
		CHECK(soml_document_set_root(source, root));
		soml_document *destination = soml_document_create(nullptr);

		if (!CHECK(destination != nullptr)) {
			soml_document_free(source);
			continue;
		}

		soml_value *value = soml_clone_value(destination, root);
		CHECK((value != nullptr) == (depth == 100));
		CHECK(destination->failure == (depth == 100 ? SOML_ERROR_NONE : SOML_ERROR_TOO_DEEP));
		soml_error error;
		soml_document *copy = soml_document_clone(source, nullptr, &error);
		CHECK((copy != nullptr) == (depth == 100));
		CHECK(error.code == (depth == 100 ? SOML_ERROR_NONE : SOML_ERROR_TOO_DEEP));
		soml_document_free(copy);
		soml_document_free(destination);
		soml_document_free(source);
	}

	soml_document *source = soml_document_create(nullptr);
	soml_value *root = soml_new_array(source);
	uint64_t nan_bits = 0xFFF8000000000123;
	double nan;
	memcpy(&nan, &nan_bits, sizeof(nan));
	CHECK(soml_array_append(source, root, soml_new_float(source, nan)));
	CHECK(soml_array_append(source, root, soml_new_string(source, "\r", 1)));
	CHECK(soml_document_set_root(source, root));
	soml_document *destination = soml_document_create(nullptr);
	soml_value *copy = soml_clone_value(destination, root);
	CHECK(copy != nullptr);
	check_cloned_value(root, copy, destination);
	CHECK(soml_document_set_root(destination, copy));
	soml_error error;
	CHECK(!soml_validate(destination, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	CHECK(soml_document_clone(source, nullptr, &error) == nullptr && error.code == SOML_ERROR_INVALID_VALUE);
	soml_document_free(source);
	soml_document_free(destination);
	CHECK(soml_document_clone(nullptr, nullptr, &error) == nullptr && error.code == SOML_ERROR_INVALID_ARGUMENT);
}


/*
Independent exact models cover random nineteen-digit rationals, mantissa carry and tie parities, all supported formatting exponents, and every available floating-point rounding mode. The same comparisons run in fast-math and sanitizer builds.
*/
static void test_wide_float_model(void) {
	static const int modes[] = {
		FE_TONEAREST,
		#ifdef FE_UPWARD
		FE_UPWARD,
		#endif
		#ifdef FE_DOWNWARD
		FE_DOWNWARD,
		#endif
		#ifdef FE_TOWARDZERO
		FE_TOWARDZERO,
		#endif
	};
	static constexpr uint64_t boundaries[] = {1, 2, 3, 5, 7, 9, 10, 11, 99, 100, 101, 999, 1000, 1001, 9007199254740991, 9007199254740992, 9007199254740993, 9007199254740994, 9007199254740995, 18014398509481981, 18014398509481982, 18014398509481983, 18014398509481984, 9999999999999999999ULL, 5000000000000000001, 1000000000000000001};
	size_t fast_parses = 0;
	size_t fast_formats = 0;
	size_t tested_modes = 0;
	uint64_t saved_random_state = random_state;
	int saved_rounding_mode = fegetround();

	for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
		if (fesetround(modes[mode]) != 0) {
			continue;
		}

		tested_modes++;
		random_state = 711319;
		soml__decimal decimal = {};
		double unchanged = 17.0;
		CHECK(!soml__parse_wide_float(&decimal, &unchanged) && unchanged == 17.0);
		soml__decimal_assign(&decimal, 1234567890123456789);
		decimal.truncated = true;
		CHECK(!soml__parse_wide_float(&decimal, &unchanged) && unchanged == 17.0);
		decimal.truncated = false;
		decimal.count = 20;
		decimal.point = 20;
		decimal.digits[19] = 1;
		CHECK(!soml__parse_wide_float(&decimal, &unchanged) && unchanged == 17.0);

		for (int exponent = -22; exponent <= 22; exponent++) {
			for (size_t sample = 0; sample < sizeof(boundaries) / sizeof(boundaries[0]); sample++) {
				fast_parses += check_wide_float_conversion(boundaries[sample], exponent, true);
			}
		}

		for (int exponent = -19; exponent <= 19; exponent++) {
			for (size_t sample = 0; sample < 2000; sample++) {
				uint64_t mantissa = next_random() % 9999999999999999999ULL + 1;
				fast_parses += check_wide_float_conversion(mantissa, exponent, sample < 20);
			}
		}

		for (int exponent = -63; exponent < 0; exponent++) {
			uint64_t base = (uint64_t)(exponent + 1023 + 52) << 52;

			for (uint64_t offset = 0; offset < 64; offset++) {
				fast_formats += check_small_float_conversion(base + offset);
				fast_formats += check_small_float_conversion(base + 0xFFFFFFFFFFFFFULL - offset);
			}

			for (size_t sample = 0; sample < 500; sample++) {
				fast_formats += check_small_float_conversion(base | (next_random() & 0xFFFFFFFFFFFFFULL));
			}
		}
	}

	random_state = saved_random_state;
	CHECK(fesetround(saved_rounding_mode) == 0);
	CHECK(tested_modes > 0);
	CHECK(fast_parses > 75000 * tested_modes);
	CHECK(fast_formats > 39000 * tested_modes);
}

// MARK: Allocation failure

typedef struct counting_allocator {
	size_t allocations;
	size_t fail_at;
	// Whether every allocation after the failing one fails too, as when memory has run out.
	bool keeps_failing;
	// Whether new memory is filled with 0xAA, so that a missing NUL shows without AddressSanitizer.
	bool fills_new_memory;
	size_t live_bytes;
	size_t live_blocks;
} counting_allocator;

static void *counting_reallocate(void *context, void *pointer, size_t old_size, size_t new_size) {
	counting_allocator *allocator = context;

	if (new_size == 0) {
		allocator->live_bytes -= old_size;
		allocator->live_blocks--;
		free(pointer);
		return nullptr;
	}

	size_t index = allocator->allocations++;

	if (index == allocator->fail_at || (allocator->keeps_failing && index > allocator->fail_at)) {
		return nullptr;
	}

	// From the size, because GCC 14 reports a use after `realloc` for a check of the pointer.
	bool is_new = old_size == 0;
	void *result = realloc(pointer, new_size);

	if (result != nullptr && allocator->fills_new_memory && new_size > old_size) {
		memset((unsigned char *)result + old_size, 0xAA, new_size - old_size);
	}

	if (result != nullptr) {
		allocator->live_bytes += new_size - old_size;

		if (is_new) {
			allocator->live_blocks++;
		}
	}

	return result;
}

static const char *const allocation_document = "# Every feature.\nname: 'soml'\nwhen: 2026-09-19T14:00:00Z\n404: \"escaped \\u{1f600}\"\ntext: '''\n\tblock\n\t'''\nnumbers: [1, 2.5, 0xFF, infinity, 1h30m]\na: {b: {c: 1, d: [{x: 1}, {y: 2}]}}\nz: {}\n";

static bool write_text_ignoring(void *context, const char *bytes, size_t length) {
	(void)context;
	(void)bytes;
	(void)length;
	return true;
}

/*
Builds a document with every type of value, nested, and writes it both ways. Returns whether everything succeeded, and checks that a failure is for lack of memory.
*/
static bool build_and_write(const soml_allocator *allocator) {
	soml_document *document = soml_document_create(allocator);

	if (document == nullptr) {
		return false;
	}

	soml_value *root = soml_new_object(document);

	for (int index = 0; index < 40; index++) {
		char key[16];
		int key_length = snprintf(key, sizeof(key), "k%d", 40 - index);
		soml_value *record = soml_new_object(document);
		soml_value *list = soml_new_array(document);
		soml_object_add(document, root, key, (size_t)key_length, record);
		soml_object_add(document, record, "list", 4, list);
		soml_object_add(document, record, "when", 4, soml_new_instant(document, (soml_instant){.seconds = index, .nanoseconds = 5}));
		soml_object_add(document, record, "every", 5, soml_new_duration(document, (int64_t)index * 1'000'000'007));
		soml_array_append(document, list, soml_new_string(document, "it's", 4));
		soml_array_append(document, list, soml_new_float(document, 0.1 * index));
		soml_array_append(document, list, soml_new_bool(document, true));
		soml_array_append(document, list, soml_new_null(document));
	}

	soml_document_set_root(document, root);
	soml_string output;
	soml_error error;
	bool is_written = soml_write(document, &output, &error);
	bool is_written_with_callback = is_written && soml_write_with_callback(document, write_text_ignoring, nullptr, &error);

	// A builder failure is reported by the write, so every failure here is the allocator's.
	CHECK(is_written_with_callback || error.code == SOML_ERROR_OUT_OF_MEMORY);
	soml_document_free(document);
	return is_written_with_callback;
}

static void test_fuzz_allocator(void) {
	// The fuzz allocator keeps maximum alignment and data across forced moves, including after a failed growth or an overflowing size.
	fuzz_allocator state = {.fail_at = SIZE_MAX};
	unsigned char *bytes = fuzz_reallocate(&state, nullptr, 0, 32);
	CHECK(bytes != nullptr && (uintptr_t)bytes % alignof(max_align_t) == 0 && state.live_blocks == 1);
	memset(bytes, 0xAA, 32);
	uintptr_t previous_address = (uintptr_t)bytes;
	state.fail_at = state.allocations;
	CHECK(fuzz_reallocate(&state, bytes, 32, 64) == nullptr && state.live_blocks == 1);
	state.fail_at = SIZE_MAX;
	CHECK(fuzz_reallocate(&state, bytes, 32, SIZE_MAX) == nullptr && state.live_blocks == 1);
	#ifdef HAS_ADDRESS_SANITIZER
	CHECK(__asan_address_is_poisoned(bytes - 1) && __asan_address_is_poisoned(bytes + 32));
	#endif
	unsigned char *grown = fuzz_reallocate(&state, bytes, 32, 64);
	CHECK(grown != nullptr && (uintptr_t)grown != previous_address && (uintptr_t)grown % alignof(max_align_t) == 0 && state.live_blocks == 1);
	#ifdef HAS_ADDRESS_SANITIZER
	CHECK(__asan_address_is_poisoned(grown - 1) && __asan_address_is_poisoned(grown + 64));
	#endif

	for (size_t index = 0; index < 32; index++) {
		CHECK(grown[index] == 0xAA);
	}

	previous_address = (uintptr_t)grown;
	unsigned char *shrunk = fuzz_reallocate(&state, grown, 64, 8);
	CHECK(shrunk != nullptr && (uintptr_t)shrunk != previous_address && (uintptr_t)shrunk % alignof(max_align_t) == 0 && state.live_blocks == 1);
	#ifdef HAS_ADDRESS_SANITIZER
	CHECK(__asan_address_is_poisoned(shrunk - 1) && __asan_address_is_poisoned(shrunk + 8));
	#endif

	for (size_t index = 0; index < 8; index++) {
		CHECK(shrunk[index] == 0xAA);
	}

	CHECK(fuzz_reallocate(&state, shrunk, 8, 0) == nullptr && state.live_blocks == 0);
	CHECK(fuzz_reallocate(&state, nullptr, 0, 0) == nullptr && state.live_blocks == 0);

	#ifdef HAS_ADDRESS_SANITIZER
	// Allocator metadata must not hide underruns, including at every ASan granule offset.
	for (size_t size = 1; size <= 33; size++) {
		bytes = fuzz_reallocate(&state, nullptr, 0, size);
		CHECK(bytes != nullptr);
		memset(bytes, 0xAA, size);
		CHECK(!__asan_address_is_poisoned(bytes + size - 1));
		CHECK(__asan_address_is_poisoned(bytes - 1));
		CHECK(__asan_address_is_poisoned(bytes + size));
		CHECK(fuzz_reallocate(&state, bytes, size, 0) == nullptr && state.live_blocks == 0);
	}
	#endif
}

static void test_allocator_lifetime(void) {
	// Parsed and built documents must copy the allocator, including its context, before its struct is freed.
	char text[9'000];
	memset(text, 'x', sizeof(text));

	for (unsigned mode = 0; mode < 2; mode++) {
		fuzz_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator *allocator = malloc(sizeof(*allocator));
		*allocator = (soml_allocator){.reallocate = fuzz_reallocate, .context = &state};
		soml_document *document = mode == 0 ? soml_parse("z: 1\na: 2\n", 10, allocator, nullptr) : soml_document_create(allocator);
		free(allocator);
		CHECK(document != nullptr);
		soml_value *root = soml_document_root(document);

		if (mode == 1) {
			root = soml_new_object(document);
			CHECK(soml_object_add(document, root, "z", 1, soml_new_int(document, 1)));
			CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 2)));
			CHECK(soml_document_set_root(document, root));
		}

		CHECK(soml_object_add(document, root, "text", 4, soml_new_string(document, text, sizeof(text))));
		CHECK(state.live_blocks > 1);

		for (unsigned order = 0; order < 2; order++) {
			soml_string output;
			CHECK(order == 0 ? soml_write(document, &output, nullptr) : soml_write_canonical(document, &output, nullptr));
			collector target = {.fail_after = SIZE_MAX};
			CHECK(order == 0 ? soml_write_with_callback(document, collect, &target, nullptr) : soml_write_canonical_with_callback(document, collect, &target, nullptr));
			CHECK(target.length == output.length && memcmp(target.text, output.data, output.length) == 0);
			free(target.text);
		}

		soml_document_free(document);
		CHECK(state.live_blocks == 0);
	}
}

static void test_rejected_builder_ownership(void) {
	// Rejected calls must not attach a value or change either document's tree. The owner remains usable after the rejecting document is freed.
	for (unsigned mode = 0; mode < 2; mode++) {
		fuzz_allocator states[2] = {{.fail_at = SIZE_MAX}, {.fail_at = SIZE_MAX}};
		soml_document *documents[2];
		soml_value *roots[2];
		soml_value *arrays[2];

		for (size_t index = 0; index < 2; index++) {
			soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &states[index]};
			documents[index] = mode == 0 ? soml_parse("a: [1]\n", 7, &allocator, nullptr) : soml_document_create(&allocator);

			if (mode == 1) {
				roots[index] = soml_new_object(documents[index]);
				arrays[index] = soml_new_array(documents[index]);
				CHECK(soml_array_append(documents[index], arrays[index], soml_new_int(documents[index], 1)));
				CHECK(soml_object_add(documents[index], roots[index], "a", 1, arrays[index]));
				CHECK(soml_document_set_root(documents[index], roots[index]));
			}

			roots[index] = soml_document_root(documents[index]);
			arrays[index] = soml_object_get(roots[index], "a");
			CHECK(roots[index] != nullptr && arrays[index] != nullptr);
		}

		soml_value *foreign = soml_new_string(documents[1], "safe", 4);
		CHECK(!soml_array_append(documents[0], arrays[0], foreign));
		CHECK(!soml_object_add(documents[0], roots[0], "b", 1, foreign));
		CHECK(!soml_document_set_root(documents[0], foreign));
		CHECK(!soml_document_set_root(documents[0], roots[1]));
		CHECK(!soml_array_append(documents[0], arrays[1], foreign));
		CHECK(!soml_object_add(documents[0], roots[1], "b", 1, foreign));

		soml_value *local = soml_new_int(documents[0], 2);
		CHECK(!soml_object_add(documents[0], roots[0], nullptr, 1, local));
		CHECK(!soml_array_append(documents[0], roots[0], local));
		CHECK(soml_array_append(documents[0], arrays[0], local));

		for (size_t index = 0; index < 2; index++) {
			CHECK(soml_document_root(documents[index]) == roots[index] && soml_count(roots[index]) == 1 && soml_object_get(roots[index], "a") == arrays[index]);
			int64_t integer;
			CHECK(soml_get_int(soml_array_at(arrays[index], 0), &integer) && integer == 1);
		}

		CHECK(soml_count(arrays[0]) == 2 && soml_array_at(arrays[0], 1) == local && soml_count(arrays[1]) == 1);
		CHECK(soml_object_add(documents[1], roots[1], "b", 1, foreign));
		soml_document_free(documents[0]);
		CHECK(states[0].live_blocks == 0);

		soml_string output;
		soml_error error;
		CHECK(soml_write(documents[1], &output, &error) && error.code == SOML_ERROR_NONE && string_equals(output, "a: [\n\t1\n]\nb: 'safe'\n"));
		CHECK(soml_write_canonical(documents[1], &output, &error) && error.code == SOML_ERROR_NONE && string_equals(output, "a: [\n\t1\n]\nb: 'safe'\n"));
		for (unsigned order = 0; order < 2; order++) {
			collector target = {.fail_after = SIZE_MAX};
			CHECK(order == 0 ? soml_write_with_callback(documents[1], collect, &target, &error) : soml_write_canonical_with_callback(documents[1], collect, &target, &error));
			CHECK(error.code == SOML_ERROR_NONE && target.length == output.length && memcmp(target.text, output.data, output.length) == 0);
			free(target.text);
		}

		soml_document_free(documents[1]);
		CHECK(states[1].live_blocks == 0);
	}
}

static void test_allocation_failure_while_building(void) {
	for (int keeps_failing = 0; keeps_failing < 2; keeps_failing++) {
		bool has_succeeded = false;

		for (size_t fail_at = 0; fail_at < 10'000 && !has_succeeded; fail_at++) {
			counting_allocator state = {.fail_at = fail_at, .keeps_failing = keeps_failing == 1};
			soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
			has_succeeded = build_and_write(&allocator);
			CHECK(state.live_blocks == 0 && state.live_bytes == 0);
		}

		CHECK(has_succeeded);
	}
}

static void test_builder_state_after_allocation_failure(void) {
	// Fail each allocation during insertion. Existing entries stay readable, and an unsuccessful insertion must not claim its value or add an entry.
	for (unsigned mode = 0; mode < 2; mode++) {
		bool has_succeeded = false;

		for (size_t fail_at = 0; fail_at < 100 && !has_succeeded; fail_at++) {
			counting_allocator state = {.fail_at = SIZE_MAX};
			soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
			soml_document *document = soml_document_create(&allocator);
			soml_value *root = mode == 0 ? soml_new_array(document) : soml_new_object(document);
			CHECK(soml_document_set_root(document, root));
			soml_value *values[512];

			for (size_t index = 0; index < 512; index++) {
				values[index] = soml_new_int(document, (int64_t)index);
				CHECK(values[index] != nullptr);
			}

			state.fail_at = state.allocations + fail_at;
			bool has_failed = false;

			for (size_t index = 0; index < 512; index++) {
				char key[16];
				int key_length = snprintf(key, sizeof(key), "key%zu", 512 - index);
				bool is_added = mode == 0 ? soml_array_append(document, root, values[index]) : soml_object_add(document, root, key, (size_t)key_length, values[index]);

				if (!is_added) {
					has_failed = true;
					CHECK(soml_count(root) == index);
					CHECK(mode == 0 ? soml_array_at(root, index) == nullptr : soml_object_value_at(root, index) == nullptr && soml_object_key_at(root, index).data == nullptr);

					for (size_t previous = 0; previous < index; previous++) {
						CHECK((mode == 0 ? soml_array_at(root, previous) : soml_object_value_at(root, previous)) == values[previous]);
					}

					// The allocator recovers. Retrying with the same value proves it was not attached by the failed call.
					state.fail_at = SIZE_MAX;
					CHECK(mode == 0 ? soml_array_append(document, root, values[index]) : soml_object_add(document, root, key, (size_t)key_length, values[index]));
				}

				CHECK(soml_count(root) == index + 1);
				CHECK((mode == 0 ? soml_array_at(root, index) : soml_object_value_at(root, index)) == values[index]);
			}

			// Recovery can finish the tree, but every writer must still report the original builder failure.
			state.fail_at = SIZE_MAX;
			soml_string output = {.data = "unchanged", .length = 9};
			soml_error error;
			CHECK(has_failed ? !soml_write(document, &output, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY && string_equals(output, "unchanged") : soml_write(document, &output, &error) && error.code == SOML_ERROR_NONE);
			CHECK(has_failed ? !soml_write_canonical_with_callback(document, write_text_ignoring, nullptr, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY : soml_write_canonical_with_callback(document, write_text_ignoring, nullptr, &error) && error.code == SOML_ERROR_NONE);
			has_succeeded = !has_failed;
			soml_document_free(document);
			CHECK(state.live_blocks == 0 && state.live_bytes == 0);
		}

		CHECK(has_succeeded);
	}
}

static void test_allocation_failure(void) {
	size_t length = strlen(allocation_document);
	bool has_succeeded = false;

	for (size_t fail_at = 0; fail_at < 10'000 && !has_succeeded; fail_at++) {
		counting_allocator state = {.fail_at = fail_at};
		soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
		soml_error error;
		soml_document *document = soml_parse(allocation_document, length, &allocator, &error);

		if (document == nullptr) {
			CHECK(error.code == SOML_ERROR_OUT_OF_MEMORY);
		} else {
			soml_string output;
			bool is_written = soml_write(document, &output, &error);
			CHECK(is_written || error.code == SOML_ERROR_OUT_OF_MEMORY);
			has_succeeded = is_written;
			soml_document_free(document);
		}

		CHECK(state.live_blocks == 0 && state.live_bytes == 0);
	}

	CHECK(has_succeeded);

	// The builder records the first failure, and soml_write reports it.
	has_succeeded = false;

	for (size_t fail_at = 0; fail_at < 1000 && !has_succeeded; fail_at++) {
		counting_allocator state = {.fail_at = fail_at};
		soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
		soml_document *document = soml_document_create(&allocator);

		if (document != nullptr) {
			soml_value *root = soml_new_object(document);

			for (int index = 0; index < 200; index++) {
				char key[16];
				int key_length = snprintf(key, sizeof(key), "k%d", index);
				soml_object_add(document, root, key, (size_t)key_length, soml_new_string(document, "value", 5));
			}

			soml_document_set_root(document, root);
			soml_string output;
			soml_error error;
			has_succeeded = soml_write(document, &output, &error);
			CHECK(has_succeeded || error.code == SOML_ERROR_OUT_OF_MEMORY);
			soml_document_free(document);
		}

		CHECK(state.live_blocks == 0 && state.live_bytes == 0);
	}

	CHECK(has_succeeded);
}

static void test_write_allocation_retry(void) {
	counting_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
	soml_document *document = soml_document_create(&allocator);
	soml_value *root = soml_new_object(document);
	CHECK(soml_object_add(document, root, "b", 1, soml_new_int(document, 2)));
	CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 1)));
	CHECK(soml_document_set_root(document, root));
	soml_string output;
	CHECK(soml_write(document, &output, nullptr));
	soml_string previous = output;

	// Fail the writer's temporary key sort. The output argument and earlier output remain valid, and the document is not marked as a failed builder.
	state.fail_at = state.allocations;
	soml_error error;
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY);
	CHECK(output.data == previous.data && output.length == previous.length);
	CHECK(string_equals(previous, "b: 2\na: 1\n"));
	CHECK(soml_write(document, &output, &error) && error.code == SOML_ERROR_NONE);
	CHECK(string_equals(output, "b: 2\na: 1\n"));

	// The streaming writer also recovers from allocation failure and releases its temporary sort on every path.
	state.fail_at = state.allocations;
	CHECK(!soml_write_with_callback(document, write_text_ignoring, nullptr, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY);
	CHECK(soml_write_with_callback(document, write_text_ignoring, nullptr, &error) && error.code == SOML_ERROR_NONE);
	char long_text[5000];
	memset(long_text, 'x', sizeof(long_text));
	CHECK(soml_object_add(document, root, "c", 1, soml_new_string(document, long_text, sizeof(long_text))));
	CHECK(string_equals(previous, "b: 2\na: 1\n"));

	// A long member reaches the callback while the writer still owns its temporary sort. A callback failure must release that allocation too.
	size_t live_blocks = state.live_blocks;
	collector failing = {.fail_after = 0};
	CHECK(!soml_write_with_callback(document, collect, &failing, &error) && error.code == SOML_ERROR_WRITE);
	CHECK(failing.calls == 1 && state.live_blocks == live_blocks);
	CHECK(soml_write_with_callback(document, write_text_ignoring, nullptr, &error) && error.code == SOML_ERROR_NONE);

	// Every callback can refuse a piece, including a directly sent long string and the final flush. Nested unsorted objects must release every temporary sort, and both writer orders can be retried.
	soml_value *nested = soml_new_object(document);
	CHECK(soml_object_add(document, nested, "z", 1, soml_new_string(document, long_text, sizeof(long_text))));
	CHECK(soml_object_add(document, nested, "a", 1, soml_new_int(document, 3)));
	CHECK(soml_object_add(document, root, "nested", 6, nested));
	live_blocks = state.live_blocks;

	for (unsigned mode = 0; mode < 2; mode++) {
		collector complete = {.fail_after = SIZE_MAX};
		CHECK(mode == 0 ? soml_write_with_callback(document, collect, &complete, &error) : soml_write_canonical_with_callback(document, collect, &complete, &error));
		CHECK(complete.calls >= 5 && state.live_blocks == live_blocks);
		size_t live_bytes = state.live_bytes;

		// Fail each temporary sort, including the nested sort after part of the text has reached the callback. Every failure releases scratch memory and preserves the emitted prefix, and later writes can recover.
		bool has_succeeded = false;
		size_t allocation_failures = 0;
		state.keeps_failing = true;

		for (size_t fail_at = 0; fail_at < 100 && !has_succeeded; fail_at++) {
			state.fail_at = state.allocations + fail_at;
			collector target = {.fail_after = SIZE_MAX};
			has_succeeded = mode == 0 ? soml_write_with_callback(document, collect, &target, &error) : soml_write_canonical_with_callback(document, collect, &target, &error);
			CHECK(error.code == (has_succeeded ? SOML_ERROR_NONE : SOML_ERROR_OUT_OF_MEMORY));
			CHECK(target.length <= complete.length && (target.length == 0 || memcmp(target.text, complete.text, target.length) == 0));
			CHECK(state.live_blocks == live_blocks && state.live_bytes == live_bytes);
			CHECK(has_succeeded ? target.length == complete.length : fail_at != 1 || target.calls > 0);
			allocation_failures += !has_succeeded;
			free(target.text);
		}

		CHECK(has_succeeded && allocation_failures >= 2);
		state.fail_at = SIZE_MAX;

		for (size_t fail_after = 0; fail_after < complete.calls; fail_after++) {
			collector stopped = {.fail_after = fail_after};
			bool is_complete = mode == 0 ? soml_write_with_callback(document, collect, &stopped, &error) : soml_write_canonical_with_callback(document, collect, &stopped, &error);
			CHECK(!is_complete && error.code == SOML_ERROR_WRITE && stopped.calls == fail_after + 1);
			CHECK(stopped.length < complete.length && (stopped.length == 0 || memcmp(stopped.text, complete.text, stopped.length) == 0));
			CHECK(state.live_blocks == live_blocks);
			free(stopped.text);

			collector retry = {.fail_after = SIZE_MAX};
			CHECK(mode == 0 ? soml_write_with_callback(document, collect, &retry, &error) : soml_write_canonical_with_callback(document, collect, &retry, &error));
			CHECK(error.code == SOML_ERROR_NONE && retry.length == complete.length && memcmp(retry.text, complete.text, retry.length) == 0);
			CHECK(state.live_blocks == live_blocks);
			free(retry.text);
		}

		free(complete.text);
	}

	soml_document_free(document);
	CHECK(state.live_blocks == 0 && state.live_bytes == 0);
}

static void test_write_value_failure_retry(void) {
	// Value errors, like allocation errors, must release scratch memory and leave the document reusable. A long valid member is streamed before the bad value is encountered.
	static soml_value *(*const makers[])(soml_document *) = {make_nan, make_negative_nan, make_carriage_return, make_invalid_utf8, make_surrogate, make_early_instant, make_late_instant, make_bad_nanoseconds, make_negative_nanoseconds, make_duplicate_keys, make_duplicate_sorted_keys};
	static const char *const expected[] = {"z: 2\na: 1\n", "a: 1\nz: 2\n"};
	char text[9'000];
	memset(text, 'x', sizeof(text));

	for (size_t index = 0; index < sizeof(makers) / sizeof(makers[0]); index++) {
		counting_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
		soml_document *document = soml_document_create(&allocator);
		soml_value *valid = soml_new_object(document);
		CHECK(soml_object_add(document, valid, "z", 1, soml_new_int(document, 2)));
		CHECK(soml_object_add(document, valid, "a", 1, soml_new_int(document, 1)));
		CHECK(soml_document_set_root(document, valid));
		soml_string previous[2];
		CHECK(soml_write(document, &previous[0], nullptr));
		CHECK(soml_write_canonical(document, &previous[1], nullptr));

		soml_value *invalid = soml_new_object(document);
		CHECK(soml_object_add(document, invalid, "a", 1, soml_new_string(document, text, sizeof(text))));
		CHECK(soml_object_add(document, invalid, "z", 1, makers[index](document)));
		CHECK(soml_document_set_root(document, invalid));
		size_t live_blocks = state.live_blocks;
		size_t live_bytes = state.live_bytes;
		soml_error error;

		for (unsigned order = 0; order < 2; order++) {
			soml_string output = previous[order];
			CHECK(!(order == 0 ? soml_write(document, &output, &error) : soml_write_canonical(document, &output, &error)) && error.code == SOML_ERROR_INVALID_VALUE);
			CHECK(output.data == previous[order].data && output.length == previous[order].length && string_equals(output, expected[order]));
			CHECK(state.live_blocks == live_blocks && state.live_bytes == live_bytes);
			collector target = {.fail_after = SIZE_MAX};
			CHECK(!(order == 0 ? soml_write_with_callback(document, collect, &target, &error) : soml_write_canonical_with_callback(document, collect, &target, &error)) && error.code == SOML_ERROR_INVALID_VALUE && target.length >= sizeof(text));
			CHECK(state.live_blocks == live_blocks && state.live_bytes == live_bytes);
			free(target.text);
		}

		CHECK(soml_document_set_root(document, valid));

		for (unsigned order = 0; order < 2; order++) {
			soml_string output;
			CHECK(order == 0 ? soml_write(document, &output, &error) : soml_write_canonical(document, &output, &error));
			CHECK(error.code == SOML_ERROR_NONE && error.message == nullptr && string_equals(output, expected[order]));
			collector target = {.fail_after = SIZE_MAX};
			CHECK(order == 0 ? soml_write_with_callback(document, collect, &target, &error) : soml_write_canonical_with_callback(document, collect, &target, &error));
			CHECK(error.code == SOML_ERROR_NONE && error.message == nullptr && target.length == output.length && memcmp(target.text, output.data, output.length) == 0);
			CHECK(string_equals(previous[order], expected[order]));
			free(target.text);
		}

		soml_document_free(document);
		CHECK(state.live_blocks == 0 && state.live_bytes == 0);
	}
}

static void test_write_output_after_allocation_failure(void) {
	// Fail every allocation in each stored writer, including nested sorts, buffer growth, and ownership transfer of the collected output. Failure must preserve both the output argument and earlier output, release scratch memory, and allow a retry.
	for (unsigned mode = 0; mode < 2; mode++) {
		bool has_succeeded = false;

		for (size_t fail_at = 0; fail_at < 100 && !has_succeeded; fail_at++) {
			counting_allocator state = {.fail_at = SIZE_MAX, .keeps_failing = true};
			soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
			soml_document *document = soml_document_create(&allocator);
			soml_value *root = soml_new_object(document);
			soml_value *nested = soml_new_object(document);
			char bytes[9000];
			memset(bytes, 'x', sizeof(bytes));
			CHECK(soml_object_add(document, root, "z", 1, soml_new_string(document, bytes, sizeof(bytes))));
			CHECK(soml_object_add(document, nested, "z", 1, soml_new_int(document, 1)));
			CHECK(soml_object_add(document, nested, "a", 1, soml_new_int(document, 2)));
			CHECK(soml_object_add(document, root, "a", 1, nested));
			CHECK(soml_document_set_root(document, root));
			soml_string previous;
			CHECK(mode == 0 ? soml_write(document, &previous, nullptr) : soml_write_canonical(document, &previous, nullptr));
			char *expected = malloc(previous.length + 1);
			memcpy(expected, previous.data, previous.length + 1);
			size_t live_blocks = state.live_blocks;
			size_t live_bytes = state.live_bytes;
			state.fail_at = state.allocations + fail_at;
			soml_string output = previous;
			soml_error error;
			has_succeeded = mode == 0 ? soml_write(document, &output, &error) : soml_write_canonical(document, &output, &error);

			if (!has_succeeded) {
				CHECK(error.code == SOML_ERROR_OUT_OF_MEMORY && output.data == previous.data && output.length == previous.length);
				CHECK(state.live_blocks == live_blocks && state.live_bytes == live_bytes);
			}

			CHECK(memcmp(previous.data, expected, previous.length + 1) == 0);
			state.fail_at = SIZE_MAX;
			CHECK(mode == 0 ? soml_write_canonical(document, &output, &error) : soml_write(document, &output, &error));
			CHECK(mode == 0 ? soml_write(document, &output, &error) : soml_write_canonical(document, &output, &error));
			CHECK(error.code == SOML_ERROR_NONE && output.length == previous.length && memcmp(output.data, expected, output.length + 1) == 0);
			CHECK(memcmp(previous.data, expected, previous.length + 1) == 0);
			free(expected);
			soml_document_free(document);
			CHECK(state.live_blocks == 0 && state.live_bytes == 0);
		}

		CHECK(has_succeeded);
	}
}

static void test_write_output_after_building(void) {
	// Earlier output must outlive collection growth, arena growth, root replacement, and both writer orders.
	static const char *const expected[] = {"z: 1\na: []\n", "a: []\nz: 1\n"};
	char *text = malloc(9'000);
	memset(text, 'x', 9'000);

	for (size_t mode = 0; mode < 2; mode++) {
		fuzz_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
		soml_document *document = soml_document_create(&allocator);
		soml_value *root = soml_new_object(document);
		soml_value *array = soml_new_array(document);
		CHECK(soml_object_add(document, root, "z", 1, soml_new_int(document, 1)));
		CHECK(soml_object_add(document, root, "a", 1, array));
		CHECK(soml_document_set_root(document, root));
		soml_string previous;
		CHECK(mode == 0 ? soml_write(document, &previous, nullptr) : soml_write_canonical(document, &previous, nullptr));
		CHECK(string_equals(previous, expected[mode]));

		for (int index = 0; index < 16; index++) {
			CHECK(soml_array_append(document, array, soml_new_string(document, text, 9'000)));
			char key[16];
			int length = snprintf(key, sizeof(key), "key%d", index);
			CHECK(soml_object_add(document, root, key, (size_t)length, soml_new_int(document, index)));
			CHECK(string_equals(previous, expected[mode]) && previous.data[previous.length] == '\0');
			soml_string later;
			CHECK(soml_write(document, &later, nullptr));
			CHECK(soml_write_canonical(document, &later, nullptr));
			CHECK(string_equals(previous, expected[mode]) && previous.data[previous.length] == '\0');
		}

		CHECK(soml_document_set_root(document, soml_new_array(document)));
		soml_string later;
		CHECK(soml_write(document, &later, nullptr));
		CHECK(soml_write_canonical(document, &later, nullptr));
		CHECK(string_equals(previous, expected[mode]) && previous.data[previous.length] == '\0');
		soml_document_free(document);
		CHECK(state.live_blocks == 0);
	}

	free(text);
}

static void test_allocation_failure_in_diagnoses(bool keeps_failing) {
	// An invalid document is diagnosed with a second look at the text, which allocates too. A failure there gives the plain error at the same position, or running out of memory, and leaks nothing. An object that an error stops is checked for a duplicate key before the error, which is then the first error, so a failure in that check is running out of memory, never the later error.
	static const char *const texts[] = {"a:\n'b': 1", "a:\n\"b\": 1", "{a:\n'b':}", "a:\n\"\\q\": 1", "'x'", "'''\nx\n'''", "a: 1\n, b: 2", "a: 1\na: 2\nb: ?", "x: {a: 1, a: 2, b: ?}", "{a: 1, a: 2, b: x}", "[{a: 1, b: 2, a: 3, c: [1 2]}]", "a: 1\nb: {c: 1, c: 2, d: {e: ?}}\na: 3", "'é': 1\n'é': 2\nb: ?", "x: {'😀': 1, '😀': 2, b: ?}", "a: 1\nb: 'é' x\n", "x: '😀😀' 1"};

	for (size_t index = 0; index < sizeof(texts) / sizeof(texts[0]); index++) {
		size_t length = strlen(texts[index]);
		soml_error expected;
		CHECK(soml_parse(texts[index], length, nullptr, &expected) == nullptr && expected.code == SOML_ERROR_SYNTAX);
		bool has_succeeded = false;

		for (size_t fail_at = 0; fail_at < 1000 && !has_succeeded; fail_at++) {
			counting_allocator state = {.fail_at = fail_at, .keeps_failing = keeps_failing};
			soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
			soml_error error;
			CHECK(soml_parse(texts[index], length, &allocator, &error) == nullptr);
			CHECK(error.code == SOML_ERROR_OUT_OF_MEMORY || (error.code == SOML_ERROR_SYNTAX && error.offset == expected.offset && error.line == expected.line && error.column == expected.column));
			CHECK(state.live_blocks == 0 && state.live_bytes == 0);

			// Omitting the error output must have the same allocation and cleanup behavior.
			counting_allocator silent_state = {.fail_at = fail_at, .keeps_failing = keeps_failing};
			soml_allocator silent_allocator = {.reallocate = counting_reallocate, .context = &silent_state};
			CHECK(soml_parse(texts[index], length, &silent_allocator, nullptr) == nullptr);
			CHECK(silent_state.allocations == state.allocations && silent_state.live_blocks == 0 && silent_state.live_bytes == 0);

			// The allocation that would fail was never asked for, so the result is the one without failures.
			has_succeeded = state.allocations <= fail_at;

			if (has_succeeded) {
				CHECK(error.code == expected.code && error.offset == expected.offset && strcmp(error.message, expected.message) == 0);
			}
		}

		CHECK(has_succeeded);
	}

	// The short texts above allocate nothing new in the diagnosis. A key of 6,000 characters, indented under the entry whose value is missing, is copied there into a new block, so a failure inside the diagnosis is running out of memory, never the error without the hint about indentation.
	constexpr size_t key_length = 6'000;
	size_t length = 0;
	char *text = malloc(key_length + 16);
	length += (size_t)snprintf(text, 8, "a:\n\t");
	memset(text + length, 'b', key_length);
	length += key_length;
	memcpy(text + length, ": 1", 3);
	length += 3;
	soml_error error;
	CHECK(soml_parse(text, length, nullptr, &error) == nullptr && error.line == 2 && error.column == 2 && strstr(error.message, "Indentation does not nest objects") != nullptr);
	bool has_succeeded = false;
	size_t out_of_memory_count = 0;

	for (size_t fail_at = 0; fail_at < 1000 && !has_succeeded; fail_at++) {
		counting_allocator state = {.fail_at = fail_at, .keeps_failing = keeps_failing};
		soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
		CHECK(soml_parse(text, length, &allocator, &error) == nullptr);
		CHECK(state.live_blocks == 0 && state.live_bytes == 0);
		has_succeeded = state.allocations <= fail_at;
		out_of_memory_count += error.code == SOML_ERROR_OUT_OF_MEMORY;
		CHECK(error.code == SOML_ERROR_OUT_OF_MEMORY || (error.line == 2 && error.column == 2 && strstr(error.message, "Indentation does not nest objects") != nullptr));
	}

	CHECK(has_succeeded && out_of_memory_count > 1);
	free(text);

	// A whole document that is one long string fails as an entry at its end, and is then read again as a value, into a new block. A failure there is running out of memory, never the error at the end of the string.
	constexpr size_t string_length = 9'000;
	text = malloc(string_length + 2);
	text[0] = '\'';
	memset(text + 1, 'x', string_length);
	text[string_length + 1] = '\'';
	length = string_length + 2;
	CHECK(soml_parse(text, length, nullptr, &error) == nullptr && error.line == 1 && error.column == 1 && strstr(error.message, "A bare value is not a document") != nullptr);
	has_succeeded = false;
	out_of_memory_count = 0;

	for (size_t fail_at = 0; fail_at < 1000 && !has_succeeded; fail_at++) {
		counting_allocator state = {.fail_at = fail_at, .keeps_failing = keeps_failing};
		soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
		CHECK(soml_parse(text, length, &allocator, &error) == nullptr);
		CHECK(state.live_blocks == 0 && state.live_bytes == 0);
		has_succeeded = state.allocations <= fail_at;
		out_of_memory_count += error.code == SOML_ERROR_OUT_OF_MEMORY;
		CHECK(error.code == SOML_ERROR_OUT_OF_MEMORY || (error.line == 1 && error.column == 1 && strstr(error.message, "A bare value is not a document") != nullptr));
	}

	CHECK(has_succeeded && out_of_memory_count > 1);
	free(text);
}

static void test_built_lookup_model(void) {
	// Lookup in an object that is sorted, that stops being sorted, or that was parsed and then changed, against a list of the members in the order they were added. A duplicate key finds the first, and a write refuses it.
	static const char *const keys[] = {"", "a", "aa", "ab", "b", "ba", "c", "a\0", "\xC3\xA9", "a\0b", "aaaaaaa", "aaaaaaaa", "aaaaaaaaa", "😀"};
	static const size_t key_lengths[] = {0, 1, 2, 2, 1, 2, 1, 2, 2, 3, 7, 8, 9, 4};
	constexpr size_t key_count = sizeof(keys) / sizeof(keys[0]);

	for (int round = 0; round < 2'000; round++) {
		bool is_parsed = round % 2 == 1;
		soml_document *document = is_parsed ? parse_text("{b: 0, ab: 1, '': 2}", nullptr) : soml_document_create(nullptr);
		soml_value *object = is_parsed ? soml_document_root(document) : soml_new_object(document);
		// The key index and value of every member, in the order they were added.
		size_t model_keys[32] = {4, 3, 0};
		int64_t model_values[32] = {0, 1, 2};
		size_t model_count = is_parsed ? 3 : 0;
		size_t add_count = (size_t)(next_random() % 12);

		for (size_t add = 0; add < add_count; add++) {
			size_t key = (size_t)(next_random() % key_count);
			int64_t value = 100 + (int64_t)add;
			CHECK(soml_object_add(document, object, keys[key], key_lengths[key], soml_new_int(document, value)));
			model_keys[model_count] = key;
			model_values[model_count] = value;
			model_count++;
		}

		CHECK(soml_count(object) == model_count);
		bool has_duplicate = false;

		for (size_t key = 0; key < key_count; key++) {
			int64_t expected = -1;
			size_t matches = 0;

			for (size_t member = 0; member < model_count; member++) {
				if (model_keys[member] == key) {
					expected = matches == 0 ? model_values[member] : expected;
					matches++;
				}
			}

			has_duplicate = has_duplicate || matches > 1;

			// Each query ends at its allocation boundary, without a terminator, at every alignment.
			for (size_t offset = 0; offset < alignof(max_align_t); offset++) {
				size_t length = key_lengths[key];
				char *buffer = malloc(offset + length + (length == 0 ? 1 : 0));
				memcpy(buffer + offset, keys[key], length);
				soml_value *found = soml_object_get_with_length(object, buffer + offset, length);
				CHECK(memcmp(buffer + offset, keys[key], length) == 0);
				free(buffer);
				int64_t actual = -1;
				CHECK(found == nullptr ? expected == -1 : soml_get_int(found, &actual) && actual == expected);
			}
		}

		if (!is_parsed) {
			CHECK(soml_document_set_root(document, object));
		}

		soml_string output;
		soml_error error;
		bool is_written = soml_write(document, &output, &error);
		CHECK(is_written == !has_duplicate);
		CHECK(is_written || error.code == SOML_ERROR_INVALID_VALUE);

		// A written object reads back with every member.
		if (is_written) {
			soml_document *again = soml_parse(output.data, output.length, nullptr, nullptr);
			CHECK(soml_count(soml_document_root(again)) == model_count);

			for (size_t member = 0; member < model_count; member++) {
				int64_t actual;
				CHECK(soml_get_int(soml_object_get_with_length(soml_document_root(again), keys[model_keys[member]], key_lengths[model_keys[member]]), &actual) && actual == model_values[member]);
			}

			soml_document_free(again);
		}

		// Sorting for either writer must leave every original key and value in insertion order, also after a duplicate-key error or callback refusal.
		for (unsigned order = 0; order < 2; order++) {
			is_written = order == 0 ? soml_write(document, &output, &error) : soml_write_canonical(document, &output, &error);
			CHECK(is_written == !has_duplicate && error.code == (has_duplicate ? SOML_ERROR_INVALID_VALUE : SOML_ERROR_NONE));
			collector complete = {.fail_after = SIZE_MAX};
			bool is_complete = order == 0 ? soml_write_with_callback(document, collect, &complete, &error) : soml_write_canonical_with_callback(document, collect, &complete, &error);
			CHECK(is_complete == !has_duplicate && error.code == (has_duplicate ? SOML_ERROR_INVALID_VALUE : SOML_ERROR_NONE));
			free(complete.text);
			collector stopped = {.fail_after = 0};
			CHECK(!(order == 0 ? soml_write_with_callback(document, collect, &stopped, &error) : soml_write_canonical_with_callback(document, collect, &stopped, &error)));
			CHECK(error.code == (has_duplicate ? SOML_ERROR_INVALID_VALUE : SOML_ERROR_WRITE) && stopped.calls == (has_duplicate ? 0 : 1));
			free(stopped.text);
			CHECK(soml_document_root(document) == object && soml_count(object) == model_count);

			for (size_t member = 0; member < model_count; member++) {
				soml_string key = soml_object_key_at(object, member);
				size_t expected_key = model_keys[member];
				CHECK(key.data != nullptr && key.length == key_lengths[expected_key] && memcmp(key.data, keys[expected_key], key.length) == 0 && key.data[key.length] == '\0');
				int64_t actual = -1;
				CHECK(soml_get_int(soml_object_value_at(object, member), &actual) && actual == model_values[member]);
			}
		}

		soml_document_free(document);
	}
}

static void test_built_round_trip(void) {
	// Built durations, instants, and floats across their whole ranges read back as the same values from their canonical form.
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = soml_new_array(document);
	CHECK(soml_document_set_root(document, root));
	constexpr int count = 3'000;
	static const int64_t extreme_durations[] = {INT64_MIN, INT64_MIN + 1, INT64_MAX, -1, 1, 0, 999'999'999, 1'000'000'000, 3'599'999'999'999, 3'600'000'000'000};

	for (size_t index = 0; index < sizeof(extreme_durations) / sizeof(extreme_durations[0]); index++) {
		CHECK(soml_array_append(document, root, soml_new_duration(document, extreme_durations[index])));
	}

	for (int index = 0; index < count; index++) {
		// Every magnitude is as likely, so short durations are tried as often as long ones.
		uint64_t bits = next_random() >> (next_random() % 64);
		int64_t duration = (int64_t)(bits >> 1) * (index % 2 == 0 ? 1 : -1);
		CHECK(soml_array_append(document, root, soml_new_duration(document, duration)));

		int64_t span = soml__maximum_instant_seconds - soml__minimum_instant_seconds + 1;
		int64_t seconds = soml__minimum_instant_seconds + (int64_t)(next_random() % (uint64_t)span);
		int32_t nanoseconds = (int32_t)(next_random() % 1'000'000'000);
		// Fractions with trailing zeros are written shorter, so some are rounded to a millisecond.
		nanoseconds = index % 3 == 0 ? nanoseconds / 1'000'000 * 1'000'000 : nanoseconds;
		CHECK(soml_array_append(document, root, soml_new_instant(document, (soml_instant){.seconds = seconds, .nanoseconds = nanoseconds})));

		double number;
		uint64_t float_bits = next_random();

		// A NaN cannot be written, so it is made infinite.
		if ((float_bits & 0x7FF0000000000000) == 0x7FF0000000000000) {
			float_bits &= 0xFFF0000000000000;
		}

		memcpy(&number, &float_bits, sizeof(number));
		CHECK(soml_array_append(document, root, soml_new_float(document, number)));
	}

	CHECK(soml_array_append(document, root, soml_new_instant(document, (soml_instant){.seconds = soml__minimum_instant_seconds, .nanoseconds = 0})));
	CHECK(soml_array_append(document, root, soml_new_instant(document, (soml_instant){.seconds = soml__maximum_instant_seconds, .nanoseconds = 999'999'999})));
	soml_string output;
	CHECK(soml_write(document, &output, nullptr));
	soml_document *again = soml_parse(output.data, output.length, nullptr, nullptr);
	CHECK(again != nullptr && soml_count(soml_document_root(again)) == soml_count(root));
	size_t failures = 0;

	for (size_t index = 0; index < soml_count(root); index++) {
		const soml_value *built = soml_array_at(root, index);
		const soml_value *read = soml_array_at(soml_document_root(again), index);
		int64_t built_duration;
		int64_t read_duration;
		soml_instant built_instant;
		soml_instant read_instant;
		double built_number;
		double read_number;
		bool is_equal = false;

		if (soml_get_duration(built, &built_duration)) {
			is_equal = soml_get_duration(read, &read_duration) && read_duration == built_duration;
		} else if (soml_get_instant(built, &built_instant)) {
			is_equal = soml_get_instant(read, &read_instant) && read_instant.seconds == built_instant.seconds && read_instant.nanoseconds == built_instant.nanoseconds;
		} else if (soml_get_float(built, &built_number)) {
			// Negative zero is stored as zero, so the bits compare.
			is_equal = soml_get_float(read, &read_number) && memcmp(&read_number, &built_number, sizeof(read_number)) == 0;
		}

		if (!is_equal && failures++ < 5) {
			fprintf(stderr, "Built value %zu did not read back the same\n", index);
		}
	}

	CHECK(failures == 0);

	// The canonical form writes back to the same bytes.
	soml_string second;
	CHECK(soml_write(again, &second, nullptr) && second.length == output.length && memcmp(second.data, output.data, output.length) == 0);
	soml_document_free(again);
	soml_document_free(document);
}

// MARK: Complexity

typedef char *(*input_maker)(size_t size, size_t *length);

static double seconds_to_parse(input_maker make, size_t size, soml_error_code expected_error) {
	size_t length;
	char *text = make(size, &length);
	clock_t start = clock();
	soml_error error;
	soml_document *document = soml_parse(text, length, nullptr, &error);
	soml_string output;
	CHECK(error.code == expected_error && (document != nullptr) == (expected_error == SOML_ERROR_NONE));

	if (document != nullptr && !CHECK(soml_write(document, &output, &error))) {
		fprintf(stderr, "Could not write: %s\n", error.message);
	}

	double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
	soml_document_free(document);
	free(text);
	return seconds;
}

/*
The fastest of three runs, which keeps a pause of the machine from making a linear input look quadratic.
*/
static double fastest_seconds_to_parse(input_maker make, size_t size, soml_error_code expected_error) {
	double fastest = seconds_to_parse(make, size, expected_error);

	for (int run = 1; run < 3; run++) {
		double seconds = seconds_to_parse(make, size, expected_error);
		fastest = seconds < fastest ? seconds : fastest;
	}

	return fastest;
}

static char *make_wide_object(size_t size, size_t *length) {
	size_t capacity = size * 32;
	char *text = malloc(capacity);
	size_t used = 0;

	for (size_t index = 0; index < size; index++) {
		// Keys in reverse order, so sorting has work to do.
		used += (size_t)snprintf(text + used, capacity - used, "key%zu: %zu\n", size - index, index);
	}

	*length = used;
	return text;
}

/*
A wide object, braced and inside the top-level object, that an error stops before it closes, so its entries are checked for collisions on the way out.
*/
static char *make_wide_object_with_error(size_t size, size_t *length) {
	size_t capacity = size * 32 + 32;
	char *text = malloc(capacity);
	size_t used = (size_t)snprintf(text, capacity, "outer: {\n");

	for (size_t index = 0; index < size; index++) {
		used += (size_t)snprintf(text + used, capacity - used, "key%zu: %zu\n", size - index, index);
	}

	used += (size_t)snprintf(text + used, capacity - used, "last: ?\n}\n");
	*length = used;
	return text;
}

static char *make_long_string(size_t size, size_t *length) {
	size_t capacity = size * 9 + 16;
	char *text = malloc(capacity);
	size_t used = (size_t)snprintf(text, capacity, "a: \"");

	for (size_t index = 0; index < size; index++) {
		memcpy(text + used, "x\\n\\u{e9}", 9);
		used += 9;
	}

	used += (size_t)snprintf(text + used, capacity - used, "\"");
	*length = used;
	return text;
}

static char *make_long_number(size_t size, size_t *length) {
	size_t capacity = size * 8 + 16;
	char *text = malloc(capacity);
	size_t used = (size_t)snprintf(text, capacity, "a: 1.");
	memset(text + used, '3', size * 8);
	used += size * 8;
	*length = used;
	return text;
}

static char *make_many_comments(size_t size, size_t *length) {
	size_t capacity = size * 32 + 16;
	char *text = malloc(capacity);
	size_t used = 0;

	for (size_t index = 0; index < size; index++) {
		used += (size_t)snprintf(text + used, capacity - used, "/* c */ # line\n");
	}

	used += (size_t)snprintf(text + used, capacity - used, "a: 1");
	*length = used;
	return text;
}

static char *make_long_block_string(size_t size, size_t *length) {
	size_t capacity = size * 16 + 32;
	char *text = malloc(capacity);
	size_t used = (size_t)snprintf(text, capacity, "a: '''\n");

	for (size_t index = 0; index < size; index++) {
		used += (size_t)snprintf(text + used, capacity - used, "\t\tline\n\n");
	}

	used += (size_t)snprintf(text + used, capacity - used, "\t\t'''");
	*length = used;
	return text;
}

static char *make_wide_array(size_t size, size_t *length) {
	size_t capacity = size * 32 + 16;
	char *text = malloc(capacity);
	size_t used = (size_t)snprintf(text, capacity, "[");

	for (size_t index = 0; index < size; index++) {
		used += (size_t)snprintf(text + used, capacity - used, "{a: [%zu, 1.5, 'x']},", index);
	}

	used += (size_t)snprintf(text + used, capacity - used, "]");
	*length = used;
	return text;
}

static char *make_many_small_objects(size_t size, size_t *length) {
	size_t capacity = size * 16 + 16;
	char *text = malloc(capacity);
	size_t used = (size_t)snprintf(text, capacity, "[");

	for (size_t index = 0; index < size; index++) {
		used += (size_t)snprintf(text + used, capacity - used, "{b: 1, a: {}},");
	}

	used += (size_t)snprintf(text + used, capacity - used, "]");
	*length = used;
	return text;
}

static char *make_shared_key_prefixes(size_t size, size_t *length) {
	// Keys that share a long prefix make every comparison in the sort expensive.
	size_t prefix_length = 200;
	size_t capacity = size * (prefix_length + 32) + 16;
	char *text = malloc(capacity);
	size_t used = 0;

	for (size_t index = 0; index < size; index++) {
		memset(text + used, 'k', prefix_length);
		used += prefix_length;
		used += (size_t)snprintf(text + used, capacity - used, "%zu: 1\n", size - index);
	}

	*length = used;
	return text;
}

/*
`a: 1.` and `count` zeros, then `ending`. A fraction of trailing zeros is a valid duration, and a digit after them is too fine.
*/
static char *make_duration_fraction(size_t count, const char *ending, size_t *length) {
	size_t ending_length = strlen(ending);
	char *text = malloc(count + ending_length + 8);
	size_t used = (size_t)snprintf(text, 8, "a: 1.");
	memset(text + used, '0', count);
	used += count;
	memcpy(text + used, ending, ending_length);
	*length = used + ending_length;
	return text;
}

static char *make_long_duration_fraction(size_t size, size_t *length) {
	return make_duration_fraction(size * 8, "s", length);
}

static char *make_too_fine_duration_fraction(size_t size, size_t *length) {
	return make_duration_fraction(size * 8, "5s", length);
}

[[maybe_unused]] static void test_complexity(void) {
	static const struct {
		const char *name;
		input_maker make;
		size_t size;
		soml_error_code expected_error;
	} inputs[] = {
		{"wide object", make_wide_object, 25'000, SOML_ERROR_NONE},
		{"wide object with an error", make_wide_object_with_error, 25'000, SOML_ERROR_SYNTAX},
		{"long string", make_long_string, 100'000, SOML_ERROR_NONE},
		{"long number", make_long_number, 100'000, SOML_ERROR_NONE},
		{"many comments", make_many_comments, 50'000, SOML_ERROR_NONE},
		{"long block string", make_long_block_string, 50'000, SOML_ERROR_NONE},
		{"wide array", make_wide_array, 25'000, SOML_ERROR_NONE},
		{"many small objects", make_many_small_objects, 25'000, SOML_ERROR_NONE},
		{"shared key prefixes", make_shared_key_prefixes, 10'000, SOML_ERROR_NONE},
		{"long duration fraction", make_long_duration_fraction, 100'000, SOML_ERROR_NONE},
		{"too fine duration fraction", make_too_fine_duration_fraction, 100'000, SOML_ERROR_SYNTAX},
	};

	for (size_t index = 0; index < sizeof(inputs) / sizeof(inputs[0]); index++) {
		double small = fastest_seconds_to_parse(inputs[index].make, inputs[index].size, inputs[index].expected_error);
		double large = fastest_seconds_to_parse(inputs[index].make, inputs[index].size * 8, inputs[index].expected_error);

		// Linear or n log n time gives about 8 to 10 times as long. Quadratic time gives 64.
		bool is_linear = large < 0.05 || large < small * 24;

		if (!is_linear) {
			fprintf(stderr, "%s: %.3f s for 1×, %.3f s for 8×\n", inputs[index].name, small, large);
		}

		CHECK(is_linear);
	}
}

static void test_reads_at_the_end_of_the_text(void) {
	// Each text ends where a check looks one character ahead, so AddressSanitizer reports a read past its end.
	CHECK(is_rejected("a: 1 /* x *", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 1 /", SOML_ERROR_SYNTAX));
	CHECK(has_error_at("a: 2026-09-19", 1, 4, "A date is not an instant"));
	CHECK(has_error_at("a: 2026-09-19 ", 1, 4, "A date is not an instant"));
	CHECK(has_error_at("a: 2026-09-19 1", 1, 4, "A date is not an instant"));
	CHECK(has_error_at("a: 2026-09-19 14:", 1, 4, "A date is not an instant"));
	CHECK(has_error_at("a: 2026-09-19 14:00", 1, 4, "uppercase “T”"));
	CHECK(has_error_at("a: 2026-09-19 14:00:00", 1, 4, "without an offset is a local time"));
	CHECK(has_error_at("a: 2026-09-19 14:00:00Z", 1, 4, "uppercase “T”"));
	CHECK(is_rejected("a: 2026-09-19T14:00:0", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 2026-09-19T14:00:00", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 2026-09-19T14:00:00+07:0", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 'x", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: \"x\\", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: \"\\u{1", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: '''\nx\n''", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 1.", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: 1h1.", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: tru", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("a: -infinit", SOML_ERROR_SYNTAX));
	CHECK(is_rejected("\"a", SOML_ERROR_SYNTAX));

	// Every prefix and every suffix of a document with every feature, each in a block of exactly its size.
	size_t length = strlen(allocation_document);

	for (size_t cut = 0; cut <= length; cut++) {
		char *prefix = malloc(cut == 0 ? 1 : cut);
		memcpy(prefix, allocation_document, cut);
		soml_document_free(soml_parse(prefix, cut, nullptr, nullptr));
		free(prefix);

		char *suffix = malloc(length - cut == 0 ? 1 : length - cut);
		memcpy(suffix, allocation_document + cut, length - cut);
		soml_document_free(soml_parse(suffix, length - cut, nullptr, nullptr));
		free(suffix);
	}
}

static void test_misaligned_buffers(void) {
	// Every truncated prefix ends at its allocation boundary, at every alignment. Its result must match an aligned input, including the error position, and parsing must leave the input unchanged.
	size_t length = strlen(allocation_document);

	for (size_t cut = 0; cut <= length; cut++) {
		soml_error expected_error;
		soml_document *expected = soml_parse(allocation_document, cut, nullptr, &expected_error);
		soml_string expected_output = {};

		if (expected != nullptr) {
			CHECK(soml_write_canonical(expected, &expected_output, nullptr));
		}

		for (size_t offset = 0; offset < alignof(max_align_t); offset++) {
			char *buffer = malloc(offset + cut + (cut == 0 ? 1 : 0));
			memcpy(buffer + offset, allocation_document, cut);
			soml_error error;
			soml_document *document = soml_parse(buffer + offset, cut, nullptr, &error);
			CHECK(memcmp(buffer + offset, allocation_document, cut) == 0);
			free(buffer);
			CHECK((document != nullptr) == (expected != nullptr) && error.code == expected_error.code && error.offset == expected_error.offset);

			if (document != nullptr) {
				soml_string output;
				CHECK(soml_write_canonical(document, &output, nullptr) && output.length == expected_output.length && memcmp(output.data, expected_output.data, output.length) == 0);
			}

			soml_document_free(document);
		}

		soml_document_free(expected);
	}

	// Serialized text can also be read from an unaligned, unterminated buffer.
	soml_document *document = parse_text(allocation_document, nullptr);
	soml_string output;
	CHECK(soml_write_canonical(document, &output, nullptr));

	for (size_t offset = 0; offset < alignof(max_align_t); offset++) {
		char *buffer = malloc(offset + output.length);
		memcpy(buffer + offset, output.data, output.length);
		soml_document *parsed = soml_parse(buffer + offset, output.length, nullptr, nullptr);
		CHECK(memcmp(buffer + offset, output.data, output.length) == 0);
		free(buffer);
		soml_string again;
		CHECK(parsed != nullptr && soml_write_canonical(parsed, &again, nullptr) && again.length == output.length && memcmp(again.data, output.data, output.length) == 0);
		soml_document_free(parsed);
	}

	soml_document_free(document);
}

static void test_every_control_escape(void) {
	// Each C0 control other than tab and line feed, and DEL, is written as \u{…} with its own value, without leading zeros, and reads back.
	for (int byte = 0; byte < 0x80; byte++) {
		if ((byte >= 0x20 && byte != 0x7F) || byte == '\t' || byte == '\n' || byte == '\r') {
			continue;
		}

		char text[32];
		char expected[32];
		snprintf(text, sizeof(text), "a: \"\\u{%x}\"\n", byte);
		snprintf(expected, sizeof(expected), "a: \"\\u{%x}\"\n", byte);
		CHECK(has_canonical_form(text, expected));
	}

	CHECK(has_canonical_form("a: \"\\t\\n\"\n", "a: \"\\t\\n\"\n"));
}

/*
Makes `count` ints in a document, and then appends them to its root array, with the allocation `fail_at` after the ints failing. An append fails exactly when an allocation does, and the writers then report it. Returns the number of allocations that the appends asked for.
*/
static size_t append_ints_failing_at(size_t count, size_t fail_at) {
	counting_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
	soml_document *document = soml_document_create(&allocator);
	soml_value *root = soml_new_array(document);
	soml_value **items = malloc(count * sizeof(soml_value *));

	for (size_t index = 0; index < count; index++) {
		items[index] = soml_new_int(document, (int64_t)index);
	}

	CHECK(soml_document_set_root(document, root));
	size_t start = state.allocations;
	state.fail_at = fail_at == SIZE_MAX ? SIZE_MAX : start + fail_at;
	bool is_complete = true;

	for (size_t index = 0; index < count; index++) {
		is_complete = soml_array_append(document, root, items[index]) && is_complete;
	}

	size_t append_allocations = state.allocations - start;
	state.fail_at = SIZE_MAX;
	soml_string output;
	soml_error error;
	CHECK(is_complete == (fail_at >= append_allocations));
	CHECK(is_complete ? soml_write(document, &output, &error) : !soml_write(document, &output, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY);
	free(items);
	soml_document_free(document);
	CHECK(state.live_blocks == 0 && state.live_bytes == 0);
	return append_allocations;
}

static void test_builder_failures_are_recorded(void) {
	soml_string output = {};
	soml_error error;

	// An append that runs out of memory makes the writers fail, also when the caller ignores it. The ints are made first, and enough of them that their appends need memory from the allocator, which depends on the size of a pointer.
	size_t count = 1'000;
	size_t append_allocations = 0;

	while (append_allocations == 0) {
		count *= 2;
		append_allocations = append_ints_failing_at(count, SIZE_MAX);
	}

	for (size_t fail_at = 0; fail_at < append_allocations; fail_at++) {
		append_ints_failing_at(count, fail_at);
	}

	// A `nullptr` key with a length, a `nullptr` root, a `nullptr` string with a length, and a container of the other type are failures that every writer reports, also after a valid call.
	for (int failure = 0; failure < 4; failure++) {
		soml_document *document = soml_document_create(nullptr);
		soml_value *root = soml_new_object(document);

		if (failure == 0) {
			CHECK(!soml_object_add(document, root, nullptr, 1, soml_new_int(document, 1)));
		} else if (failure == 1) {
			CHECK(!soml_document_set_root(document, nullptr));
		} else if (failure == 2) {
			CHECK(soml_new_string(document, nullptr, 1) == nullptr);
		} else {
			CHECK(!soml_array_append(document, root, soml_new_int(document, 1)));
		}

		CHECK(soml_document_set_root(document, root));
		CHECK(soml_object_add(document, root, "b", 1, soml_new_int(document, 2)));
		output = (soml_string){};
		CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && output.data == nullptr);
		CHECK(!soml_write_canonical(document, &output, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
		collector target = {.fail_after = SIZE_MAX};
		CHECK(!soml_write_with_callback(document, collect, &target, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
		CHECK(!soml_write_canonical_with_callback(document, collect, &target, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
		CHECK(target.length == 0);
		free(target.text);
		soml_document_free(document);
	}
}

static void test_first_builder_failure(void) {
	// Mixed failures must preserve the first code and message, even after the allocator recovers and building continues. Error messages are static and outlive the document.
	char text[9'000];
	memset(text, 'x', sizeof(text));

	for (unsigned order = 0; order < 2; order++) {
		counting_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
		soml_document *document = soml_document_create(&allocator);
		soml_value *root = soml_new_object(document);
		CHECK(soml_document_set_root(document, root));

		if (order == 0) {
			CHECK(!soml_document_set_root(document, nullptr));
		} else {
			state.fail_at = state.allocations;
			CHECK(soml_new_string(document, text, sizeof(text)) == nullptr);
		}

		soml_error error;
		soml_error_code expected_code = order == 0 ? SOML_ERROR_INVALID_ARGUMENT : SOML_ERROR_OUT_OF_MEMORY;
		soml_string output = {.data = "unchanged", .length = 9};
		CHECK(!soml_write(document, &output, &error) && error.code == expected_code && error.message != nullptr);
		char *expected_message = malloc(strlen(error.message) + 1);
		strcpy(expected_message, error.message);

		if (order == 0) {
			state.fail_at = state.allocations;
			CHECK(soml_new_string(document, text, sizeof(text)) == nullptr);
		} else {
			CHECK(!soml_document_set_root(document, nullptr));
		}

		CHECK(soml_new_string(document, nullptr, 1) == nullptr);
		state.fail_at = SIZE_MAX;
		CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 2)));
		CHECK(!soml_write(document, &output, &error) && error.code == expected_code && strcmp(error.message, expected_message) == 0 && string_equals(output, "unchanged"));
		CHECK(!soml_write_canonical(document, &output, &error) && error.code == expected_code && strcmp(error.message, expected_message) == 0 && string_equals(output, "unchanged"));
		collector target = {.fail_after = SIZE_MAX};
		CHECK(!soml_write_with_callback(document, collect, &target, &error) && error.code == expected_code && strcmp(error.message, expected_message) == 0);
		CHECK(!soml_write_canonical_with_callback(document, collect, &target, &error) && error.code == expected_code && strcmp(error.message, expected_message) == 0);
		CHECK(target.length == 0);
		free(target.text);
		soml_document_free(document);
		CHECK(state.live_blocks == 0 && state.live_bytes == 0);
		CHECK(strcmp(error.message, expected_message) == 0);
		free(expected_message);
	}
}

static void test_instant_range(void) {
	// Validate both fields before formatting, including full-width extremes, while preserving the builder's original fields. Every writer accepts exactly the documented range.
	static const int64_t seconds[] = {INT64_MIN, -62'135'596'801, -62'135'596'800, -62'135'596'799, -1, 0, 1, 253'402'300'798, 253'402'300'799, 253'402'300'800, INT64_MAX};
	static const int32_t nanoseconds[] = {INT32_MIN, -1, 0, 1, 999'999'998, 999'999'999, 1'000'000'000, INT32_MAX};

	for (size_t second = 0; second < sizeof(seconds) / sizeof(seconds[0]); second++) {
		for (size_t fraction = 0; fraction < sizeof(nanoseconds) / sizeof(nanoseconds[0]); fraction++) {
			fuzz_allocator state = {.fail_at = SIZE_MAX};
			soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
			soml_document *document = soml_document_create(&allocator);
			soml_value *root = soml_new_array(document);
			soml_instant expected = {.seconds = seconds[second], .nanoseconds = nanoseconds[fraction]};
			soml_value *value = soml_new_instant(document, expected);
			CHECK(soml_array_append(document, root, value));
			CHECK(soml_document_set_root(document, root));
			soml_instant actual = {};
			CHECK(soml_get_instant(value, &actual) && actual.seconds == expected.seconds && actual.nanoseconds == expected.nanoseconds);
			bool is_valid = expected.seconds >= -62'135'596'800 && expected.seconds <= 253'402'300'799 && expected.nanoseconds >= 0 && expected.nanoseconds < 1'000'000'000;

			for (unsigned order = 0; order < 2; order++) {
				soml_string output = {.data = "unchanged", .length = 9};
				soml_error error;
				bool is_written = order == 0 ? soml_write(document, &output, &error) : soml_write_canonical(document, &output, &error);
				CHECK(is_written == is_valid && error.code == (is_valid ? SOML_ERROR_NONE : SOML_ERROR_INVALID_VALUE));

				if (is_written) {
					soml_document *parsed = soml_parse(output.data, output.length, nullptr, nullptr);
					CHECK(parsed != nullptr && soml_get_instant(soml_array_at(soml_document_root(parsed), 0), &actual) && actual.seconds == expected.seconds && actual.nanoseconds == expected.nanoseconds);
					soml_document_free(parsed);
				} else {
					CHECK(string_equals(output, "unchanged"));
				}

				collector target = {.fail_after = SIZE_MAX};
				bool is_streamed = order == 0 ? soml_write_with_callback(document, collect, &target, &error) : soml_write_canonical_with_callback(document, collect, &target, &error);
				CHECK(is_streamed == is_valid && error.code == (is_valid ? SOML_ERROR_NONE : SOML_ERROR_INVALID_VALUE));

				if (is_streamed) {
					CHECK(target.length == output.length && memcmp(target.text, output.data, output.length) == 0);
				}

				free(target.text);
			}

			soml_document_free(document);
			CHECK(state.live_blocks == 0);
		}
	}
}

static void test_builder_scalar_storage(void) {
	soml_document *document = soml_document_create(nullptr);
	CHECK(document != nullptr);

	for (int value = 0; value < 2; value++) {
		bool expected = value != 0;
		bool actual = !expected;
		CHECK(soml_get_bool(soml_new_bool(document, expected), &actual) && actual == expected);
	}

	CHECK(soml_is_null(soml_new_null(document)));

	// Both signs of zero, subnormal endpoints, normal endpoints, values with more than float precision, and infinities must survive construction without rounding. Negative zero alone becomes positive zero.
	static const uint64_t float_patterns[] = {0, 0x8000000000000000, 1, 0x8000000000000001, 0x000FFFFFFFFFFFFF, 0x800FFFFFFFFFFFFF, 0x0010000000000000, 0x8010000000000000, 0x3FF0000000000001, 0xBFF0000000000001, 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x7FF0000000000000, 0xFFF0000000000000};

	for (size_t index = 0; index < sizeof(float_patterns) / sizeof(float_patterns[0]); index++) {
		double expected;
		memcpy(&expected, &float_patterns[index], sizeof(expected));
		uint64_t actual_bits = ~float_patterns[index];
		double actual;
		memcpy(&actual, &actual_bits, sizeof(actual));
		CHECK(soml_get_float(soml_new_float(document, expected), &actual));
		memcpy(&actual_bits, &actual, sizeof(actual_bits));
		CHECK(actual_bits == (index == 1 ? 0 : float_patterns[index]));
	}

	// Every bit, its predecessor, and its complement cover both signs and the full-width endpoints without signed arithmetic overflow.
	for (size_t bit = 0; bit < 64; bit++) {
		uint64_t single_bit = (uint64_t)1 << bit;
		const uint64_t patterns[] = {single_bit, single_bit - 1, ~single_bit};

		for (size_t index = 0; index < sizeof(patterns) / sizeof(patterns[0]); index++) {
			int64_t expected;
			memcpy(&expected, &patterns[index], sizeof(expected));
			int64_t actual = ~expected;
			CHECK(soml_get_int(soml_new_int(document, expected), &actual) && actual == expected);
			actual = ~expected;
			CHECK(soml_get_duration(soml_new_duration(document, expected), &actual) && actual == expected);
		}
	}

	// Seconds at the ends of the supported range, on both sides of the epoch, and beyond 32 bits must retain every nanosecond digit.
	static const int64_t seconds[] = {-62'135'596'800, -4'294'967'296, -1, 0, 1, 4'294'967'296, 253'402'300'799};
	static const int32_t nanoseconds[] = {0, 1, 999'999'998, 999'999'999};

	for (size_t second = 0; second < sizeof(seconds) / sizeof(seconds[0]); second++) {
		for (size_t fraction = 0; fraction < sizeof(nanoseconds) / sizeof(nanoseconds[0]); fraction++) {
			soml_instant expected = {.seconds = seconds[second], .nanoseconds = nanoseconds[fraction]};
				soml_instant actual = {.seconds = ~expected.seconds, .nanoseconds = ~expected.nanoseconds};
			CHECK(soml_get_instant(soml_new_instant(document, expected), &actual) && actual.seconds == expected.seconds && actual.nanoseconds == expected.nanoseconds);
		}
	}

	soml_document_free(document);
}

static void test_value_details(void) {
	// The builder stores negative zero as zero, which reads back with no sign.
	soml_document *document = soml_document_create(nullptr);
	double number = 1.0;
	double zero = 0.0;
	CHECK(soml_get_float(soml_new_float(document, -0.0), &number) && memcmp(&number, &zero, sizeof(number)) == 0);
	soml_document_free(document);

	// An index equal to the count is out of range.
	document = parse_text("a: [1, 2]\nb: []\nc: {}\n", nullptr);
	soml_value *root = soml_document_root(document);
	CHECK(soml_object_key_at(root, 3).data == nullptr && soml_object_key_at(root, 3).length == 0);
	CHECK(soml_object_value_at(root, 3) == nullptr);
	CHECK(soml_array_at(soml_object_get(root, "a"), 2) == nullptr);
	CHECK(soml_object_key_at(soml_object_get(root, "c"), 0).data == nullptr);

	// Every item and member of a parsed document is in a container, the first one too.
	soml_value *items = soml_object_get(root, "a");
	soml_value *empty = soml_object_get(root, "b");
	CHECK(!soml_array_append(document, empty, soml_array_at(items, 0)));
	CHECK(!soml_array_append(document, empty, soml_array_at(items, 1)));
	CHECK(!soml_array_append(document, empty, soml_object_value_at(root, 0)));
	CHECK(soml_count(empty) == 0);
	soml_document_free(document);

	// Every kind of string and key is followed by a NUL, in memory that is not zeroed.
	counting_allocator state = {.fail_at = SIZE_MAX, .fills_new_memory = true};
	soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
	const char text[] = "a: '''\n\tblock\n\t'''\nb: \"esc\\n\"\nc: 'lit'\n\"d\\t\": \"\"\"\n\tx\\ty\n\t\"\"\"\n'e': ''\n";
	document = soml_parse(text, sizeof(text) - 1, &allocator, nullptr);
	root = soml_document_root(document);
	CHECK(soml_count(root) == 5);

	for (size_t index = 0; index < soml_count(root); index++) {
		soml_string key = soml_object_key_at(root, index);
		soml_string string = {};
		CHECK(key.data[key.length] == '\0');
		CHECK(soml_get_string(soml_object_value_at(root, index), &string) && string.data[string.length] == '\0');
	}

	soml_string output;
	CHECK(soml_write(document, &output, nullptr) && output.data[output.length] == '\0');
	soml_document_free(document);
	CHECK(state.live_blocks == 0 && state.live_bytes == 0);
}

static void test_writer_frees_its_memory(void) {
	// The writer frees the order it sorts an unsorted object in, also when it refuses a duplicate key, and when the callback stops it at any piece.
	for (int is_canonical = 0; is_canonical < 2; is_canonical++) {
		counting_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
		soml_document *document = soml_document_create(&allocator);
		soml_value *root = soml_new_object(document);
		soml_value *inner = soml_new_object(document);
		soml_object_add(document, inner, "y", 1, soml_new_int(document, 1));
		soml_object_add(document, inner, "x", 1, soml_new_int(document, 2));
		soml_object_add(document, inner, "y", 1, soml_new_int(document, 3));
		soml_object_add(document, root, "b", 1, soml_new_int(document, 1));
		soml_object_add(document, root, "a", 1, inner);
		CHECK(soml_document_set_root(document, root));
		size_t live_blocks = state.live_blocks;
		size_t live_bytes = state.live_bytes;
		soml_string output;
		soml_error error;
		CHECK(!(is_canonical ? soml_write_canonical(document, &output, &error) : soml_write(document, &output, &error)) && error.code == SOML_ERROR_INVALID_VALUE);
		CHECK(state.live_blocks == live_blocks && state.live_bytes == live_bytes);
		soml_document_free(document);
		CHECK(state.live_blocks == 0 && state.live_bytes == 0);
	}

	// Unsorted objects at several levels, so that a sort order is held at each while the callback stops at each piece.
	counting_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
	soml_document *document = soml_document_create(&allocator);
	soml_value *root = soml_new_object(document);
	soml_value *parent = root;

	for (int level = 0; level < 20; level++) {
		soml_value *child = soml_new_object(document);

		for (int index = 300; index > 0; index--) {
			char key[16];
			int key_length = snprintf(key, sizeof(key), "k%03d", index);
			soml_object_add(document, parent, key, (size_t)key_length, soml_new_string(document, "value", 5));
		}

		soml_object_add(document, parent, "child", 5, child);
		parent = child;
	}

	CHECK(soml_document_set_root(document, root));
	size_t live_blocks = state.live_blocks;
	size_t live_bytes = state.live_bytes;

	for (int is_canonical = 0; is_canonical < 2; is_canonical++) {
		soml_string expected;
		CHECK(is_canonical ? soml_write_canonical(document, &expected, nullptr) : soml_write(document, &expected, nullptr));
		live_blocks = state.live_blocks;
		live_bytes = state.live_bytes;
		bool has_succeeded = false;

		for (size_t fail_after = 0; !has_succeeded; fail_after++) {
			collector target = {.fail_after = fail_after};
			soml_error error;
			has_succeeded = is_canonical ? soml_write_canonical_with_callback(document, collect, &target, &error) : soml_write_with_callback(document, collect, &target, &error);
			CHECK(has_succeeded || error.code == SOML_ERROR_WRITE);
			CHECK(state.live_blocks == live_blocks && state.live_bytes == live_bytes);

			// The canonical callback sorts a large object as soml_write_canonical does.
			if (has_succeeded) {
				CHECK(target.length == expected.length && memcmp(target.text, expected.data, expected.length) == 0);
			}

			free(target.text);
		}
	}

	soml_document_free(document);
	CHECK(state.live_blocks == 0 && state.live_bytes == 0);
}

// Large temporary output buffers use heap storage so the WebAssembly stack remains available for deep-container calls.
/*
Compare duration fractions with wider integer arithmetic independent of the parser's decimal normalization and unit conversion.
*/
static void test_duration_fraction_model(void) {
	typedef unsigned _BitInt(128) wide_unsigned;
	static const char *const units[] = {"h", "m", "s", "ms", "us", "ns"};
	static constexpr uint64_t scales[] = {3600000000000, 60000000000, 1000000000, 1000000, 1000, 1};
	static constexpr uint64_t numerators[] = {0, 1, 2, 5, 7, 9, 10, 11, 25, 36, 59, 125, 250, 333, 999, 1001, 123456, 999999999, 1000000000001, 9999999999999};
	uint64_t denominator = 1;

	for (int precision = 1; precision <= 13; precision++) {
		denominator *= 10;

		for (size_t unit = 0; unit < sizeof(scales) / sizeof(scales[0]); unit++) {
			for (size_t sample = 0; sample < sizeof(numerators) / sizeof(numerators[0]); sample++) {
				uint64_t numerator = numerators[sample];

				if (numerator >= denominator) {
					continue;
				}

				wide_unsigned product = (wide_unsigned)numerator * scales[unit];
				bool is_integral = product % denominator == 0;
				int64_t expected = (int64_t)(scales[unit] + (uint64_t)(product / denominator));
				char digits[16];
				int digit_count = snprintf(digits, sizeof(digits), "%0*" PRIu64, precision, numerator);
				CHECK(digit_count == precision);

				for (unsigned variant = 0; variant < 4; variant++) {
					char fraction[32];
					size_t length = 0;

					for (int digit = 0; digit < precision; digit++) {
						if ((variant & 1) != 0 && digit > 0) {
							fraction[length++] = '_';
						}

						fraction[length++] = digits[digit];
					}

					fraction[length] = '\0';
					char input[64];
					int input_length = snprintf(input, sizeof(input), "[%s1.%s%s]", variant >= 2 ? "-" : "", fraction, units[unit]);
					soml_error error;
					soml_document *document = soml_parse(input, (size_t)input_length, nullptr, &error);
					CHECK((document != nullptr) == is_integral);

					if (document != nullptr) {
						int64_t actual = 0;
						CHECK(soml_get_duration(soml_array_at(soml_document_root(document), 0), &actual));
						CHECK(actual == (variant >= 2 ? -expected : expected));
					} else {
						CHECK(error.code == SOML_ERROR_SYNTAX && strstr(error.message, "whole number of nanoseconds") != nullptr);
					}

					soml_document_free(document);
				}
			}
		}
	}
}

/*
Count calendar days from 0001 independently of the civil-date algorithm, then test offset normalization, leap centuries, negative epoch seconds, and canonical UTC output.
*/
static void test_calendar_model(void) {
	static constexpr int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	static constexpr int offsets[] = {-1439, -420, -1, 0, 1, 330, 1439};
	int64_t days = -719162;

	for (int year = 1; year <= 9999; year++) {
		bool is_leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
		bool is_sample = year == 1 || year == 9999 || year % 97 == 0 || year % 100 == 0 || year == 1969 || year == 1970 || year == 2000;

		for (int month = 1; month <= 12; month++) {
			int count = month_days[month - 1] + (month == 2 && is_leap);

			if (is_sample) {
				for (int day = 1; day <= count; day += count - 1) {
					for (size_t offset = 0; offset < sizeof(offsets) / sizeof(offsets[0]); offset++) {
						int minutes = offsets[offset];
						int absolute = minutes < 0 ? -minutes : minutes;
						int64_t seconds = (days + day - 1) * 86400 + 43200 - minutes * 60;
						char input[64];
						int length = snprintf(input, sizeof(input), "[%04d-%02d-%02dT12:00:00.000000001%c%02d:%02d]", year, month, day, minutes < 0 ? '-' : '+', absolute / 60, absolute % 60);
						soml_error error;
						soml_document *document = soml_parse(input, (size_t)length, nullptr, &error);
						bool in_range = seconds >= -62135596800 && seconds <= 253402300799;
						CHECK((document != nullptr) == in_range);

						if (document != nullptr) {
							soml_instant instant = {};
							CHECK(soml_get_instant(soml_array_at(soml_document_root(document), 0), &instant));
							CHECK(instant.seconds == seconds && instant.nanoseconds == 1);
							soml_string output;
							CHECK(soml_write_canonical(document, &output, nullptr));
							soml_document *again = soml_parse(output.data, output.length, nullptr, nullptr);
							soml_instant round_trip = {};
							CHECK(soml_get_instant(soml_array_at(soml_document_root(again), 0), &round_trip));
							CHECK(round_trip.seconds == seconds && round_trip.nanoseconds == 1);
							soml_document_free(again);
						} else {
							CHECK(error.code == SOML_ERROR_SYNTAX && strstr(error.message, "in UTC") != nullptr);
						}

						soml_document_free(document);
					}
				}
			}

			days += count;
		}
	}
}

/*
A stored write needs only its collected output allocation, keeps arena spare space, and protects both sides of its retained buffer under ASan.
*/
static void test_retained_output_allocation_budget(void) {
	static constexpr size_t lengths[] = {0, 1, 4063, 4064, 4095, 4096, 4097, 8191, 8192, 16383};

	for (size_t sample = 0; sample < sizeof(lengths) / sizeof(lengths[0]); sample++) {
		size_t length = lengths[sample];
		char *bytes = malloc(length == 0 ? 1 : length);
		memset(bytes, 'x', length);
		fuzz_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
		soml_document *document = soml_document_create(&allocator);
		soml_value *root = soml_new_array(document);
		CHECK(soml_array_append(document, root, soml_new_string(document, bytes, length)));
		CHECK(soml_document_set_root(document, root));
		free(bytes);
		soml__chunk *arena = document->chunks;
		size_t arena_used = document->chunk_used;
		size_t allocations = state.allocations;
		soml_string output;
		CHECK(soml_write(document, &output, nullptr));
		CHECK(document->chunks == arena && document->chunk_used == arena_used);
		CHECK(state.allocations - allocations <= 3);
		CHECK(output.length == length + 8 && output.data[output.length] == '\0');
		CHECK(memcmp(output.data, "[\n\t'", 4) == 0 && memcmp(output.data + output.length - 4, "'\n]\n", 4) == 0);
		CHECK(arena->next != nullptr);
		#ifdef HAS_ADDRESS_SANITIZER
		CHECK(__asan_address_is_poisoned(output.data - 1));
		CHECK(!__asan_address_is_poisoned(output.data + output.length));
		CHECK(__asan_address_is_poisoned(output.data + output.length + 1));
		#endif
		// A short sorted output has exactly one allocation, including its terminator and ownership transfer.
		if (length == 0) {
			CHECK(state.allocations - allocations == 1);
		}

		soml_value *later = soml_new_int(document, 42);
		CHECK(later != nullptr && soml_array_append(document, root, later));
		CHECK(output.data[output.length] == '\0' && output.data[3] == '\'');
		soml_document_free(document);
		CHECK(state.live_blocks == 0);
	}
}

static int compare_sort_model(const void *context, size_t left, size_t right) {
	const unsigned char *keys = context;
	return (keys[left] > keys[right]) - (keys[left] < keys[right]);
}

/*
Check merge stability against independently ranked indexes across odd run lengths, duplicate groups, sorted runs, reverse order, and interleaved keys.
*/
static void test_merge_sort_model(void) {
	for (size_t count = 0; count <= 129; count++) {
		for (unsigned shape = 0; shape < 5; shape++) {
			unsigned char keys[129];
			size_t items[129];
			size_t temporary[129];

			for (size_t index = 0; index < count; index++) {
				items[index] = index;
				keys[index] = (unsigned char)(shape == 0 ? index : shape == 1 ? count - index : shape == 2 ? index / 7 : shape == 3 ? (index * 37) % 17 : (index / 8) * 8 + 7 - index % 8);
			}

			soml__merge_sort(items, temporary, count, compare_sort_model, keys);

			for (size_t index = 0; index < count; index++) {
				size_t rank = 0;

				for (size_t other = 0; other < count; other++) {
					rank += keys[other] < keys[index] || (keys[other] == keys[index] && other < index);
				}

				CHECK(items[rank] == index);
			}
		}
	}
}

/*
Invalid quote endings are grammar failures even when the allocator would refuse an otherwise unnecessary scratch block.
*/
static void test_unterminated_string_allocation_budget(void) {
	static const char *const inputs[] = {"[\"plain", "[\"plain\n", "['plain", "['plain\n"};

	for (size_t index = 0; index < sizeof(inputs) / sizeof(inputs[0]); index++) {
		fuzz_allocator state = {.fail_at = 2};
		soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
		soml_error error;
		soml_document *document = soml_parse(inputs[index], strlen(inputs[index]), &allocator, &error);
		CHECK(document == nullptr && error.code == SOML_ERROR_SYNTAX && strstr(error.message, "Unterminated string") != nullptr);
		CHECK(state.allocations == 2 && state.live_blocks == 0);
	}
}

/*
Exercise all overlapping comment-delimiter strings up to ten bytes against a simple two-pass model, including a nested opener in an unterminated comment.
*/
static void test_comment_delimiter_model(void) {
	for (unsigned length = 0; length <= 10; length++) {
		for (unsigned pattern = 0; pattern < (1u << length); pattern++) {
			unsigned char input[14] = {'/', '*'};

			for (unsigned index = 0; index < length; index++) {
				input[index + 2] = (pattern & (1u << index)) != 0 ? '*' : '/';
			}

			size_t size = length + 2;
			size_t closing = size;
			size_t nested = size;

			for (size_t index = 2; index + 1 < size; index++) {
				if (input[index] == '*' && input[index + 1] == '/') {
					closing = index;
					break;
				}
			}

			for (size_t index = 2; index + 2 <= closing; index++) {
				if (input[index] == '/' && input[index + 1] == '*') {
					nested = index;
					break;
				}
			}

			soml__parser parser = {.text = input, .length = size};
			bool success = soml__skip_block_comment(&parser);
			CHECK(success == (closing < size && nested == size));

			if (success) {
				CHECK(parser.position == closing + 2);
			} else {
				CHECK(parser.error.offset == (closing == size ? 0 : nested));
				CHECK(strstr(parser.error.message, closing == size ? "Unterminated" : "nested") != nullptr);
			}
		}
	}
}

/*
The word classifier must agree with independent byte ranges at every byte position, including high-bit bytes that must be rejected before unsigned word subtraction.
*/
static void test_bare_key_word_model(void) {
	for (size_t position = 0; position < 8; position++) {
		for (unsigned byte = 0; byte <= 255; byte++) {
			unsigned char bytes[] = {'A', 'z', '0', '9', '_', '-', 'x', 'Y'};
			bytes[position] = (unsigned char)byte;
			uint64_t word;
			memcpy(&word, bytes, sizeof(word));
			bool expected = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_' || byte == '-';
			CHECK(soml__is_bare_key_word(word) == expected);
			CHECK(soml__skip_bare_key(bytes, sizeof(bytes), 0) == (expected ? sizeof(bytes) : position));
		}
	}
}

/*
The small float formatter uses wide integer intervals; compare its results with the independent high-precision decimal algorithm across each supported binary exponent and both sides of mantissa boundaries.
*/
static void test_small_float_formatting_model(void) {
	static constexpr uint64_t mantissas[] = {0, 1, 2, 3, 7, 15, 0x123456789ABCD, 0x5555555555555, 0xAAAAAAAAAAAAA, 0xFFFFFFFFFFFFB, 0xFFFFFFFFFFFFC, 0xFFFFFFFFFFFFD, 0xFFFFFFFFFFFFE, 0xFFFFFFFFFFFFF};
	size_t fast_count = 0;

	for (int exponent = -70; exponent <= 2; exponent++) {
		for (size_t sample = 0; sample < sizeof(mantissas) / sizeof(mantissas[0]); sample++) {
			uint64_t bits = (uint64_t)(exponent + 1023 + 52) << 52 | mantissas[sample];
			soml__decimal fast;

			if (soml__decimal_shortest_small(&fast, bits)) {
				fast_count++;
				CHECK(exponent >= -63 && exponent < 0);
				soml__decimal reference;
				soml__decimal_shortest(&reference, bits);
				CHECK(fast.count == reference.count && fast.point == reference.point && fast.truncated == reference.truncated);
				CHECK(memcmp(fast.digits, reference.digits, (size_t)reference.count) == 0);
			}
		}
	}

	CHECK(fast_count > 800);
}

/*
A source made mostly of comments should need only a small first arena, and retaining an error position should still count Unicode scalars correctly on the final line.
*/
static void test_comment_heavy_arena_budget(void) {
	constexpr size_t length = 200000;
	char *text = malloc(length);
	text[0] = '#';
	memset(text + 1, 'x', length - 1);
	memcpy(text + length - 7, "\na: 1\n", 6);
	text[length - 1] = ' ';
	fuzz_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
	soml_document *document = soml_parse(text, length, &allocator, nullptr);
	CHECK(document != nullptr && soml_count(soml_document_root(document)) == 1);
	CHECK(document != nullptr && document->chunks->size == 4096 && document->chunks->next == nullptr);
	soml_document_free(document);
	CHECK(state.live_blocks == 0);
	memcpy(text + length - 7, "\né?", 4);
	soml_error error;
	document = soml_parse(text, length - 3, nullptr, &error);
	CHECK(document == nullptr && error.line == 2 && error.column == 1);
	free(text);
}

static void test_document_inspection(void) {
	counting_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
	soml_document *document = soml_document_create(&allocator);
	soml_error error = {.code = SOML_ERROR_WRITE, .message = "unchanged", .offset = 17, .line = 3, .column = 4};
	CHECK(soml_document_allocated_bytes(nullptr) == 0);
	CHECK(soml_document_allocated_bytes(document) == state.live_bytes);
	CHECK(!soml_document_get_error(document, &error));
	CHECK(!soml_document_get_error(nullptr, &error));
	CHECK(!soml_document_get_error(document, nullptr));
	CHECK(error.code == SOML_ERROR_WRITE && strcmp(error.message, "unchanged") == 0 && error.offset == 17 && error.line == 3 && error.column == 4);
	soml_value *root = soml_new_object(document);
	CHECK(soml_document_set_root(document, root));
	CHECK(soml_object_add(document, root, "z", 1, soml_new_int(document, 1)));
	CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 2)));
	CHECK(soml_object_reserve(document, root, 8192));
	CHECK(soml_document_allocated_bytes(document) == state.live_bytes);
	size_t before = soml_document_allocated_bytes(document);
	CHECK(soml_write_canonical_with_callback(document, write_text_ignoring, nullptr, &error));
	CHECK(soml_document_allocated_bytes(document) == before && state.live_bytes == before);
	soml_string output;
	CHECK(soml_write_canonical(document, &output, &error));
	CHECK(soml_document_allocated_bytes(document) == state.live_bytes && state.live_bytes > before);
	CHECK(!soml_document_get_error(document, &error));
	CHECK(!soml_object_set(document, root, nullptr, 1, soml_new_null(document)));
	CHECK(soml_document_get_error(document, &error));
	CHECK(error.code == SOML_ERROR_INVALID_ARGUMENT && error.offset == 0 && error.line == 0 && error.column == 0);
	const char *first_message = error.message;
	CHECK(!soml_array_reserve(document, root, SIZE_MAX));
	CHECK(soml_document_get_error(document, &error) && error.message == first_message);
	soml_document_free(document);
	CHECK(strcmp(first_message, "A key is nullptr, but its length is not 0") == 0);
	CHECK(state.live_blocks == 0 && state.live_bytes == 0);

	document = soml_document_create(&allocator);
	CHECK(soml_document_set_root(document, soml_new_float(document, 1.0)));
	CHECK(!soml_write(document, &output, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	CHECK(!soml_document_get_error(document, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	soml_document_free(document);
	CHECK(state.live_bytes == 0 && state.live_blocks == 0);
	const char input[] = "z: [1, 2]\na: 'value'\n";
	document = soml_parse(input, sizeof(input) - 1, &allocator, nullptr);
	CHECK(document != nullptr && soml_document_allocated_bytes(document) == state.live_bytes);
	soml_document_free(document);
	CHECK(state.live_bytes == 0 && state.live_blocks == 0);
}

static void test_collection_views_and_reserve(void) {
	fuzz_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
	soml_document *document = soml_document_create(&allocator);
	soml_value *array = soml_new_array(document);
	soml_value *object = soml_new_object(document);
	soml_value *scalar = soml_new_null(document);
	const soml_value *wrong_arrays[] = {nullptr, object, scalar};
	const soml_value *wrong_objects[] = {nullptr, array, scalar};

	for (size_t index = 0; index < sizeof(wrong_arrays) / sizeof(wrong_arrays[0]); index++) {
		soml_array_view items = soml_array_items(wrong_arrays[index]);
		soml_object_view members = soml_object_members(wrong_objects[index]);
		CHECK(items.data == nullptr && items.count == 0 && members.data == nullptr && members.count == 0);
	}

	CHECK(soml_array_items(array).count == 0 && soml_object_members(object).count == 0);
	CHECK(soml_array_reserve(document, array, 0));
	CHECK(soml_object_reserve(document, object, 0));
	CHECK(soml_array_reserve(document, array, 64));
	CHECK(soml_object_reserve(document, object, 64));
	soml_value *values[32];

	for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
		values[index] = soml_new_int(document, (int64_t)index);
	}

	size_t allocations = state.allocations;
	CHECK(soml_array_reserve(document, array, 1));
	CHECK(soml_object_reserve(document, object, 1));
	CHECK(state.allocations == allocations);

	for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
		CHECK(soml_array_append(document, array, values[index]));
	}

	CHECK(state.allocations == allocations);
	soml_array_view items = soml_array_items(array);
	CHECK(items.count == 32);

	for (size_t index = 0; index < items.count; index++) {
		CHECK(items.data[index] == values[index] && items.data[index] == soml_array_at(array, index));
	}

	CHECK(soml_array_reserve(document, array, 8192));
	items = soml_array_items(array);
	CHECK(items.count == 32 && items.data[0] == values[0] && items.data[31] == values[31]);
	CHECK(soml_object_set(document, object, "a\0b", 3, scalar));
	soml_object_view members = soml_object_members(object);
	CHECK(members.count == 1 && members.data[0].value == scalar && members.data[0].key.length == 3 && memcmp(members.data[0].key.data, "a\0b", 3) == 0);
	CHECK(soml_object_reserve(document, object, 8192));
	members = soml_object_members(object);
	CHECK(members.count == 1 && members.data[0].value == scalar && members.data[0].key.length == 3);
	soml_document_free(document);
	CHECK(state.live_blocks == 0);
}

static void test_array_editing_ownership(void) {
	soml_document *document = parse_text("[1, 2, 3]", nullptr);
	soml_value *array = soml_document_root(document);
	soml_value *first = soml_array_at(array, 0);
	soml_value *second = soml_array_at(array, 1);
	soml_value *last = soml_array_at(array, 2);
	soml_value *zero = soml_new_int(document, 0);
	soml_value *middle = soml_new_int(document, 7);
	soml_value *end = soml_new_int(document, 8);
	CHECK(soml_array_insert(document, array, 0, zero));
	CHECK(soml_array_insert(document, array, 2, middle));
	CHECK(soml_array_insert(document, array, soml_count(array), end));
	soml_array_view view = soml_array_items(array);
	CHECK(view.count == 6 && view.data[0] == zero && view.data[1] == first && view.data[2] == middle && view.data[3] == second && view.data[4] == last && view.data[5] == end);
	CHECK(soml_array_set(document, array, 3, second));
	soml_value *replacement = soml_new_int(document, 9);
	CHECK(soml_array_set(document, array, 3, replacement));
	CHECK(soml_array_append(document, array, second));
	CHECK(soml_array_remove(document, array, 0));
	CHECK(soml_array_append(document, array, zero));
	CHECK(soml_array_pop(document, array) == zero);
	soml_value *holder = soml_new_array(document);
	CHECK(soml_array_append(document, holder, zero));
	CHECK(soml_array_remove(document, array, 1));
	CHECK(soml_array_append(document, holder, middle));
	soml_string output;
	CHECK(soml_write_canonical(document, &output, nullptr));
	CHECK(string_equals(output, "[\n\t1\n\t9\n\t3\n\t8\n\t2\n]\n"));
	CHECK(soml_array_clear(document, array));
	CHECK(soml_count(array) == 0 && soml_array_items(array).count == 0);
	CHECK(soml_array_pop(document, array) == nullptr);
	soml_error error;
	CHECK(!soml_document_get_error(document, &error));
	CHECK(soml_array_append(document, holder, first));
	CHECK(soml_array_append(document, holder, replacement));
	CHECK(soml_array_append(document, holder, last));
	CHECK(soml_array_append(document, holder, end));
	CHECK(soml_array_append(document, holder, second));
	CHECK(soml_array_clear(document, array));
	CHECK(soml_array_append(document, array, soml_new_bool(document, true)));
	view = soml_array_items(array);
	CHECK(view.count == 1 && soml_type_of(view.data[0]) == SOML_TYPE_BOOL);
	CHECK(soml_write_canonical(document, &output, nullptr) && string_equals(output, "[\n\ttrue\n]\n"));
	soml_document_free(document);
}

static void test_object_editing_ownership(void) {
	soml_document *document = parse_text("z: 1\n\"a\\u{0}\": 2\na: 3\n", nullptr);
	soml_value *object = soml_document_root(document);
	soml_value *old = soml_object_get_with_length(object, "a\0", 2);
	soml_value *replacement = soml_new_int(document, 4);
	CHECK(soml_object_set(document, object, "a\0", 2, old));
	CHECK(soml_object_set(document, object, "a\0", 2, replacement));
	soml_object_view members = soml_object_members(object);
	CHECK(members.count == 3 && members.data[1].value == replacement && members.data[1].key.length == 2 && memcmp(members.data[1].key.data, "a\0", 2) == 0);
	CHECK(soml_object_set(document, object, "old", 3, old));
	soml_value *taken = soml_object_take(document, object, "a", 1);
	CHECK(taken != nullptr && soml_object_get(object, "a") == nullptr);
	CHECK(soml_object_set(document, object, nullptr, 0, taken));
	CHECK(soml_object_take(document, object, "missing", 7) == nullptr);
	CHECK(soml_object_remove(document, object, "missing", 7));
	soml_error error;
	CHECK(!soml_document_get_error(document, &error));
	soml_value *removed = soml_object_get(object, "z");
	CHECK(soml_object_remove(document, object, "z", 1));
	CHECK(soml_object_set(document, object, "last", 4, removed));
	members = soml_object_members(object);
	CHECK(members.count == 4 && members.data[0].value == replacement && string_equals(members.data[1].key, "old") && members.data[2].key.length == 0 && string_equals(members.data[3].key, "last"));
	soml_string output;
	CHECK(soml_write_canonical(document, &output, nullptr));
	CHECK(string_equals(output, "'': 3\n\"a\\u{0}\": 4\nlast: 1\nold: 2\n"));
	CHECK(soml_object_clear(document, object));
	CHECK(soml_object_members(object).count == 0 && soml_count(object) == 0);
	CHECK(soml_object_take(document, object, nullptr, 0) == nullptr);
	CHECK(soml_object_set(document, object, "a", 1, taken));
	CHECK(soml_object_set(document, object, "b", 1, removed));
	CHECK(soml_object_set(document, object, "c", 1, old));
	CHECK(soml_object_set(document, object, "d", 1, replacement));
	CHECK(soml_write_canonical(document, &output, nullptr) && string_equals(output, "a: 3\nb: 1\nc: 2\nd: 4\n"));
	soml_document_free(document);
}

static void test_parsed_object_index_after_removals(void) {
	static const soml_string keys[] = {{"z", 1}, {"a\0", 2}, {"a", 1}, {"m", 1}};

	for (unsigned permutation = 0; permutation < 24; permutation++) {
		soml_document *document = parse_text("z: 30\n\"a\\u{0}\": 10\na: 20\nm: 40\n", nullptr);
		soml_value *object = soml_document_root(document);

		if (!CHECK(object != nullptr)) {
			soml_document_free(document);
			continue;
		}

		soml_value *values[4];
		bool removed[4] = {};
		unsigned selection = permutation;
		CHECK(object->is_sorted && object->as.object.sorted != nullptr);
		CHECK(soml_object_reserve(document, object, 8192));
		CHECK(object->is_sorted && object->as.object.sorted != nullptr);

		for (size_t index = 0; index < 4; index++) {
			values[index] = soml_object_get_with_length(object, keys[index].data, keys[index].length);
			CHECK(values[index] != nullptr);
		}

		for (size_t remaining = 4; remaining > 0; remaining--) {
			unsigned choice = selection % (unsigned)remaining;
			selection /= (unsigned)remaining;
			size_t chosen = 0;

			for (size_t index = 0; index < 4; index++) {
				if (!removed[index]) {
					if (choice == 0) {
						chosen = index;
						break;
					}

					choice--;
				}
			}

			CHECK(soml_object_take(document, object, keys[chosen].data, keys[chosen].length) == values[chosen]);
			removed[chosen] = true;
			soml_object_view members = soml_object_members(object);
			CHECK(members.count == remaining - 1 && object->is_sorted);
			size_t position = 0;

			for (size_t index = 0; index < 4; index++) {
				CHECK(soml_object_get_with_length(object, keys[index].data, keys[index].length) == (removed[index] ? nullptr : values[index]));

				if (!removed[index]) {
					CHECK(members.data[position].value == values[index]);
					CHECK(members.data[position].key.length == keys[index].length && memcmp(members.data[position].key.data, keys[index].data, keys[index].length) == 0);
					position++;
				}
			}

			soml_string output;
			CHECK(soml_write_canonical(document, &output, nullptr));
			soml_document *again = soml_parse(output.data, output.length, nullptr, nullptr);
			CHECK(again != nullptr && soml_count(soml_document_root(again)) == remaining - 1);
			soml_document_free(again);
		}

		soml_document_free(document);
	}
}

static void test_collection_reserve_failure(void) {
	for (unsigned kind = 0; kind < 2; kind++) {
		for (unsigned failure = 0; failure < 2; failure++) {
			counting_allocator state = {.fail_at = SIZE_MAX};
			soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
			const char *text = kind == 0 ? "[1, 2]" : "z: 1\na: 2\n";
			soml_document *document = soml_parse(text, strlen(text), &allocator, nullptr);
			soml_value *root = soml_document_root(document);
			soml_value *first = kind == 0 ? soml_array_at(root, 0) : soml_object_value_at(root, 0);
			soml_value *second = kind == 0 ? soml_array_at(root, 1) : soml_object_value_at(root, 1);
			size_t bytes = state.live_bytes;
			size_t allocations = state.allocations;
			size_t capacity = failure == 0 ? SIZE_MAX : 8192;
			state.fail_at = state.allocations;
			CHECK(!(kind == 0 ? soml_array_reserve(document, root, capacity) : soml_object_reserve(document, root, capacity)));
			CHECK(state.live_bytes == bytes && soml_document_allocated_bytes(document) == bytes && soml_count(root) == 2);
			CHECK((kind == 0 ? soml_array_at(root, 0) : soml_object_value_at(root, 0)) == first);
			CHECK((kind == 0 ? soml_array_at(root, 1) : soml_object_value_at(root, 1)) == second);
			CHECK(failure != 0 || state.allocations == allocations);

			if (kind == 1) {
				CHECK(soml_object_get(root, "z") == first && soml_object_get(root, "a") == second && root->is_sorted);
			}

			soml_error error;
			CHECK(soml_document_get_error(document, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY);
			state.fail_at = SIZE_MAX;
			CHECK(kind == 0 ? soml_array_reserve(document, root, 8192) : soml_object_reserve(document, root, 8192));
			CHECK(soml_count(root) == 2 && soml_document_allocated_bytes(document) == state.live_bytes);
			CHECK(soml_document_get_error(document, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY);
			soml_document_free(document);
			CHECK(state.live_blocks == 0 && state.live_bytes == 0);
		}
	}
}

static void test_collection_edit_failure_atomicity(void) {
	for (unsigned operation = 0; operation < 6; operation++) {
		soml_document *document = parse_text("[1, 2]", nullptr);
		soml_value *array = soml_document_root(document);
		soml_value *first = soml_array_at(array, 0);
		soml_value *second = soml_array_at(array, 1);
		soml_value *candidate = soml_new_int(document, 3);
		soml_document *foreign = soml_document_create(nullptr);
		soml_value *other = soml_new_int(foreign, 4);
		bool changed;

		switch (operation) {
			case 0: {
				changed = soml_array_insert(document, array, SIZE_MAX, candidate);
				break;
			}
			case 1: {
				changed = soml_array_set(document, array, 2, candidate);
				break;
			}
			case 2: {
				changed = soml_array_remove(document, array, SIZE_MAX);
				break;
			}
			case 3: {
				changed = soml_array_set(document, array, 0, second);
				break;
			}
			case 4: {
				changed = soml_array_set(document, array, 0, other);
				break;
			}
			default: {
				changed = soml_array_set(document, array, 0, nullptr);
				break;
			}
		}

		CHECK(!changed && soml_count(array) == 2 && soml_array_at(array, 0) == first && soml_array_at(array, 1) == second);
		CHECK(soml_array_append(document, array, candidate));
		CHECK(soml_array_append(foreign, soml_new_array(foreign), other));
		CHECK(!soml_array_append(document, soml_new_array(document), first));
		soml_error error;
		CHECK(soml_document_get_error(document, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
		soml_document_free(foreign);
		soml_document_free(document);
	}

	for (unsigned operation = 0; operation < 4; operation++) {
		soml_document *document = parse_text("z: 1\na: 2\n", nullptr);
		soml_value *object = soml_document_root(document);
		soml_value *first = soml_object_get(object, "z");
		soml_value *second = soml_object_get(object, "a");
		soml_value *candidate = soml_new_int(document, 3);
		bool changed = operation == 0 ? soml_object_set(document, object, "z", 1, second) : operation == 1 ? soml_object_set(document, object, "z", 1, nullptr) : operation == 2 ? soml_object_remove(document, object, nullptr, 1) : soml_object_take(document, object, nullptr, 1) != nullptr;
		CHECK(!changed && soml_count(object) == 2 && soml_object_get(object, "z") == first && soml_object_get(object, "a") == second);
		CHECK(soml_object_set(document, object, "candidate", 9, candidate));
		CHECK(!soml_array_append(document, soml_new_array(document), first));
		soml_document_free(document);
	}
}

static void test_array_insert_allocation_failure(void) {
	counting_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
	soml_document *document = soml_parse("[1, 2]", 6, &allocator, nullptr);
	soml_value *array = soml_document_root(document);
	soml_value *first = soml_array_at(array, 0);
	soml_value *second = soml_array_at(array, 1);
	soml_value *sink = soml_new_object(document);
	CHECK(soml_object_reserve(document, sink, 4));
	soml_value *payload = soml_new_null(document);
	soml_value *candidate = soml_new_int(document, 3);
	// Fill the remaining arena through a copied key so the insertion must request a real allocation.
	size_t remaining = document->chunks->size - document->chunk_used;
	#ifdef HAS_ADDRESS_SANITIZER
	size_t key_length = remaining - 17;
	#else
	size_t key_length = remaining - 1;
	#endif
	char *key = malloc(key_length);
	memset(key, 'x', key_length);
	CHECK(soml_object_add(document, sink, key, key_length, payload));
	free(key);
	CHECK(document->chunk_used == document->chunks->size);
	size_t bytes = state.live_bytes;
	state.fail_at = state.allocations;
	CHECK(!soml_array_insert(document, array, 1, candidate));
	CHECK(soml_count(array) == 2 && soml_array_at(array, 0) == first && soml_array_at(array, 1) == second);
	CHECK(state.live_bytes == bytes && soml_document_allocated_bytes(document) == bytes);
	state.fail_at = SIZE_MAX;
	CHECK(soml_object_set(document, sink, "candidate", 9, candidate));
	soml_error error;
	CHECK(soml_document_get_error(document, &error) && error.code == SOML_ERROR_OUT_OF_MEMORY);
	soml_document_free(document);
	CHECK(state.live_blocks == 0 && state.live_bytes == 0);
}

static void test_object_duplicate_editing_and_foreign_values(void) {
	soml_document *document = soml_document_create(nullptr);
	soml_value *object = soml_new_object(document);
	soml_value *first = soml_new_int(document, 1);
	soml_value *second = soml_new_int(document, 2);
	soml_value *replacement = soml_new_int(document, 3);
	CHECK(soml_object_add(document, object, "a", 1, first));
	CHECK(soml_object_add(document, object, "a", 1, second));
	CHECK(soml_object_set(document, object, "a", 1, replacement));
	CHECK(soml_object_value_at(object, 0) == replacement && soml_object_value_at(object, 1) == second);
	CHECK(soml_object_take(document, object, "a", 1) == replacement);
	CHECK(soml_object_get(object, "a") == second && soml_count(object) == 1);
	CHECK(soml_object_remove(document, object, "a", 1));
	CHECK(soml_object_set(document, object, "first", 5, first));
	CHECK(soml_object_set(document, object, "second", 6, second));
	CHECK(soml_object_set(document, object, "third", 5, replacement));
	CHECK(soml_document_set_root(document, object));
	soml_string output;
	CHECK(soml_write_canonical(document, &output, nullptr) && string_equals(output, "first: 1\nsecond: 2\nthird: 3\n"));
	soml_document *foreign = soml_document_create(nullptr);
	soml_value *foreign_object = soml_new_object(foreign);
	soml_value *foreign_value = soml_new_int(foreign, 4);
	CHECK(!soml_object_set(document, object, "first", 5, foreign_value));
	CHECK(soml_object_get(object, "first") == first && soml_count(object) == 3);
	CHECK(soml_object_set(foreign, foreign_object, "value", 5, foreign_value));
	CHECK(!soml_object_clear(document, foreign_object));
	CHECK(!soml_object_reserve(document, foreign_object, 8192));
	CHECK(soml_object_take(document, foreign_object, "value", 5) == nullptr);
	CHECK(!soml_object_remove(document, foreign_object, "value", 5));
	CHECK(soml_count(foreign_object) == 1 && soml_object_get(foreign_object, "value") == foreign_value);
	soml_error error;
	CHECK(!soml_document_get_error(foreign, &error));
	CHECK(soml_object_take(nullptr, object, "first", 5) == nullptr);
	CHECK(!soml_object_remove(document, nullptr, "first", 5));
	CHECK(!soml_object_set(document, first, "first", 5, soml_new_null(document)));
	soml_document_free(foreign);
	soml_document_free(document);

	document = parse_text("[1]", nullptr);
	soml_value *array = soml_document_root(document);
	first = soml_array_at(array, 0);
	foreign = soml_document_create(nullptr);
	soml_value *foreign_array = soml_new_array(foreign);
	foreign_value = soml_new_int(foreign, 2);
	CHECK(soml_array_append(foreign, foreign_array, foreign_value));
	CHECK(!soml_array_insert(document, array, 0, foreign_value));
	CHECK(!soml_array_clear(document, foreign_array));
	CHECK(!soml_array_reserve(document, foreign_array, 8192));
	CHECK(!soml_array_remove(document, foreign_array, 0));
	CHECK(soml_array_pop(document, foreign_array) == nullptr);
	CHECK(soml_count(array) == 1 && soml_array_at(array, 0) == first && soml_count(foreign_array) == 1 && soml_array_at(foreign_array, 0) == foreign_value);
	CHECK(!soml_document_get_error(foreign, &error));
	CHECK(soml_array_pop(nullptr, array) == nullptr);
	CHECK(!soml_array_clear(document, nullptr));
	CHECK(!soml_array_set(document, first, 0, soml_new_null(document)));
	soml_document_free(foreign);
	soml_document_free(document);
}

// This 9 KB input exercises callback pieces on both sides of the 4 KB writer boundaries.
static void test_caller_buffer_lengths_and_guards(void) {
	static const char *const inputs[] = {
		"{}",
		"[]",
		"z: [true, null, -9223372036854775808, infinity, -infinity, 2026-10-13T12:00:00.001Z, -1h2m3.000000004s]\n\"a\\u{0}\": \"quote'\\n\\t\\u{0}é😀\"\na: {z: 1.0, a: 2}\n"
	};

	for (size_t sample = 0; sample <= sizeof(inputs) / sizeof(inputs[0]); sample++) {
		counting_allocator state = {.fail_at = SIZE_MAX};
		soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
		soml_document *document;

		if (sample < sizeof(inputs) / sizeof(inputs[0])) {
			document = soml_parse(inputs[sample], strlen(inputs[sample]), &allocator, nullptr);
		} else {
			document = soml_document_create(&allocator);
			soml_value *root = soml_new_object(document);
			char bytes[9000];
			memset(bytes, 'x', sizeof(bytes));
			bytes[4095] = '\n';
			bytes[8191] = '\'';
			CHECK(soml_object_add(document, root, "z", 1, soml_new_string(document, bytes, sizeof(bytes))));
			CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 5)));
			CHECK(soml_document_set_root(document, root));
		}

		CHECK(document != nullptr);
		soml_string expected[2];
		CHECK(soml_write(document, &expected[0], nullptr));
		CHECK(soml_write_canonical(document, &expected[1], nullptr));
		CHECK(expected[0].length == expected[1].length);
		size_t retained = state.live_bytes;
		size_t measured = SIZE_MAX;
		soml_error error = {.code = SOML_ERROR_WRITE};
		CHECK(soml_validate(document, &error) && error.code == SOML_ERROR_NONE && error.message == nullptr);
		CHECK(soml_measure(document, &measured, &error) && measured == expected[0].length && error.code == SOML_ERROR_NONE);
		CHECK(soml_document_allocated_bytes(document) == retained && state.live_bytes == retained);

		for (unsigned canonical_order = 0; canonical_order < 2; canonical_order++) {
			const size_t capacities[] = {0, 1, measured, measured + 1, measured + 17};

			for (size_t choice = 0; choice < sizeof(capacities) / sizeof(capacities[0]); choice++) {
				size_t capacity = capacities[choice];
				unsigned char *allocation = malloc(capacity + 16);
				memset(allocation, 0xDA, capacity + 16);
				char *buffer = (char *)allocation + 8;
				size_t length = SIZE_MAX;
				bool success = canonical_order == 0 ? soml_write_to_buffer(document, buffer, capacity, &length, &error) : soml_write_canonical_to_buffer(document, buffer, capacity, &length, &error);
				CHECK(success == (capacity > measured));

				if (success) {
					CHECK(length == measured && memcmp(buffer, expected[canonical_order].data, measured) == 0 && buffer[length] == '\0');
					CHECK(error.code == SOML_ERROR_NONE && error.message == nullptr);

					for (size_t index = measured + 1; index < capacity; index++) {
						CHECK((unsigned char)buffer[index] == 0xDA);
					}
				} else {
					CHECK(error.code == SOML_ERROR_WRITE && length == SIZE_MAX);
					// A rejected write may publish complete callback pieces, but only a correct prefix.
					size_t prefix = 0;

					while (prefix < capacity && (unsigned char)buffer[prefix] != 0xDA) {
						CHECK(prefix < measured && buffer[prefix] == expected[canonical_order].data[prefix]);
						prefix++;
					}

					for (size_t index = prefix; index < capacity; index++) {
						CHECK((unsigned char)buffer[index] == 0xDA);
					}
				}

				for (size_t index = 0; index < 8; index++) {
					CHECK(allocation[index] == 0xDA && allocation[capacity + 8 + index] == 0xDA);
				}

				CHECK(state.live_bytes == retained && soml_document_allocated_bytes(document) == retained);
				CHECK(!soml_document_get_error(document, &error));
				free(allocation);
			}
		}

		soml_document_free(document);
		CHECK(state.live_bytes == 0 && state.live_blocks == 0);
	}
}

static void test_validation_rejects_and_recovers(void) {
	static soml_value *(*const makers[])(soml_document *) = {make_nan, make_negative_nan, make_carriage_return, make_invalid_utf8, make_surrogate, make_early_instant, make_late_instant, make_bad_nanoseconds, make_negative_nanoseconds, make_duplicate_keys, make_duplicate_sorted_keys};

	for (size_t sample = 0; sample < sizeof(makers) / sizeof(makers[0]) + 2; sample++) {
		soml_document *document = soml_document_create(nullptr);
		soml_value *root = soml_new_array(document);
		soml_value *invalid;

		if (sample < sizeof(makers) / sizeof(makers[0])) {
			invalid = makers[sample](document);
		} else {
			invalid = soml_new_object(document);
			const char key[] = {sample == sizeof(makers) / sizeof(makers[0]) ? '\r' : (char)0xFF};
			CHECK(soml_object_add(document, invalid, key, sizeof(key), soml_new_null(document)));
		}

		CHECK(soml_array_append(document, root, invalid));
		CHECK(soml_document_set_root(document, root));
		soml_error error;
		size_t length = 73;
		char buffer[4096];
		CHECK(!soml_validate(document, &error) && error.code == SOML_ERROR_INVALID_VALUE);
		CHECK(!soml_measure(document, &length, &error) && error.code == SOML_ERROR_INVALID_VALUE && length == 73);
		CHECK(!soml_write_to_buffer(document, buffer, sizeof(buffer), &length, &error) && error.code == SOML_ERROR_INVALID_VALUE && length == 73);
		CHECK(!soml_write_canonical_to_buffer(document, buffer, sizeof(buffer), &length, &error) && error.code == SOML_ERROR_INVALID_VALUE && length == 73);
		CHECK(!soml_document_get_error(document, &error));
		CHECK(soml_array_set(document, root, 0, soml_new_int(document, 5)));
		CHECK(soml_validate(document, nullptr));
		CHECK(soml_measure(document, &length, nullptr) && length == 7);
		CHECK(soml_write_to_buffer(document, buffer, sizeof(buffer), &length, nullptr) && length == 7 && strcmp(buffer, "[\n\t5\n]\n") == 0);
		soml_document_free(document);
	}
}

static void test_validation_depth_and_root(void) {
	for (size_t depth = 100; depth <= 101; depth++) {
		soml_document *document = soml_document_create(nullptr);
		soml_value *root = soml_new_array(document);
		soml_value *parent = root;

		for (size_t level = 1; level < depth; level++) {
			soml_value *child = level % 2 == 0 ? soml_new_object(document) : soml_new_array(document);
			CHECK(soml_type_of(parent) == SOML_TYPE_ARRAY ? soml_array_append(document, parent, child) : soml_object_add(document, parent, "a", 1, child));
			parent = child;
		}

		CHECK(soml_type_of(parent) == SOML_TYPE_ARRAY ? soml_array_append(document, parent, soml_new_int(document, 1)) : soml_object_add(document, parent, "a", 1, soml_new_int(document, 1)));
		CHECK(soml_document_set_root(document, root));
		soml_error error;
		size_t length = SIZE_MAX;
		constexpr size_t capacity = 32768;
		char *buffer = malloc(capacity);

		if (!CHECK(buffer != nullptr)) {
			soml_document_free(document);
			continue;
		}

		bool valid = depth == 100;
		CHECK(soml_validate(document, &error) == valid && error.code == (valid ? SOML_ERROR_NONE : SOML_ERROR_TOO_DEEP));
		CHECK(soml_measure(document, &length, &error) == valid);
		CHECK(valid ? length < capacity : length == SIZE_MAX && error.code == SOML_ERROR_TOO_DEEP);
		CHECK(soml_write_to_buffer(document, buffer, capacity, &length, &error) == valid);
		CHECK(valid || (length == SIZE_MAX && error.code == SOML_ERROR_TOO_DEEP));
		CHECK(!soml_document_get_error(document, &error));
		free(buffer);
		soml_document_free(document);
	}

	soml_document *document = soml_document_create(nullptr);
	soml_error error;
	CHECK(!soml_validate(document, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	CHECK(soml_document_set_root(document, soml_new_int(document, 1)));
	CHECK(!soml_validate(document, &error) && error.code == SOML_ERROR_INVALID_VALUE);
	CHECK(soml_document_set_root(document, soml_new_object(document)));
	CHECK(soml_validate(document, &error) && error.code == SOML_ERROR_NONE);
	soml_document_free(document);
}

static void test_writer_api_invalid_arguments_and_builder_failure(void) {
	soml_document *document = parse_text("[]", nullptr);
	soml_error error;
	size_t length = 83;
	char buffer[16] = "unchanged";
	CHECK(!soml_validate(nullptr, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
	CHECK(!soml_measure(nullptr, &length, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && length == 83);
	CHECK(!soml_measure(document, nullptr, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
	CHECK(!soml_write_to_buffer(nullptr, buffer, sizeof(buffer), &length, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && length == 83);
	CHECK(!soml_write_to_buffer(document, buffer, sizeof(buffer), nullptr, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
	CHECK(!soml_write_to_buffer(document, nullptr, 1, &length, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && length == 83);
	CHECK(!soml_write_to_buffer(document, nullptr, 0, &length, &error) && error.code == SOML_ERROR_WRITE && length == 83);
	CHECK(!soml_write_canonical_to_buffer(document, nullptr, 0, &length, &error) && error.code == SOML_ERROR_WRITE && length == 83);
	CHECK(strcmp(buffer, "unchanged") == 0 && !soml_document_get_error(document, &error));
	CHECK(!soml_array_remove(document, soml_document_root(document), 0));
	CHECK(soml_document_get_error(document, &error));
	const char *message = error.message;
	CHECK(!soml_validate(document, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && error.message == message);
	CHECK(!soml_measure(document, &length, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && length == 83 && error.message == message);
	CHECK(!soml_write_to_buffer(document, buffer, sizeof(buffer), &length, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && length == 83 && error.message == message && strcmp(buffer, "unchanged") == 0);
	CHECK(!soml_write_canonical_to_buffer(document, buffer, sizeof(buffer), &length, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT && length == 83 && strcmp(buffer, "unchanged") == 0);
	soml_document_free(document);
}

static void test_writer_api_scratch_failure(void) {
	static const char expected[] = "a: 3\nz: {\n\ta: 2\n\tz: 1\n}\n";
	for (unsigned operation = 0; operation < 4; operation++) {
		for (size_t failing_sort = 0; failing_sort < 2; failing_sort++) {
			counting_allocator state = {.fail_at = SIZE_MAX};
			soml_allocator allocator = {.reallocate = counting_reallocate, .context = &state};
			soml_document *document = soml_document_create(&allocator);
			soml_value *root = soml_new_object(document);
			soml_value *nested = soml_new_object(document);
			CHECK(soml_object_add(document, nested, "z", 1, soml_new_int(document, 1)));
			CHECK(soml_object_add(document, nested, "a", 1, soml_new_int(document, 2)));
			CHECK(soml_object_add(document, root, "z", 1, nested));
			CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 3)));
			CHECK(soml_document_set_root(document, root));
			size_t retained = state.live_bytes;
			size_t blocks = state.live_blocks;
			state.fail_at = state.allocations + failing_sort;
			size_t length = 97;
			char buffer[128] = "unchanged";
			soml_error error;
			bool success = operation == 0 ? soml_validate(document, &error) : operation == 1 ? soml_measure(document, &length, &error) : operation == 2 ? soml_write_to_buffer(document, buffer, sizeof(buffer), &length, &error) : soml_write_canonical_to_buffer(document, buffer, sizeof(buffer), &length, &error);
			CHECK(!success && error.code == SOML_ERROR_OUT_OF_MEMORY && length == 97);
			CHECK(state.live_bytes == retained && state.live_blocks == blocks && soml_document_allocated_bytes(document) == retained);
			CHECK(!soml_document_get_error(document, &error));
			CHECK(string_equals(soml_object_key_at(root, 0), "z") && string_equals(soml_object_key_at(nested, 0), "z"));
			state.fail_at = SIZE_MAX;
			CHECK(soml_validate(document, &error));
			CHECK(soml_measure(document, &length, &error) && length == sizeof(expected) - 1);
			CHECK(soml_write_canonical_to_buffer(document, buffer, sizeof(buffer), &length, &error) && strcmp(buffer, expected) == 0);
			CHECK(state.live_bytes == retained && state.live_blocks == blocks);
			soml_document_free(document);
			CHECK(state.live_blocks == 0 && state.live_bytes == 0);
		}
	}
}

static void test_built_object_index(void) {
	fuzz_allocator state = {.fail_at = SIZE_MAX};
	soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
	soml_document *document = soml_document_create(&allocator);
	soml_value *root = soml_new_object(document);
	static const soml_string keys[] = {{"z", 1}, {"a\0", 2}, {"a", 1}, {"", 0}, {"m", 1}};
	soml_value *values[5];

	for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
		values[index] = soml_new_int(document, (int64_t)index);
		CHECK(soml_object_add(document, root, keys[index].data, keys[index].length, values[index]));
	}

	CHECK(soml_document_set_root(document, root));
	CHECK(!root->is_sorted);
	CHECK(soml_object_index(document, root) && root->is_sorted && root->as.object.sorted != nullptr);
	soml_value *replacement = soml_new_int(document, 10);
	state.fail_at = state.allocations;
	size_t allocations = state.allocations;
	CHECK(soml_object_index(document, root));
	CHECK(soml_object_set(document, root, "z", 1, replacement));
	CHECK(soml_object_remove(document, root, "a\0", 2));
	CHECK(root->is_sorted);
	soml_error error;
	size_t measured;
	char buffer[128];
	CHECK(soml_validate(document, &error));
	CHECK(soml_measure(document, &measured, &error));
	CHECK(soml_write_to_buffer(document, buffer, sizeof(buffer), &measured, &error) && strcmp(buffer, "z: 10\na: 2\n'': 3\nm: 4\n") == 0);
	CHECK(soml_write_canonical_to_buffer(document, buffer, sizeof(buffer), &measured, &error) && strcmp(buffer, "'': 3\na: 2\nm: 4\nz: 10\n") == 0);
	CHECK(state.allocations == allocations);

	for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
		CHECK(soml_object_get_with_length(root, keys[index].data, keys[index].length) == (index == 0 ? replacement : index == 1 ? nullptr : values[index]));
	}

	CHECK(soml_object_value_at(root, 0) == replacement && soml_object_value_at(root, 1) == values[2] && soml_object_value_at(root, 2) == values[3] && soml_object_value_at(root, 3) == values[4]);
	state.fail_at = SIZE_MAX;
	CHECK(soml_object_add(document, root, "b", 1, soml_new_int(document, 11)));
	CHECK(soml_object_index(document, root));
	CHECK(soml_write_canonical_to_buffer(document, buffer, sizeof(buffer), &measured, nullptr) && strcmp(buffer, "'': 3\na: 2\nb: 11\nm: 4\nz: 10\n") == 0);
	soml_document_free(document);
	CHECK(state.live_blocks == 0);

	for (unsigned mode = 0; mode < 2; mode++) {
		state = (fuzz_allocator){.fail_at = SIZE_MAX};
		document = soml_document_create(&allocator);
		root = soml_new_object(document);
		CHECK(soml_object_add(document, root, "z", 1, soml_new_int(document, 1)));
		CHECK(soml_object_add(document, root, "a", 1, soml_new_int(document, 2)));

		if (mode == 0) {
			CHECK(soml_object_add(document, root, "z", 1, soml_new_int(document, 3)));
		} else {
			state.fail_at = state.allocations;
		}

		CHECK(!soml_object_index(document, root));
		CHECK(!root->is_sorted && string_equals(soml_object_key_at(root, 0), "z") && soml_count(root) == (mode == 0 ? 3 : 2));
		CHECK(soml_document_get_error(document, &error) && error.code == (mode == 0 ? SOML_ERROR_INVALID_VALUE : SOML_ERROR_OUT_OF_MEMORY));
		CHECK(soml_object_get(root, "z") == soml_object_value_at(root, 0));
		soml_document_free(document);
		CHECK(state.live_blocks == 0);
	}

	document = soml_document_create(nullptr);
	root = soml_new_object(document);
	CHECK(soml_object_index(document, root));
	CHECK(soml_object_add(document, root, "a", 1, soml_new_null(document)));
	CHECK(soml_object_index(document, root));
	CHECK(!soml_object_index(nullptr, root));
	CHECK(!soml_document_get_error(document, &error));
	soml_document *foreign = soml_document_create(nullptr);
	soml_value *foreign_object = soml_new_object(foreign);
	CHECK(soml_object_add(foreign, foreign_object, "z", 1, soml_new_null(foreign)));
	CHECK(soml_object_add(foreign, foreign_object, "a", 1, soml_new_null(foreign)));
	CHECK(!soml_object_index(document, foreign_object));
	CHECK(!foreign_object->is_sorted && soml_count(foreign_object) == 2 && !soml_document_get_error(foreign, &error));
	CHECK(!soml_object_index(document, nullptr));
	CHECK(!soml_object_index(document, soml_new_array(document)));
	CHECK(soml_document_get_error(document, &error) && error.code == SOML_ERROR_INVALID_ARGUMENT);
	soml_document_free(foreign);
	soml_document_free(document);
}

int main(int argument_count, char **arguments) {
	// ThreadSanitizer exercises concurrent reads directly; the full suite's large scalar corpora do not exercise shared state.
	if (argument_count == 2 && strcmp(arguments[1], "--concurrent") == 0) {
		test_concurrent_reads();
		printf("%d concurrency checks, %d failures\n", check_count, failure_count);
		return failure_count != 0;
	}

	test_bare_key_word_model();
	test_document_inspection();
	test_caller_buffer_lengths_and_guards();
	test_validation_rejects_and_recovers();
	test_validation_depth_and_root();
	test_writer_api_invalid_arguments_and_builder_failure();
	test_writer_api_scratch_failure();
	test_built_object_index();
	test_collection_views_and_reserve();
	test_array_editing_ownership();
	test_object_editing_ownership();
	test_parsed_object_index_after_removals();
	test_collection_reserve_failure();
	test_collection_edit_failure_atomicity();
	test_array_insert_allocation_failure();
	test_object_duplicate_editing_and_foreign_values();
	test_small_float_formatting_model();
	test_comment_heavy_arena_budget();
	test_duration_fraction_model();
	test_calendar_model();
	test_retained_output_allocation_budget();
	test_merge_sort_model();
	test_unterminated_string_allocation_budget();
	test_comment_delimiter_model();
	test_accessors();
	test_member_order();
	test_strings_with_nul();
	test_text_needs_no_terminator();
	test_size_overflow();
	test_allocation_size_overflow();
	test_accessor_bounds();
	test_arena_redzones();
	test_error_positions();
	test_dot_after_key();
	test_error_hints();
	test_suggestions();
	test_errors_that_do_not_mislead();
	test_missing_value_before_key();
	test_first_collision();
	test_characters_at_every_offset();
	test_rules();
	test_separators();
	test_depth();
	test_small_stack();
	test_string_scanning();
	test_utf8_sequences();
	test_quote_bytes();
	test_builder();
	test_builder_copies_text();
	test_write_errors();
	test_nan_payloads();
	test_builder_canonical_forms();
	test_built_key_order();
	test_changing_parsed_documents();
	test_growing_empty_parsed_collections();
	test_write_member_order();
	test_values_from_another_document();
	test_write_depth();
	test_one_place_per_value();
	test_byte_masks();
	test_string_scanner_boundaries();
	test_unescaped_string_allocations();
	test_ordered_writer_allocation_budget();
	test_indented_block_string_budget();
	test_parsed_ordering_mutations();
	test_write_callback();
	test_concurrent_reads();
	test_decimal_shifts();
	test_float_parsing();
	test_float_halfway_values();
	test_float_formatting();
	test_float_round_trip();
	test_float_boundaries();
	test_float_fast_path();
	test_wide_float_model();
	test_clone_lifetime();
	test_clone_failures();
	test_clone_depth_and_invalid_scalars();
	test_rounding_modes();
	test_flush_subnormals();
	test_whole_float_formatting();
	test_allocation_failure();
	test_fuzz_allocator();
	test_allocator_lifetime();
	test_rejected_builder_ownership();
	test_allocation_failure_while_building();
	test_builder_state_after_allocation_failure();
	test_write_allocation_retry();
	test_write_value_failure_retry();
	test_write_output_after_allocation_failure();
	test_write_output_after_building();
	test_allocation_failure_in_diagnoses(false);
	test_allocation_failure_in_diagnoses(true);
	test_built_lookup_model();
	test_built_round_trip();
	test_reads_at_the_end_of_the_text();
	test_misaligned_buffers();
	test_every_control_escape();
	test_builder_failures_are_recorded();
	test_first_builder_failure();
	test_instant_range();
	test_builder_scalar_storage();
	test_value_details();
	test_writer_frees_its_memory();
	// WebAssembly grows its memory in steps that make timings too uneven to compare.
	#ifndef __wasm__
	test_complexity();
	#endif

	if (failure_count > 0) {
		fprintf(stderr, "%d of %d checks failed\n", failure_count, check_count);
		return 1;
	}

	printf("%d checks passed\n", check_count);
	return 0;
}
