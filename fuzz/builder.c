/*
A fuzz target for the builder and writer, for libFuzzer and AFL++. `make fuzz` builds and runs it.

The input is a program: each byte chooses an operation (make a value, append, add a member, set the root, look up, write), and the bytes after it are the operation's arguments. Values come from two documents, so that values from the wrong document are tried too, and any value can be given to any operation, including `nullptr` and values already in a container.

The checks: nothing may crash or read out of bounds, allocator sizes must match and nothing may leak, a text that `soml_write` writes must parse and write back to the same bytes, the text read back must have the same canonical form, and each callback writer must write the same bytes as its `soml_write` twin, or fail with the same error code, except when an allocation fails. A callback that refuses a piece must stop the writer immediately with `SOML_ERROR_WRITE`, after a correct prefix of the text. Getters must succeed only for their matching type and leave failed outputs unchanged. Key lookup must find the first member with the exact bytes and length of the query. Insertions must update the count and add the intended value and key only on success, and a failed root change must keep the previous root.

Strings and keys are passed in exact-size copies, checked for changes and freed immediately after each call, so modifying an argument, reading beyond it, or retaining it is caught. Every new string and added key must keep its exact bytes after that free, even when the value is never written. Scalar values must retain their supplied values too, with negative zero stored as zero and NaNs remaining NaN until the writer refuses them.

Built with `-DFUZZ_FAILING_ALLOCATOR`, the first two bytes choose which allocation fails.
*/
#define SOML_IMPLEMENTATION
#include "../soml.h"
#include "allocator.h"

#include <stdlib.h>
#include <string.h>

typedef struct reader {
	const uint8_t *data;
	size_t size;
	size_t position;
} reader;

static uint8_t read_byte(reader *input) {
	return input->position < input->size ? input->data[input->position++] : 0;
}

static uint64_t read_bits(reader *input) {
	uint64_t value = 0;

	for (int index = 0; index < 8; index++) {
		value = value << 8 | read_byte(input);
	}

	return value;
}

/*
An exact-size copy of up to 32 bytes of the input, as a string or key that may be invalid UTF-8 or hold a carriage return. The caller frees it after the API call.
*/
static char *read_text(reader *input, size_t *length) {
	size_t wanted = read_byte(input) % 33;
	size_t available = input->size - input->position;
	*length = wanted < available ? wanted : available;
	char *text = malloc(*length == 0 ? 1 : *length);

	if (text == nullptr) {
		abort();
	}

	if (*length > 0) {
		memcpy(text, input->data + input->position, *length);
	}

	input->position += *length;
	return text;
}

/*
Checks that a string or key read from the input was not modified by the API call, then frees it.
*/
static void free_text(const reader *input, char *text, size_t length) {
	if (length > 0 && memcmp(text, input->data + input->position - length, length) != 0) {
		abort();
	}

	free(text);
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

	char *grown = realloc(target->text, target->length + length);

	if (grown == nullptr) {
		return false;
	}

	memcpy(grown + target->length, bytes, length);
	target->text = grown;
	target->length += length;
	return true;
}

/*
A callback writer writes the same bytes as its `soml_write` twin, or fails with the same error code, also when a builder failed earlier. Stored output must end with a NUL on success and remain unchanged on failure and after later writes. Allocation failure can affect one writer without affecting the other.
*/
static void check_callback_twin(soml_document *document, bool is_canonical) {
	soml_string text = {.data = "unchanged", .length = SIZE_MAX};
	soml_string previous = text;
	soml_error error;
	bool is_written = is_canonical ? soml_write_canonical(document, &text, &error) : soml_write(document, &text, &error);

	if (is_written ? text.data == nullptr || text.data[text.length] != '\0' : text.data != previous.data || text.length != previous.length) {
		abort();
	}

	collector target = {.fail_after = SIZE_MAX};
	soml_error callback_error;
	bool is_called_back = is_canonical ? soml_write_canonical_with_callback(document, collect, &target, &callback_error) : soml_write_with_callback(document, collect, &target, &callback_error);

	bool allocation_failed = (!is_written && error.code == SOML_ERROR_OUT_OF_MEMORY) || (!is_called_back && callback_error.code == SOML_ERROR_OUT_OF_MEMORY);

	if (!allocation_failed) {
		if (is_written != is_called_back) {
			abort();
		}

		bool is_same = is_written ? target.length == text.length && memcmp(target.text, text.data, text.length) == 0 : callback_error.code == error.code;

		if (!is_same) {
			abort();
		}
	}

	if (is_written && is_called_back) {
		// Refuse the middle piece, including the first one when the text fits in a single callback.
		collector stopped = {.fail_after = target.calls / 2};
		bool is_complete = is_canonical ? soml_write_canonical_with_callback(document, collect, &stopped, &callback_error) : soml_write_with_callback(document, collect, &stopped, &callback_error);

		if (is_complete || (callback_error.code != SOML_ERROR_OUT_OF_MEMORY && (callback_error.code != SOML_ERROR_WRITE || stopped.calls != stopped.fail_after + 1))) {
			abort();
		}

		if (stopped.length > text.length || (stopped.length > 0 && memcmp(stopped.text, text.data, stopped.length) != 0)) {
			abort();
		}

		free(stopped.text);

		// The callback's copy also checks that another stored write cannot invalidate the earlier text.
		soml_string later;
		bool is_written_later = is_canonical ? soml_write(document, &later, &error) : soml_write_canonical(document, &later, &error);

		if ((!is_written_later && error.code != SOML_ERROR_OUT_OF_MEMORY) || memcmp(text.data, target.text, text.length) != 0 || text.data[text.length] != '\0') {
			abort();
		}
	}

	free(target.text);
}

static void check_write(soml_document *document) {
	// Writer errors must never become persistent builder failures.
	soml_error_code previous_failure = document == nullptr ? SOML_ERROR_NONE : document->failure;
	check_callback_twin(document, false);
	check_callback_twin(document, true);
	soml_string text;
	soml_error error;

	if (!soml_write(document, &text, &error)) {
		if (error.code == SOML_ERROR_NONE || error.message == nullptr || (document != nullptr && document->failure != previous_failure)) {
			abort();
		}

		return;
	}

	// Validation and non-retaining output agree with the successful stored writer. Scratch allocation failures remain retriable.
	size_t measured = SIZE_MAX;
	soml_error inspection_error;

	if (!soml_validate(document, &inspection_error) && inspection_error.code != SOML_ERROR_OUT_OF_MEMORY) {
		abort();
	}

	bool is_measured = soml_measure(document, &measured, &inspection_error);

	if (is_measured ? measured != text.length : inspection_error.code != SOML_ERROR_OUT_OF_MEMORY || measured != SIZE_MAX) {
		abort();
	}

	char *buffer = malloc(text.length + 1);

	if (buffer == nullptr) {
		abort();
	}

	size_t length = SIZE_MAX;
	bool is_buffered = soml_write_to_buffer(document, buffer, text.length + 1, &length, &inspection_error);

	if (is_buffered ? length != text.length || memcmp(buffer, text.data, text.length + 1) != 0 : inspection_error.code != SOML_ERROR_OUT_OF_MEMORY || length != SIZE_MAX) {
		abort();
	}

	free(buffer);
	soml_document *copy = soml_document_clone(document, nullptr, &inspection_error);

	if (copy != nullptr) {
		soml_string copied;

		if (!soml_write(copy, &copied, &inspection_error) || copied.length != text.length || memcmp(copied.data, text.data, text.length + 1) != 0) {
			abort();
		}

		soml_document_free(copy);
	} else if (inspection_error.code != SOML_ERROR_OUT_OF_MEMORY) {
		abort();
	}

	// The text reads back, and writes back to the same bytes.
	soml_document *again = soml_parse(text.data, text.length, nullptr, &error);

	if (again == nullptr) {
		abort();
	}

	soml_string second;

	if (!soml_write(again, &second, &error) || second.length != text.length || memcmp(second.data, text.data, text.length) != 0) {
		abort();
	}

	// Both documents have the same canonical form, because member order is all that the text and canonical form differ in.
	soml_string canonical;
	soml_string second_canonical;

	if (soml_write_canonical(document, &canonical, &error)) {
		if (!soml_write_canonical(again, &second_canonical, &error) || second_canonical.length != canonical.length || memcmp(second_canonical.data, canonical.data, canonical.length) != 0) {
			abort();
		}
	} else if (error.code != SOML_ERROR_OUT_OF_MEMORY) {
		abort();
	}

	soml_document_free(again);

	if (document->failure != previous_failure) {
		abort();
	}
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
	constexpr size_t value_capacity = 64;
	soml_document *documents[2] = {soml_document_create(&allocator), soml_document_create(&allocator)};
	// Documents copy the allocator, so the caller's struct can be changed immediately.
	allocator = (soml_allocator){};
	soml_value *values[value_capacity] = {};
	size_t value_count = 0;
	reader input = {.data = data, .size = size};

	while (input.position < input.size) {
		uint8_t operation = read_byte(&input);
		soml_document *document = documents[operation >> 7];
		soml_error_code previous_failure = document == nullptr ? SOML_ERROR_NONE : document->failure;
		const char *previous_message = document == nullptr ? nullptr : document->failure_message;
		soml_value *first = values[read_byte(&input) % value_capacity];
		soml_value *second = values[read_byte(&input) % value_capacity];
		soml_value *made = nullptr;
		size_t length;
		char *text;

		switch (operation % 32) {
			case 0: {
				text = read_text(&input, &length);
				made = soml_new_string(document, text, length);
				free_text(&input, text, length);

				if (made != nullptr) {
					soml_string string;

					if (!soml_get_string(made, &string) || string.data == nullptr || string.length != length || string.data[length] != '\0' || (length > 0 && memcmp(string.data, input.data + input.position - length, length) != 0)) {
						abort();
					}
				}

				break;
			}

			case 1: {
				int64_t expected = (int64_t)read_bits(&input);
				int64_t actual = ~expected;
				made = soml_new_int(document, expected);

				if (made != nullptr && (!soml_get_int(made, &actual) || actual != expected)) {
					abort();
				}

				break;
			}

			case 2: {
				uint64_t bits = read_bits(&input);
				double number;
				memcpy(&number, &bits, sizeof(number));
				made = soml_new_float(document, number);

				if (made != nullptr) {
					uint64_t actual_bits = ~bits;
					double actual;
					memcpy(&actual, &actual_bits, sizeof(actual));

					if (!soml_get_float(made, &actual)) {
						abort();
					}

					memcpy(&actual_bits, &actual, sizeof(actual_bits));
					bool is_nan = (bits & 0x7FFFFFFFFFFFFFFF) > 0x7FF0000000000000;
					bool actual_is_nan = (actual_bits & 0x7FFFFFFFFFFFFFFF) > 0x7FF0000000000000;
					uint64_t expected_bits = bits == 0x8000000000000000 ? 0 : bits;

					if (is_nan ? !actual_is_nan : actual_bits != expected_bits) {
						abort();
					}
				}

				break;
			}

			case 3: {
				uint8_t kind = read_byte(&input) % 3;
				bool expected = kind == 0;
				bool actual = !expected;
				made = kind == 2 ? soml_new_null(document) : soml_new_bool(document, expected);

				if (made != nullptr && (kind == 2 ? !soml_is_null(made) : !soml_get_bool(made, &actual) || actual != expected)) {
					abort();
				}

				break;
			}

			case 4: {
				// The full integer widths include valid instants and extremes that the writer must refuse.
				int64_t seconds = (int64_t)read_bits(&input);
				int32_t nanoseconds = (int32_t)read_bits(&input);
				made = soml_new_instant(document, (soml_instant){.seconds = seconds, .nanoseconds = nanoseconds});
				soml_instant actual = {.seconds = ~seconds, .nanoseconds = ~nanoseconds};

				if (made != nullptr && (!soml_get_instant(made, &actual) || actual.seconds != seconds || actual.nanoseconds != nanoseconds)) {
					abort();
				}

				break;
			}

			case 5: {
				int64_t expected = (int64_t)read_bits(&input);
				int64_t actual = ~expected;
				made = soml_new_duration(document, expected);

				if (made != nullptr && (!soml_get_duration(made, &actual) || actual != expected)) {
					abort();
				}

				break;
			}

			case 6: {
				made = soml_new_array(document);
				break;
			}

			case 7: {
				made = soml_new_object(document);
				break;
			}

			case 8: {
				size_t count = soml_count(first);
				bool was_attached = second != nullptr && second->is_attached;
				bool is_appended = soml_array_append(document, first, second);

				if (soml_count(first) != count + (size_t)is_appended || (is_appended && soml_array_at(first, count) != second) || (!is_appended && second != nullptr && second->is_attached != was_attached)) {
					abort();
				}

				break;
			}

			case 9: {
				text = read_text(&input, &length);
				size_t count = soml_count(first);
				bool was_attached = second != nullptr && second->is_attached;
				bool is_added = soml_object_add(document, first, text, length, second);
				free_text(&input, text, length);

				if (soml_count(first) != count + (size_t)is_added || (!is_added && second != nullptr && second->is_attached != was_attached)) {
					abort();
				}

				if (is_added) {
					soml_string key = soml_object_key_at(first, count);

					if (soml_object_value_at(first, count) != second || key.data == nullptr || key.length != length || key.data[key.length] != '\0' || (length > 0 && memcmp(key.data, input.data + input.position - length, length) != 0)) {
						abort();
					}
				}

				break;
			}

			case 10: {
				soml_value *previous = soml_document_root(document);
				bool was_attached = first != nullptr && first->is_attached;
				bool is_set = soml_document_set_root(document, first);

				if (soml_document_root(document) != (is_set ? first : previous) || (!is_set && ((first != nullptr && first->is_attached != was_attached) || (previous != nullptr && !previous->is_attached)))) {
					abort();
				}

				break;
			}

			case 11: {
				check_write(document);
				break;
			}

			case 12: {
				// Every accessor on any value, which may be nullptr or of another type.
				text = read_text(&input, &length);
				soml_type type = soml_type_of(first);
				soml_string string = {.data = text, .length = SIZE_MAX};
				int64_t integer = 7;
				double number = 7;
				bool boolean = true;
				soml_instant instant = {.seconds = 7, .nanoseconds = 7};
				int64_t duration = 7;
				size_t count = soml_count(first);
				soml_value *expected = nullptr;

				for (size_t index = 0; index < count; index++) {
					soml_string key = soml_object_key_at(first, index);

					if (key.data != nullptr && key.length == length && (length == 0 || memcmp(key.data, text, length) == 0)) {
						expected = soml_object_value_at(first, index);
						break;
					}
				}

				if (soml_object_get_with_length(first, text, length) != expected) {
					abort();
				}

				if (soml_get_string(first, &string) != (type == SOML_TYPE_STRING) || soml_get_int(first, &integer) != (type == SOML_TYPE_INT) || soml_get_float(first, &number) != (type == SOML_TYPE_FLOAT) || soml_get_bool(first, &boolean) != (type == SOML_TYPE_BOOL) || soml_get_instant(first, &instant) != (type == SOML_TYPE_INSTANT) || soml_get_duration(first, &duration) != (type == SOML_TYPE_DURATION) || soml_is_null(first) != (type == SOML_TYPE_NULL)) {
					abort();
				}

				// A failed getter must leave every output field unchanged.
				if ((type != SOML_TYPE_STRING && (string.data != text || string.length != SIZE_MAX)) || (type != SOML_TYPE_INT && integer != 7) || (type != SOML_TYPE_FLOAT && number != 7) || (type != SOML_TYPE_BOOL && !boolean) || (type != SOML_TYPE_INSTANT && (instant.seconds != 7 || instant.nanoseconds != 7)) || (type != SOML_TYPE_DURATION && duration != 7)) {
					abort();
				}

				if (type == SOML_TYPE_STRING && (string.data == nullptr || string.data[string.length] != '\0')) {
					abort();
				}

				free_text(&input, text, length);

				// One index past the end too, which must not read out of bounds.
				for (size_t index = 0; index <= count; index++) {
					(void)soml_array_at(first, index);
					(void)soml_object_key_at(first, index);
					(void)soml_object_value_at(first, index);
				}

				const size_t indexes[] = {count, SIZE_MAX, SIZE_MAX / 2 + 1};

				for (size_t position = 0; position < sizeof(indexes) / sizeof(indexes[0]); position++) {
					size_t index = indexes[position];
					soml_string key = soml_object_key_at(first, index);

					if (index >= count && (soml_array_at(first, index) != nullptr || key.data != nullptr || key.length != 0 || soml_object_value_at(first, index) != nullptr)) {
						abort();
					}
				}

				break;
			}

			case 13: {
				(void)soml_array_reserve(document, first, read_byte(&input));
				break;
			}

			case 14: {
				(void)soml_object_reserve(document, first, read_byte(&input));
				break;
			}

			case 15: {
				size_t index = read_byte(&input);
				size_t count = soml_count(first);
				bool is_inserted = soml_array_insert(document, first, index, second);

				if (soml_count(first) != count + (size_t)is_inserted || (is_inserted && soml_array_at(first, index) != second)) {
					abort();
				}

				break;
			}

			case 16: {
				size_t index = read_byte(&input);
				soml_value *previous = soml_array_at(first, index);
				bool is_set = soml_array_set(document, first, index, second);

				if (soml_array_at(first, index) != (is_set ? second : previous)) {
					abort();
				}

				break;
			}

			case 17: {
				size_t index = read_byte(&input);
				size_t count = soml_count(first);
				soml_value *previous = soml_array_at(first, index);
				bool is_removed = soml_array_remove(document, first, index);

				if (soml_count(first) != count - (size_t)is_removed || (is_removed && previous->is_attached)) {
					abort();
				}

				break;
			}

			case 18: {
				made = soml_array_pop(document, first);
				break;
			}

			case 19: {
				(void)soml_array_clear(document, first);
				break;
			}

			case 20: {
				text = read_text(&input, &length);
				soml_value *previous = soml_object_get_with_length(first, text, length);
				bool is_set = soml_object_set(document, first, text, length, second);

				if (soml_object_get_with_length(first, text, length) != (is_set ? second : previous)) {
					abort();
				}

				free_text(&input, text, length);
				break;
			}

			case 21: {
				text = read_text(&input, &length);
				(void)soml_object_remove(document, first, text, length);
				free_text(&input, text, length);
				break;
			}

			case 22: {
				text = read_text(&input, &length);
				made = soml_object_take(document, first, text, length);
				free_text(&input, text, length);
				break;
			}

			case 23: {
				(void)soml_object_clear(document, first);
				break;
			}

			case 24: {
				(void)soml_object_index(document, first);
				break;
			}

			case 25: {
				made = soml_clone_value(document, first);
				break;
			}

			case 26: {
				soml_array_view array = soml_array_items(first);
				soml_object_view object = soml_object_members(first);

				for (size_t index = 0; index < array.count; index++) {
					if (array.data[index] != soml_array_at(first, index)) {
						abort();
					}
				}

				for (size_t index = 0; index < object.count; index++) {
					if (object.data[index].value != soml_object_value_at(first, index) || object.data[index].key.data != soml_object_key_at(first, index).data) {
						abort();
					}
				}

				break;
			}

			case 27: {
				soml_error error;
				bool has_error = soml_document_get_error(document, &error);

				if (has_error != (previous_failure != SOML_ERROR_NONE) || (has_error && error.code != previous_failure) || (document != nullptr && soml_document_allocated_bytes(document) == 0)) {
					abort();
				}

				break;
			}

			default: {
				break;
			}
		}

		// A later builder failure or successful operation must never replace the first failure.
		if (previous_failure != SOML_ERROR_NONE && (document->failure != previous_failure || document->failure_message == nullptr || strcmp(document->failure_message, previous_message) != 0)) {
			abort();
		}

		if (made != nullptr) {
			values[value_count++ % value_capacity] = made;
		}
	}

	check_write(documents[0]);
	check_write(documents[1]);
	soml_document_free(documents[0]);
	soml_document_free(documents[1]);

	if (state.live_blocks != 0) {
		abort();
	}

	return 0;
}
