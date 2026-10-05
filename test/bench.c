/*
Measures reading and writing speed. Run it with `make bench`, or with `make bench-compare` to also measure the most used JSON and TOML libraries in C, which it downloads first. Run it from the repository root, because it reads the files in `test/fixtures/bench`.

The first table is for the shared benchmark fixtures of the SOML repository, in `test/fixtures/bench`, each with the same data in SOML, JSON, and TOML: a service config, the config of a code-hosting platform, and earthquakes from the USGS feed. Its readme says what each one measures. The table gives the time per document, because the same data has a different size in each format. Reading builds the library's tree of values, and writing writes it back as text on several lines.

The second table is for documents of one kind of value, each an array of a million copies of it, except for the extreme floats which use 100,000 copies. Floats near the ends of the range are the slow case: they are converted with a high-precision decimal, which takes microseconds per number.

Pass `--quick` to use two rounds, about 250 KB per timed batch, and smaller homogeneous arrays and builder documents. Memory measurements include retained document allocations and exclude transient sorting scratch and allocator overhead. Callback writing counts bytes without copying them, while caller-buffer writing copies into a reusable buffer.

Times are the CPU time of the thread, which other work on the machine changes much less than the time on a clock.
*/
// For `clock_gettime` with glibc in strict C23 mode.
#define _POSIX_C_SOURCE 200809L

#define SOML_IMPLEMENTATION
#include "../soml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef BENCH_COMPARE
	#include "cJSON.h"
	#include "tomlc17.h"
	#include "yyjson.h"
#endif

static size_t round_count = 20;
static size_t target_bytes = 5'000'000;

static double now(void) {
	struct timespec time;

	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
		perror("clock_gettime");
		exit(1);
	}

	return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

static char *read_file(const char *path, size_t *length) {
	FILE *file = fopen(path, "rb");

	if (file == nullptr) {
		fprintf(stderr, "Could not open %s. Run the benchmark from the repository root.\n", path);
		exit(1);
	}

	char *text = nullptr;
	long size = -1;
	size_t capacity;

	if (fseek(file, 0, SEEK_END) == 0) {
		size = ftell(file);
	}

	if (size >= 0 && !ckd_add(&capacity, size, 1) && fseek(file, 0, SEEK_SET) == 0) {
		text = malloc(capacity);
	}

	if (text == nullptr) {
		fclose(file);
		fprintf(stderr, "Could not size or allocate %s\n", path);
		exit(1);
	}

	*length = fread(text, 1, (size_t)size, file);
	bool is_success = *length == (size_t)size && !ferror(file);
	is_success = fclose(file) == 0 && is_success;

	if (!is_success) {
		free(text);
		fprintf(stderr, "Could not read %s\n", path);
		exit(1);
	}

	text[*length] = '\0';
	return text;
}

static char *read_fixture(const char *name, const char *extension, size_t *length) {
	char path[256];
	int path_length = snprintf(path, sizeof(path), "test/fixtures/bench/%s.%s", name, extension);

	if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
		fprintf(stderr, "The fixture path is too long\n");
		exit(1);
	}

	return read_file(path, length);
}

// MARK: Libraries

/*
A library that reads a format into a tree of values, and writes it back. `write` is `nullptr` for a library without a writer. Reading returns `nullptr` and writing returns `false` on failure.
*/
typedef struct benchmarked_library {
	const char *name;
	const char *format;
	const char *extension;
	void *(*read)(const char *text, size_t length);
	bool (*write)(void *document);
	void (*free)(void *document);
} benchmarked_library;

static void *read_soml(const char *text, size_t length) {
	return soml_parse(text, length, nullptr, nullptr);
}

static bool write_soml(void *document) {
	soml_string text;
	return soml_write(document, &text, nullptr);
}

static void free_soml(void *document) {
	soml_document_free(document);
}

#ifdef BENCH_COMPARE
static void *read_yyjson(const char *text, size_t length) {
	return yyjson_read(text, length, 0);
}

static bool write_yyjson(void *document) {
	char *text = yyjson_write(document, YYJSON_WRITE_PRETTY, nullptr);
	bool is_success = text != nullptr;
	free(text);
	return is_success;
}

static void free_yyjson(void *document) {
	yyjson_doc_free(document);
}

static void *read_cjson(const char *text, size_t length) {
	return cJSON_ParseWithLength(text, length);
}

static bool write_cjson(void *document) {
	char *text = cJSON_Print(document);
	bool is_success = text != nullptr;
	free(text);
	return is_success;
}

static void free_cjson(void *document) {
	cJSON_Delete(document);
}

static void *read_tomlc17(const char *text, size_t length) {
	if (length > INT_MAX) {
		return nullptr;
	}

	toml_result_t *result = malloc(sizeof(*result));

	if (result == nullptr) {
		return nullptr;
	}

	*result = toml_parse(text, (int)length);

	if (!result->ok) {
		fprintf(stderr, "tomlc17: %s\n", result->errmsg);
		toml_free(*result);
		free(result);
		return nullptr;
	}

	return result;
}

static void free_tomlc17(void *document) {
	toml_result_t *result = document;
	toml_free(*result);
	free(result);
}
#endif

static const benchmarked_library libraries[] = {
	{"soml.h", "SOML", "soml", read_soml, write_soml, free_soml},
#ifdef BENCH_COMPARE
	{"yyjson", "JSON", "json", read_yyjson, write_yyjson, free_yyjson},
	{"cJSON", "JSON", "json", read_cjson, write_cjson, free_cjson},
	{"tomlc17", "TOML", "toml", read_tomlc17, nullptr, free_tomlc17},
#endif
};

constexpr size_t library_count = sizeof(libraries) / sizeof(libraries[0]);

// MARK: Realistic documents

/*
The fastest time of the selected rounds, each of which reads, or reads once and writes, the document enough times to process the selected byte budget, because one call is too short to time alone. The fastest round is the one that other work on the machine slowed the least.
*/
static double seconds_per_document(const benchmarked_library *library, const char *text, size_t length, bool is_writing) {
	size_t count = target_bytes / length + 1;
	double fastest = 0.0;

	for (size_t round = 0; round < round_count; round++) {
		void *written = is_writing ? library->read(text, length) : nullptr;

		if (is_writing && written == nullptr) {
			fprintf(stderr, "%s could not read the document\n", library->name);
			exit(1);
		}

		double start = now();

		for (size_t index = 0; index < count; index++) {
			bool is_success;
			void *document = written;

			if (is_writing) {
				is_success = library->write(written);
			} else {
				document = library->read(text, length);
				is_success = document != nullptr;
			}

			if (!is_success) {
				fprintf(stderr, "%s could not %s the document\n", library->name, is_writing ? "write" : "read");
				exit(1);
			}

			if (!is_writing) {
				library->free(document);
			}
		}

		double seconds = (now() - start) / (double)count;
		fastest = round == 0 || seconds < fastest ? seconds : fastest;

		if (is_writing) {
			library->free(written);
		}
	}

	return fastest;
}

static void measure_document(const char *name, const char *description) {
	double seconds[2][library_count];

	for (size_t index = 0; index < library_count; index++) {
		size_t length;
		char *text = read_fixture(name, libraries[index].extension, &length);
		seconds[0][index] = seconds_per_document(&libraries[index], text, length, false);
		seconds[1][index] = libraries[index].write == nullptr ? 0.0 : seconds_per_document(&libraries[index], text, length, true);
		free(text);
	}

	for (int is_writing = 0; is_writing < 2; is_writing++) {
		// One unit for the row, so that its times compare at a glance.
		double slowest = 0.0;

		for (size_t index = 0; index < library_count; index++) {
			slowest = seconds[is_writing][index] > slowest ? seconds[is_writing][index] : slowest;
		}

		bool is_milliseconds = slowest >= 1e-3;
		printf("| %s %s |", is_writing ? "Write" : "Read", description);

		for (size_t index = 0; index < library_count; index++) {
			if (is_writing && libraries[index].write == nullptr) {
				printf(" – |");
			} else {
				printf(is_milliseconds ? " %.2f ms |" : " %.1f µs |", seconds[is_writing][index] * (is_milliseconds ? 1e3 : 1e6));
			}
		}

		printf("\n");
	}
}

// MARK: One kind of value

static void measure_values(const char *name, const char *item, size_t count) {
	size_t item_length = strlen(item);
	size_t capacity;
	char *text = ckd_add(&capacity, item_length, 1) || ckd_mul(&capacity, capacity, count) || ckd_add(&capacity, capacity, 2) ? nullptr : malloc(capacity);

	if (text == nullptr) {
		fprintf(stderr, "Could not allocate the %s benchmark\n", name);
		exit(1);
	}

	size_t used = 0;
	text[used++] = '[';

	for (size_t index = 0; index < count; index++) {
		memcpy(text + used, item, item_length);
		used += item_length;
		// A comma after the last item is allowed too.
		text[used++] = ',';
	}

	text[used++] = ']';
	double start = now();
	soml_error error;
	soml_document *document = soml_parse(text, used, nullptr, &error);
	double parsed = now();
	soml_string output;

	if (document == nullptr || !soml_write(document, &output, &error)) {
		fprintf(stderr, "%s: %s\n", name, error.message);
		exit(1);
	}

	double written = now();
	printf("| %s | %.0f MB/s | %.0f ns | %.0f ns |\n", name, (double)used / 1e6 / (parsed - start), (parsed - start) * 1e9 / (double)count, (written - parsed) * 1e9 / (double)count);
	soml_document_free(document);
	free(text);
}

// MARK: Memory and writing strategies

typedef enum writer_mode {
	WRITE_RETAINED,
	WRITE_CALLBACK,
	WRITE_BUFFER,
	WRITE_CANONICAL_RETAINED,
	WRITE_CANONICAL_CALLBACK,
	WRITE_CANONICAL_BUFFER,
} writer_mode;

static bool count_output(void *context, const char *bytes, size_t length) {
	(void)bytes;
	size_t *count = context;
	return !ckd_add(count, *count, length);
}

static bool write_mode(soml_document *document, writer_mode mode, char *buffer, size_t capacity) {
	size_t length = 0;
	soml_string output;
	bool is_success;

	switch (mode) {
		case WRITE_RETAINED: {
			is_success = soml_write(document, &output, nullptr);
			length = is_success ? output.length : 0;
			break;
		}

		case WRITE_CALLBACK: {
			is_success = soml_write_with_callback(document, count_output, &length, nullptr);
			break;
		}

		case WRITE_BUFFER: {
			is_success = soml_write_to_buffer(document, buffer, capacity, &length, nullptr);
			break;
		}

		case WRITE_CANONICAL_RETAINED: {
			is_success = soml_write_canonical(document, &output, nullptr);
			length = is_success ? output.length : 0;
			break;
		}

		case WRITE_CANONICAL_CALLBACK: {
			is_success = soml_write_canonical_with_callback(document, count_output, &length, nullptr);
			break;
		}

		case WRITE_CANONICAL_BUFFER: {
			is_success = soml_write_canonical_to_buffer(document, buffer, capacity, &length, nullptr);
			break;
		}

		default: {
			return false;
		}
	}

	return is_success && length == capacity - 1;
}

static void measure_storage_and_writers(const char *name) {
	size_t input_length;
	char *text = read_fixture(name, "soml", &input_length);
	soml_error error;
	soml_document *document = soml_parse(text, input_length, nullptr, &error);

	if (document == nullptr) {
		fprintf(stderr, "%s: %s\n", name, error.message);
		exit(1);
	}

	size_t document_bytes = soml_document_allocated_bytes(document);
	soml_string output;

	if (!soml_write(document, &output, &error)) {
		fprintf(stderr, "%s: %s\n", name, error.message);
		exit(1);
	}

	size_t capacity;

	if (ckd_add(&capacity, output.length, 1)) {
		fprintf(stderr, "The output length overflows\n");
		exit(1);
	}

	printf("| %s | %zu | %zu | %zu | %zu |\n", name, input_length, document_bytes, output.length, soml_document_allocated_bytes(document) - document_bytes);
	soml_document_free(document);

	static const char *const names[] = {"Retained", "Callback", "Caller buffer", "Canonical retained", "Canonical callback", "Canonical caller buffer"};
	double seconds[sizeof(names) / sizeof(names[0])];
	size_t count = target_bytes / input_length + 1;
	char *buffer = malloc(capacity);

	if (buffer == nullptr) {
		fprintf(stderr, "Could not allocate the writer benchmark\n");
		exit(1);
	}

	for (size_t mode = 0; mode < sizeof(names) / sizeof(names[0]); mode++) {
		double fastest = 0.0;

		for (size_t round = 0; round < round_count; round++) {
			document = soml_parse(text, input_length, nullptr, &error);

			if (document == nullptr) {
				fprintf(stderr, "%s: %s\n", name, error.message);
				exit(1);
			}

			double start = now();

			for (size_t index = 0; index < count; index++) {
				if (!write_mode(document, (writer_mode)mode, buffer, capacity)) {
					fprintf(stderr, "%s: %s writing failed\n", name, names[mode]);
					exit(1);
				}
			}

			double elapsed = (now() - start) / (double)count;
			fastest = round == 0 || elapsed < fastest ? elapsed : fastest;
			soml_document_free(document);
		}

		seconds[mode] = fastest;
	}

	printf("\n| %s writing | Time per document |\n|---|---|\n", name);

	for (size_t mode = 0; mode < sizeof(names) / sizeof(names[0]); mode++) {
		printf("| %s | %.1f µs |\n", names[mode], seconds[mode] * 1e6);
	}

	double fastest_clone = 0.0;

	for (size_t round = 0; round < round_count; round++) {
		document = soml_parse(text, input_length, nullptr, &error);

		if (document == nullptr) {
			fprintf(stderr, "%s: %s\n", name, error.message);
			exit(1);
		}

		double start = now();

		for (size_t index = 0; index < count; index++) {
			soml_document *copy = soml_document_clone(document, nullptr, &error);

			if (copy == nullptr) {
				fprintf(stderr, "%s: %s\n", name, error.message);
				exit(1);
			}

			soml_document_free(copy);
		}

		double elapsed = (now() - start) / (double)count;
		fastest_clone = round == 0 || elapsed < fastest_clone ? elapsed : fastest_clone;
		soml_document_free(document);
	}

	printf("| Deep clone and free | %.1f µs |\n", fastest_clone * 1e6);
	free(buffer);
	free(text);
}

// MARK: Builder and lookup

static soml_document *build_values(bool is_object, bool is_reserved, size_t count) {
	soml_document *document = soml_document_create(nullptr);
	soml_value *root = is_object ? soml_new_object(document) : soml_new_array(document);
	bool is_success = soml_document_set_root(document, root);

	if (is_success && is_reserved) {
		is_success = is_object ? soml_object_reserve(document, root, count) : soml_array_reserve(document, root, count);
	}

	for (size_t index = 0; is_success && index < count; index++) {
		soml_value *value = soml_new_int(document, (int64_t)index);

		if (is_object) {
			// Reverse insertion order forces a linear lookup until an index is built.
			char key[32];
			int length = snprintf(key, sizeof(key), "key%08zu", count - index - 1);
			is_success = length > 0 && (size_t)length < sizeof(key) && soml_object_add(document, root, key, (size_t)length, value);
		} else {
			is_success = soml_array_append(document, root, value);
		}
	}

	if (!is_success) {
		soml_document_free(document);
		fprintf(stderr, "Building the benchmark document failed\n");
		exit(1);
	}

	return document;
}

static void measure_builders(size_t count) {
	printf("\n| Build %zu values and free | Time | Retained bytes |\n|---|---|---|\n", count);
	size_t repetitions = target_bytes / count / sizeof(soml_value) + 1;

	for (int is_object = 0; is_object < 2; is_object++) {
		for (int is_reserved = 0; is_reserved < 2; is_reserved++) {
			double fastest = 0.0;
			soml_document *sample = build_values(is_object != 0, is_reserved != 0, count);
			size_t allocated_bytes = soml_document_allocated_bytes(sample);
			soml_document_free(sample);

			for (size_t round = 0; round < round_count; round++) {
				double start = now();

				for (size_t index = 0; index < repetitions; index++) {
					soml_document *document = build_values(is_object != 0, is_reserved != 0, count);
					soml_document_free(document);
				}

				double elapsed = (now() - start) / (double)repetitions;
				fastest = round == 0 || elapsed < fastest ? elapsed : fastest;
			}

			printf("| %s, %s | %.1f µs | %zu |\n", is_object ? "Object" : "Array", is_reserved ? "reserved" : "growing", fastest * 1e6, allocated_bytes);
		}
	}
}

static void measure_lookup(size_t count) {
	soml_document *document = build_values(true, true, count);
	soml_value *root = soml_document_root(document);
	// Borrowed keys avoid timing key formatting, and varied targets prevent the compiler from hoisting a single lookup out of the loop.
	soml_object_view members = soml_object_members(root);
	size_t repetitions = target_bytes / count + 1;
	printf("\n| Lookup in %zu reverse-inserted keys | Time | Retained index allocation |\n|---|---|---|\n", count);

	for (int is_indexed = 0; is_indexed < 2; is_indexed++) {
		size_t allocated_before = soml_document_allocated_bytes(document);

		if (is_indexed && !soml_object_index(document, root)) {
			fprintf(stderr, "Building the lookup index failed\n");
			exit(1);
		}

		members = soml_object_members(root);
		size_t index_bytes = soml_document_allocated_bytes(document) - allocated_before;
		double fastest = 0.0;

		for (size_t round = 0; round < round_count; round++) {
			double start = now();

			for (size_t index = 0; index < repetitions; index++) {
				size_t member_index = index * 7919 % count;
				soml_string key = members.data[member_index].key;
				int64_t value;

				if (!soml_get_int(soml_object_get_with_length(root, key.data, key.length), &value) || value != (int64_t)member_index) {
					fprintf(stderr, "Looking up the benchmark key failed\n");
					exit(1);
				}
			}

			double elapsed = (now() - start) / (double)repetitions;
			fastest = round == 0 || elapsed < fastest ? elapsed : fastest;
		}

		printf("| %s | %.0f ns | %zu |\n", is_indexed ? "Indexed" : "Linear", fastest * 1e9, index_bytes);
	}

	soml_document_free(document);
}

int main(int argument_count, char **arguments) {
	bool is_quick = argument_count == 2 && strcmp(arguments[1], "--quick") == 0;

	if (argument_count > 1 && !is_quick) {
		fprintf(stderr, "Usage: %s [--quick]\n", arguments[0]);
		return 2;
	}

	if (is_quick) {
		round_count = 2;
		target_bytes = 250'000;
	}

	size_t value_count = is_quick ? 10'000 : 1'000'000;
	size_t slow_value_count = is_quick ? 1'000 : 100'000;
	size_t builder_count = is_quick ? 1'000 : 10'000;
	printf("CPU time; %zu rounds; %zu bytes per batch%s.\n\n", round_count, target_bytes, is_quick ? "; quick mode" : "");
	printf("| Same data |");

	for (size_t index = 0; index < library_count; index++) {
		printf(" %s, %s |", libraries[index].name, libraries[index].format);
	}

	printf("\n|---|");

	for (size_t index = 0; index < library_count; index++) {
		printf("---|");
	}

	printf("\n");
	measure_document("service", "`service` (3 KB)");
	measure_document("platform", "`platform` (35 KB)");
	measure_document("earthquakes", "`earthquakes` (733 KB)");
	printf("\n| Values | Read | Read per value | Write per value |\n|---|---|---|---|\n");
	measure_values("Ints, `123456`", "123456", value_count);
	measure_values("Floats, `0.123456789`", "0.123456789", value_count);
	measure_values("Strings, `'hello world'`", "'hello world'", value_count);
	measure_values("Instants, `2026-09-19T14:00:00.5+07:00`", "2026-09-19T14:00:00.5+07:00", value_count);
	measure_values("Durations, `1h30m15.5s`", "1h30m15.5s", value_count);
	measure_values("Objects, `{name: 'a', port: 8080}`", "{name: 'a', port: 8080}", value_count);
	measure_values("Largest float, `1.7976931348623157e308`", "1.7976931348623157e308", slow_value_count);
	measure_values("Smallest float, `5e-324`", "5e-324", slow_value_count);
	printf("\nRetained memory in bytes; excludes allocator overhead and transient scratch.\n\n| Fixture | Input | Parsed document | Output | Retained output allocation |\n|---|---|---|---|---|\n");
	measure_storage_and_writers("service");
	printf("\n| Fixture | Input | Parsed document | Output | Retained output allocation |\n|---|---|---|---|---|\n");
	measure_storage_and_writers("platform");
	printf("\n| Fixture | Input | Parsed document | Output | Retained output allocation |\n|---|---|---|---|---|\n");
	measure_storage_and_writers("earthquakes");
	measure_builders(builder_count);
	measure_lookup(builder_count);
	return 0;
}
