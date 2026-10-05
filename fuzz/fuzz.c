/*
A fuzz target for libFuzzer and AFL++. `make fuzz` builds and runs it.

Every input is parsed. When it is a valid document, its written text must parse again to an equal value, and give exactly the same text, and both documents must give the same canonical form. Every value's getter must succeed, and string values and object keys must have their promised NUL terminator.

The allocator checks each reported old size and always moves a growing buffer, so incorrect sizes abort and stale pointers are exposed to AddressSanitizer.

The parser gets an exact-size copy of the input, checked for changes and freed before the document is used, so modifying it, reading past it, or retaining it is caught.

Built with `-DFUZZ_FAILING_ALLOCATOR`, the first two bytes of the input choose which allocation fails. A document that parses with a working allocator must then fail only with `SOML_ERROR_OUT_OF_MEMORY`, an invalid one with that or with the error it has with a working allocator, and nothing may leak.
*/
#define SOML_IMPLEMENTATION
#include "../soml.h"
#include "allocator.h"

#include <stdlib.h>
#include <string.h>

static void visit(const soml_value *value, size_t depth) {
	if (depth > 200) {
		abort();
	}

	// The first invalid index and full-width indexes must never alias a valid item or key.
	size_t count = soml_count(value);
	const size_t indexes[] = {count, SIZE_MAX, SIZE_MAX / 2 + 1};

	for (size_t position = 0; position < sizeof(indexes) / sizeof(indexes[0]); position++) {
		size_t index = indexes[position];
		soml_string key = soml_object_key_at(value, index);

		if (index >= count && (soml_array_at(value, index) != nullptr || key.data != nullptr || key.length != 0 || soml_object_value_at(value, index) != nullptr)) {
			abort();
		}
	}

	soml_string string;
	int64_t integer;
	double number;
	bool boolean;
	soml_instant instant;

	switch (soml_type_of(value)) {
		case SOML_TYPE_ARRAY: {
			for (size_t index = 0; index < soml_count(value); index++) {
				visit(soml_array_at(value, index), depth + 1);
			}

			break;
		}

		case SOML_TYPE_OBJECT: {
			for (size_t index = 0; index < soml_count(value); index++) {
				soml_string key = soml_object_key_at(value, index);

				if (key.data == nullptr || key.data[key.length] != '\0') {
					abort();
				}

				// Every member is found by its key.
				if (soml_object_get_with_length(value, key.data, key.length) != soml_object_value_at(value, index)) {
					abort();
				}

				visit(soml_object_value_at(value, index), depth + 1);
			}

			break;
		}

		case SOML_TYPE_STRING: {
			if (!soml_get_string(value, &string) || string.data[string.length] != '\0') {
				abort();
			}

			break;
		}

		case SOML_TYPE_INT: {
			if (!soml_get_int(value, &integer)) {
				abort();
			}

			break;
		}

		case SOML_TYPE_FLOAT: {
			if (!soml_get_float(value, &number)) {
				abort();
			}

			break;
		}

		case SOML_TYPE_BOOL: {
			if (!soml_get_bool(value, &boolean)) {
				abort();
			}

			break;
		}

		case SOML_TYPE_INSTANT: {
			if (!soml_get_instant(value, &instant) || instant.nanoseconds < 0 || instant.nanoseconds > 999'999'999) {
				abort();
			}

			break;
		}

		case SOML_TYPE_DURATION: {
			if (!soml_get_duration(value, &integer)) {
				abort();
			}

			break;
		}

		case SOML_TYPE_NULL: {
			if (!soml_is_null(value)) {
				abort();
			}

			break;
		}

		case SOML_TYPE_NONE: {
			abort();
		}
	}
}

/*
Whether two values are equal as the specification defines it: the same type, the same bits or bytes, the same items in order, and the same members in any order.
*/
static bool values_equal(const soml_value *left, const soml_value *right) {
	soml_type type = soml_type_of(left);

	if (type != soml_type_of(right)) {
		return false;
	}

	switch (type) {
		case SOML_TYPE_STRING: {
			soml_string left_string;
			soml_string right_string;
			if (!soml_get_string(left, &left_string) || !soml_get_string(right, &right_string)) {
				return false;
			}

			return left_string.length == right_string.length && memcmp(left_string.data, right_string.data, left_string.length) == 0;
		}

		case SOML_TYPE_INT:
		case SOML_TYPE_DURATION: {
			int64_t left_integer = 0;
			int64_t right_integer = 0;
			// Use the getter for this type, and reject a failed read before comparing its output.
			bool is_read = type == SOML_TYPE_INT ? soml_get_int(left, &left_integer) && soml_get_int(right, &right_integer) : soml_get_duration(left, &left_integer) && soml_get_duration(right, &right_integer);

			if (!is_read) {
				return false;
			}

			return left_integer == right_integer;
		}

		case SOML_TYPE_FLOAT: {
			double left_number;
			double right_number;
			if (!soml_get_float(left, &left_number) || !soml_get_float(right, &right_number)) {
				return false;
			}

			// The bits rather than `==`, so that a negative zero, which must be stored as zero, is caught too.
			return memcmp(&left_number, &right_number, sizeof(left_number)) == 0;
		}

		case SOML_TYPE_BOOL: {
			bool left_boolean;
			bool right_boolean;
			if (!soml_get_bool(left, &left_boolean) || !soml_get_bool(right, &right_boolean)) {
				return false;
			}

			return left_boolean == right_boolean;
		}

		case SOML_TYPE_INSTANT: {
			soml_instant left_instant;
			soml_instant right_instant;
			if (!soml_get_instant(left, &left_instant) || !soml_get_instant(right, &right_instant)) {
				return false;
			}

			return left_instant.seconds == right_instant.seconds && left_instant.nanoseconds == right_instant.nanoseconds;
		}

		case SOML_TYPE_ARRAY: {
			if (soml_count(left) != soml_count(right)) {
				return false;
			}

			for (size_t index = 0; index < soml_count(left); index++) {
				if (!values_equal(soml_array_at(left, index), soml_array_at(right, index))) {
					return false;
				}
			}

			return true;
		}

		case SOML_TYPE_OBJECT: {
			if (soml_count(left) != soml_count(right)) {
				return false;
			}

			for (size_t index = 0; index < soml_count(left); index++) {
				soml_string key = soml_object_key_at(left, index);

				if (!values_equal(soml_object_value_at(left, index), soml_object_get_with_length(right, key.data, key.length))) {
					return false;
				}
			}

			return true;
		}

		case SOML_TYPE_NULL: {
			return soml_is_null(left) && soml_is_null(right);
		}

		case SOML_TYPE_NONE: {
			return false;
		}
	}

	return false;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
#ifdef FUZZ_FAILING_ALLOCATOR
	if (size < 2) {
		return 0;
	}

	fuzz_allocator state = {.fail_at = (size_t)data[0] << 8 | data[1]};
	data += 2;
	size -= 2;
#else
	fuzz_allocator state = {.fail_at = SIZE_MAX};
#endif

	soml_allocator allocator = {.reallocate = fuzz_reallocate, .context = &state};
	soml_error error;
	char *text = malloc(size == 0 ? 1 : size);

	if (text == nullptr) {
		abort();
	}

	if (size > 0) {
		memcpy(text, data, size);
	}

	soml_document *document = soml_parse(text, size, &allocator, &error);

	if (size > 0 && memcmp(text, data, size) != 0) {
		abort();
	}

	free(text);

	if (document == nullptr) {
		if (error.code == SOML_ERROR_NONE || error.message == nullptr || error.offset > size) {
			abort();
		}

		// An error in the text has a position. Running out of memory before reading it has none.
		bool is_text_error = error.code == SOML_ERROR_SYNTAX || error.code == SOML_ERROR_TOO_DEEP;

		// Walk complete UTF-8 scalars before a text error, independently of the parser's position calculation. Tabs occupy one column too.
		if (is_text_error) {
			size_t line = 1;
			size_t column = 1;
			size_t offset = 0;

			while (offset < error.offset) {
				uint8_t byte = data[offset];
				if (byte == '\n') {
					line++;
					column = 1;
				} else {
					column++;
				}

				offset += byte < 0x80 ? 1 : byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : 4;
			}

			if (offset != error.offset || error.line != line || error.column != column) {
				abort();
			}
		}

#ifdef FUZZ_FAILING_ALLOCATOR
		// A document that is valid with a working allocator can only fail for lack of memory, and an invalid one fails for lack of memory or with the same error.
		soml_error expected;
		soml_document *valid = soml_parse((const char *)data, size, nullptr, &expected);

		if (error.code != SOML_ERROR_OUT_OF_MEMORY && (valid != nullptr || error.code != expected.code || error.offset != expected.offset || error.line != expected.line || error.column != expected.column || strcmp(error.message, expected.message) != 0)) {
			abort();
		}

		soml_document_free(valid);
#endif
	} else {
		visit(soml_document_root(document), 0);
		soml_string first;

		if (soml_write(document, &first, &error)) {
			soml_document *again = soml_parse(first.data, first.length, &allocator, &error);

			if (again != nullptr) {
				// The text holds the same value.
				if (!values_equal(soml_document_root(document), soml_document_root(again))) {
					abort();
				}

				soml_string second;

				if (soml_write(again, &second, &error)) {
					if (second.length != first.length || memcmp(first.data, second.data, first.length) != 0) {
						abort();
					}
				} else if (error.code != SOML_ERROR_OUT_OF_MEMORY) {
					abort();
				}

				// Member order is all that the text and canonical form differ in, so both documents have the same canonical form.
				soml_string first_canonical;
				soml_string second_canonical;

				if (soml_write_canonical(document, &first_canonical, &error) && soml_write_canonical(again, &second_canonical, &error)) {
					if (second_canonical.length != first_canonical.length || memcmp(first_canonical.data, second_canonical.data, first_canonical.length) != 0) {
						abort();
					}
				} else if (error.code != SOML_ERROR_OUT_OF_MEMORY) {
					abort();
				}

				soml_document_free(again);
			} else if (error.code != SOML_ERROR_OUT_OF_MEMORY) {
				// The text must read back.
				abort();
			}
		} else if (error.code != SOML_ERROR_OUT_OF_MEMORY) {
			// A parsed document can always be written.
			abort();
		}

		soml_document_free(document);
	}

	if (state.live_blocks != 0) {
		abort();
	}

	return 0;
}
