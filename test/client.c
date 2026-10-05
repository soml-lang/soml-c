/*
A client of the declarations, compiled as C99 and C++17 and linked to a separately compiled C23 implementation. Including the header twice also checks its declaration guard. Run it with `make clients`.
*/
#include "../soml.h"
#include "../soml.h"

static bool count_bytes(void *context, const char *bytes, size_t length) {
	(void)bytes;
	*(size_t *)context += length;
	return true;
}

int main(void) {
	const char text[] = "port: 80";
	soml_document *document = soml_parse(text, sizeof(text) - 1, NULL, NULL);
	int64_t port = 0;
	bool found = soml_get_int(soml_object_get(soml_document_root(document), "port"), &port);

	// Each writer, so that each one links.
	soml_string written;
	soml_string canonical;
	size_t length = 0;
	size_t canonical_length = 0;
	bool is_written = soml_write(document, &written, NULL) && soml_write_canonical(document, &canonical, NULL) && soml_write_with_callback(document, count_bytes, &length, NULL) && soml_write_canonical_with_callback(document, count_bytes, &canonical_length, NULL);
	is_written = is_written && length == written.length && canonical_length == canonical.length;

	soml_document_free(document);
	return found && port == 80 && is_written ? 0 : 1;
}
