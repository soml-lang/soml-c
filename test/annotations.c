/*
Misuses the API on purpose, to check that the annotations in the declarations let the compiler or its static analyzer report each misuse. `make annotations` compiles it with GCC's warnings and analyzer, and with Clang's analyzer, and checks the reports against the list in the Makefile. It is never run.

Each function holds one misuse, and `correct` holds none, so that a report there is a false warning.
*/
#include "../soml.h"

#include <stdlib.h>

void ignore_query_results(const soml_value *value, const soml_document *document);
size_t correct(const char *key);
size_t leak(void);
void double_free(void);
size_t use_after_free(void);
void free_with_free(void);
soml_document *parse_past_the_end(void);
soml_value *string_past_the_end(soml_document *document);
soml_value *key_past_the_end(soml_document *document, soml_value *object);
soml_value *lookup_past_the_end(const soml_value *object);
soml_document *parse_nullptr_with_length(void);
soml_value *lookup_unterminated(const soml_value *object);

size_t correct(const char *key) {
	soml_document *document = soml_parse("a: 1", 4, nullptr, nullptr);
	size_t count = soml_count(soml_object_get(soml_document_root(document), key));
	soml_document_free(document);
	soml_document *built = soml_document_create(nullptr);
	soml_value *root = soml_new_object(built);
	soml_object_add(built, root, "a", 1, soml_new_string(built, "b", 1));
	count += soml_count(soml_object_get_with_length(root, "a", 1));
	soml_document_free(built);
	soml_document_free(soml_parse(nullptr, 0, nullptr, nullptr));
	return count;
}

size_t leak(void) {
	soml_document *document = soml_document_create(nullptr);
	return soml_count(soml_document_root(document));
}

void double_free(void) {
	soml_document *document = soml_document_create(nullptr);
	soml_document_free(document);
	soml_document_free(document);
}

size_t use_after_free(void) {
	soml_document *document = soml_document_create(nullptr);
	soml_document_free(document);
	return soml_count(soml_document_root(document));
}

void free_with_free(void) {
	soml_document *document = soml_document_create(nullptr);
	free(document);
}

soml_document *parse_past_the_end(void) {
	return soml_parse("a: 1", 10, nullptr, nullptr);
}

soml_value *string_past_the_end(soml_document *document) {
	return soml_new_string(document, "abc", 8);
}

soml_value *key_past_the_end(soml_document *document, soml_value *object) {
	return soml_object_add(document, object, "a", 4, soml_new_null(document)) ? object : nullptr;
}

soml_value *lookup_past_the_end(const soml_value *object) {
	return soml_object_get_with_length(object, "a", 6);
}

soml_document *parse_nullptr_with_length(void) {
	return soml_parse(nullptr, 5, nullptr, nullptr);
}

soml_value *lookup_unterminated(const soml_value *object) {
	const char key[2] = {'a', 'b'};
	return soml_object_get(object, key);
}

void ignore_query_results(const soml_value *value, const soml_document *document) {
	soml_string string;
	int64_t integer;
	double number;
	bool boolean;
	soml_instant instant;
	soml_document_root(document);
	soml_type_of(value);
	soml_get_string(value, &string);
	soml_get_int(value, &integer);
	soml_get_float(value, &number);
	soml_get_bool(value, &boolean);
	soml_get_instant(value, &instant);
	soml_get_duration(value, &integer);
	soml_is_null(value);
	soml_count(value);
	soml_array_at(value, 0);
	soml_object_get(value, "a");
	soml_object_get_with_length(value, "a", 1);
	soml_object_key_at(value, 0);
	soml_object_value_at(value, 0);
}
