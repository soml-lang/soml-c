/*
Checks that the declarations compile as a client with Clang's -fbounds-safety, that valid calls succeed, and that invalid lengths and accesses trap. Run it with `make bounds-safety`.

A client declares handles as `__single`, because a local pointer to an incomplete type cannot have bounds.
*/
#include <ptrcheck.h>
#include "../soml.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_port(const char *__counted_by(length) text, size_t length) {
	soml_error error;
	soml_document *__single document = soml_parse(text, length, nullptr, &error);
	soml_value *__single port = soml_object_get(soml_document_root(document), "port");
	int64_t value = 0;
	bool has_port = soml_get_int(port, &value);
	soml_string key = soml_object_key_at(soml_document_root(document), 0);
	char first = key.length > 0 ? key.data[0] : '\0';
	soml_document_free(document);
	return has_port && first != '\0' ? (int)value : -1;
}

/*
A string and a key are followed by a NUL, which the documentation promises, so reading it must not trap.
*/
static bool has_terminators(void) {
	const char text[] = "name: 'api'";
	soml_document *__single document = soml_parse(text, sizeof(text) - 1, nullptr, nullptr);
	soml_string name = {};
	bool is_terminated = soml_get_string(soml_object_get(soml_document_root(document), "name"), &name) && name.data[name.length] == '\0';
	soml_string key = soml_object_key_at(soml_document_root(document), 0);
	is_terminated = is_terminated && key.data[key.length] == '\0';

	// A missing key has no data, which is still a valid string.
	soml_string missing = soml_object_key_at(soml_document_root(document), 1);
	is_terminated = is_terminated && missing.data == nullptr && missing.length == 0;
	soml_document_free(document);
	return is_terminated;
}

static bool has_views_and_buffer(void) {
	const char text[] = "items: [1,2]\nname: 'api'";
	soml_document *__single document = soml_parse(text, sizeof(text) - 1, nullptr, nullptr);
	soml_value *__single root = soml_document_root(document);
	soml_object_view members = soml_object_members(root);
	soml_array_view items = soml_array_items(soml_object_get(root, "items"));
	int64_t last = 0;
	bool is_valid = members.count == 2 && members.data[1].key.length == 4 && items.count == 2 && soml_get_int(items.data[1], &last) && last == 2;
	char output[128];
	size_t length = 0;
	is_valid = is_valid && soml_write_canonical_to_buffer(document, output, sizeof(output), &length, nullptr) && output[length] == '\0';
	soml_document *__single copy = soml_document_clone(document, nullptr, nullptr);
	is_valid = is_valid && copy != nullptr && soml_validate(copy, nullptr);
	soml_document_free(copy);
	soml_document_free(document);
	return is_valid;
}

static bool overread_callback(void *context, const char *__counted_by(length) bytes, size_t length) {
	(void)context;
	volatile size_t index = length;
	volatile char byte = bytes[index];
	(void)byte;
	return true;
}

/*
Volatile lengths and indexes keep the invalid calls as runtime checks rather than compile-time errors. Each misuse runs in a child, so a trap cannot prevent checking the other cases.
*/
static void invalid_access(unsigned kind) {
	const char text[] = "name: 'api'";
	soml_document *__single document = soml_parse(text, sizeof(text) - 1, nullptr, nullptr);
	soml_value *__single root = soml_document_root(document);
	volatile size_t length = sizeof(text) + 1;
	soml_string string = {};

	switch (kind) {
		case 0: {
			soml_document_free(soml_parse(text, length, nullptr, nullptr));
			break;
		}
		case 1: {
			(void)soml_new_string(document, text, length);
			break;
		}
		case 2: {
			(void)soml_object_add(document, root, text, length, soml_new_null(document));
			break;
		}
		case 3: {
			(void)soml_object_get_with_length(root, text, length);
			break;
		}
		case 4:
		case 5: {
			if (kind == 4) {
				if (!soml_get_string(soml_object_get(root, "name"), &string)) {
					break;
				}
			} else {
				string = soml_object_key_at(root, 0);
			}

			volatile size_t index = string.length + 1;
			volatile char byte = string.data[index];
			(void)byte;
			break;
		}
		case 6: {
			(void)soml_write_with_callback(document, overread_callback, nullptr, nullptr);
			break;
		}
		case 7: {
			(void)soml_write_canonical_with_callback(document, overread_callback, nullptr, nullptr);
			break;
		}
		case 8: {
			soml_object_view members = soml_object_members(root);
			volatile size_t index = members.count;
			volatile size_t key_length = members.data[index].key.length;
			(void)key_length;
			break;
		}
		case 9: {
			const char array_text[] = "[1]";
			soml_document *__single array_document = soml_parse(array_text, sizeof(array_text) - 1, nullptr, nullptr);
			soml_array_view items = soml_array_items(soml_document_root(array_document));
			volatile size_t index = items.count;
			soml_value *__single item = items.data[index];
			(void)item;
			soml_document_free(array_document);
			break;
		}
		case 10: {
			char output[2];
			volatile size_t capacity = 128;
			size_t written = 0;
			(void)soml_write_to_buffer(document, output, capacity, &written, nullptr);
			break;
		}

		default: {
			break;
		}
	}

	soml_document_free(document);
}

static bool traps(unsigned kind) {
	pid_t child = fork();

	if (child < 0) {
		return false;
	}

	if (child == 0) {
		invalid_access(kind);
		_exit(0);
	}

	int status = 0;
	return waitpid(child, &status, 0) == child && WIFSIGNALED(status) && (WTERMSIG(status) == SIGILL || WTERMSIG(status) == SIGTRAP);
}

int main(void) {
	const char text[] = "port: 8080";

	if (read_port(text, sizeof(text) - 1) != 8080 || !has_terminators() || !has_views_and_buffer()) {
		return 1;
	}

	const char *names[] = {"parse length", "string length", "key length", "lookup length", "string overread", "key overread", "write callback", "canonical write callback", "object view overread", "array view overread", "output capacity"};
	bool passed = true;

	for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		if (!traps(index)) {
			fprintf(stderr, "%s did not trap\n", names[index]);
			passed = false;
		}
	}

	return passed ? 0 : 1;
}
