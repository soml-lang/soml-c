/*
A command-line tool for the conformance tests. For each file given, it prints one line of JSON:

	{"file": "...", "ok": true, "value": <tagged value>, "canonical": "..."}
	{"file": "...", "ok": false, "code": 1, "message": "...", "line": 1, "column": 5}

The tagged value is the format of the conformance suite: arrays and objects as JSON, and every scalar as `{"type": "...", "value": "..."}`. A float's value is its canonical spelling, an instant's is its canonical UTC form, and a duration's is its length in nanoseconds. The canonical text is from `soml_write_canonical`.

With `-` as the only argument, it reads the paths from standard input, one per line. A failure to read the paths or write the results gives a nonzero exit status.
*/
#define SOML_IMPLEMENTATION
#include "../soml.h"

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void print_json_string(const char *data, size_t length) {
	putchar('"');
	size_t chunk_start = 0;

	for (size_t index = 0; index < length; index++) {
		unsigned char byte = (unsigned char)data[index];

		if (byte != '"' && byte != '\\' && byte >= 0x20 && byte != 0x7F) {
			continue;
		}

		fwrite(data + chunk_start, 1, index - chunk_start, stdout);

		if (byte == '"' || byte == '\\') {
			putchar('\\');
			putchar(byte);
		} else {
			static constexpr char hex[] = "0123456789abcdef";
			char escape[] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0xF]};
			fwrite(escape, 1, sizeof(escape), stdout);
		}

		chunk_start = index + 1;
	}

	fwrite(data + chunk_start, 1, length - chunk_start, stdout);
	putchar('"');
}

/*
The canonical text of one scalar, formatted directly without a temporary document.
*/
static void print_canonical_scalar(const soml_value *value) {
	double number;
	soml_instant instant;
	char text[40];
	size_t length;

	if (soml_get_float(value, &number)) {
		length = soml__format_float(number, text);
	} else if (soml_get_instant(value, &instant)) {
		length = soml__format_instant(instant, text);
	} else {
		fprintf(stderr, "Could not write a scalar\n");
		exit(2);
	}

	// No `a: ` member prefix or trailing line feed is needed.
	print_json_string(text, length);
}

static void print_tagged(const soml_value *value) {
	switch (soml_type_of(value)) {
		case SOML_TYPE_ARRAY: {
			putchar('[');
			size_t count = soml_count(value);

			for (size_t index = 0; index < count; index++) {
				if (index > 0) {
					putchar(',');
				}

				print_tagged(soml_array_at(value, index));
			}

			putchar(']');
			return;
		}

		case SOML_TYPE_OBJECT: {
			putchar('{');
			size_t count = soml_count(value);

			for (size_t index = 0; index < count; index++) {
				if (index > 0) {
					putchar(',');
				}

				soml_string key = soml_object_key_at(value, index);
				print_json_string(key.data, key.length);
				putchar(':');
				print_tagged(soml_object_value_at(value, index));
			}

			putchar('}');
			return;
		}

		case SOML_TYPE_STRING: {
			soml_string string;
			(void)soml_get_string(value, &string);
			printf("{\"type\":\"string\",\"value\":");
			print_json_string(string.data, string.length);
			putchar('}');
			return;
		}

		case SOML_TYPE_INT: {
			int64_t integer;
			(void)soml_get_int(value, &integer);
			printf("{\"type\":\"int\",\"value\":\"%" PRId64 "\"}", integer);
			return;
		}

		case SOML_TYPE_FLOAT: {
			printf("{\"type\":\"float\",\"value\":");
			print_canonical_scalar(value);
			putchar('}');
			return;
		}

		case SOML_TYPE_BOOL: {
			bool boolean;
			(void)soml_get_bool(value, &boolean);
			printf("{\"type\":\"bool\",\"value\":\"%s\"}", boolean ? "true" : "false");
			return;
		}

		case SOML_TYPE_NULL: {
			printf("{\"type\":\"null\",\"value\":\"null\"}");
			return;
		}

		case SOML_TYPE_INSTANT: {
			printf("{\"type\":\"instant\",\"value\":");
			print_canonical_scalar(value);
			putchar('}');
			return;
		}

		case SOML_TYPE_DURATION: {
			int64_t nanoseconds;
			(void)soml_get_duration(value, &nanoseconds);
			printf("{\"type\":\"duration\",\"value\":\"%" PRId64 "\"}", nanoseconds);
			return;
		}

		case SOML_TYPE_NONE: {
			printf("null");
			return;
		}
	}
}

static char *read_file(const char *path, size_t *length) {
	FILE *file = fopen(path, "rb");

	if (file == nullptr) {
		return nullptr;
	}

	size_t capacity = 4096;
	size_t used = 0;
	char *buffer = malloc(capacity);

	while (buffer != nullptr) {
		if (used == capacity) {
			size_t new_capacity;

			if (ckd_mul(&new_capacity, capacity, 2)) {
				break;
			}

			char *grown = realloc(buffer, new_capacity);

			if (grown == nullptr) {
				break;
			}

			buffer = grown;
			capacity = new_capacity;
		}

		size_t count = fread(buffer + used, 1, capacity - used, file);

		if (count == 0) {
			if (ferror(file)) {
				break;
			}

			fclose(file);
			*length = used;
			// A block of exactly the text's size, so that AddressSanitizer reports a read past the end of the text, which would otherwise land in the spare capacity.
			char *exact = realloc(buffer, used == 0 ? 1 : used);

			if (exact == nullptr) {
				free(buffer);
			}

			return exact;
		}

		used += count;
	}

	// A read or allocation failure: the caller reports that the file could not be read.
	fclose(file);
	free(buffer);
	return nullptr;
}

static void run(const char *path) {
	size_t length;
	char *text = read_file(path, &length);

	printf("{\"file\":");
	print_json_string(path, strlen(path));

	if (text == nullptr) {
		printf(",\"ok\":false,\"code\":-1,\"message\":\"Could not read the file\"}\n");
		return;
	}

	soml_error error;
	soml_document *document = soml_parse(text, length, nullptr, &error);
	free(text);

	if (document == nullptr) {
		printf(",\"ok\":false,\"code\":%d,\"message\":", (int)error.code);
		print_json_string(error.message, strlen(error.message));
		printf(",\"line\":%zu,\"column\":%zu}\n", error.line, error.column);
		return;
	}

	printf(",\"ok\":true,\"value\":");
	print_tagged(soml_document_root(document));
	soml_string canonical;

	if (soml_write_canonical(document, &canonical, &error)) {
		printf(",\"canonical\":");
		print_json_string(canonical.data, canonical.length);
	} else {
		printf(",\"writeError\":");
		print_json_string(error.message, strlen(error.message));
	}

	printf("}\n");
	soml_document_free(document);
}

/*
Reads whole path records with one reusable buffer. An embedded NUL is not a path separator and must not silently select another file.
*/
static bool read_paths(void) {
	soml_allocator allocator = soml__resolve_allocator(nullptr);
	char *line = nullptr;
	size_t length = 0;
	size_t capacity = 0;
	bool is_success = true;
	char input[4096];
	size_t count;

	while (is_success && (count = fread(input, 1, sizeof(input), stdin)) > 0) {
		size_t position = 0;

		while (position < count) {
			const char *newline = memchr(input + position, '\n', count - position);
			size_t end = newline == nullptr ? count : (size_t)(newline - input);
			size_t chunk_length = end - position;
			size_t needed;

			if (memchr(input + position, '\0', chunk_length) != nullptr || ckd_add(&needed, length, chunk_length) || ckd_add(&needed, needed, 1)) {
				is_success = false;
				break;
			}

			char *grown = soml__grow(&allocator, line, &capacity, 1, needed);

			if (grown == nullptr) {
				is_success = false;
				break;
			}

			line = grown;
			memcpy(line + length, input + position, chunk_length);
			length += chunk_length;

			if (newline != nullptr) {
				if (length > 0) {
					line[length] = '\0';
					run(line);

					if (ferror(stdout)) {
						is_success = false;
						break;
					}
				}

				length = 0;
			}

			position = end + (newline != nullptr);
		}
	}

	if (is_success && !ferror(stdin) && length > 0) {
		line[length] = '\0';
		run(line);
	}

	soml__release(&allocator, line, capacity);
	return is_success && !ferror(stdin) && !ferror(stdout);
}

int main(int argc, char **argv) {
	bool is_success = true;

	if (argc == 2 && strcmp(argv[1], "-") == 0) {
		is_success = read_paths();
	} else {
		for (int index = 1; index < argc; index++) {
			run(argv[index]);

			if (ferror(stdout)) {
				is_success = false;
				break;
			}
		}
	}

	return !is_success || ferror(stdin) || fflush(stdout) != 0 || ferror(stdout);
}
