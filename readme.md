# SOML for C

> A strict reader and writer for [SOML](https://github.com/soml-lang/soml), a config format for humans, in one C header

> [!NOTE]
> The format is a draft.

## Highlights

- One file, `soml.h`, with no dependencies past the C standard library: no libm, no stdio, and no locale
- Every rule in the specification is checked: duplicate keys, the int64 range, instant and duration ranges, raw control characters, invalid UTF-8, the nesting limit, and more
- Exact: an int is an `int64_t`, a float is read and written correctly rounded, and instants and durations keep every nanosecond
- Output that keeps your member order, and exact canonical form on request, where the same value always gives the same bytes
- Memory safe by design: every read is bounds-checked, every size computation is overflow-checked, nesting is limited so hostile input cannot overflow the stack, and parsing takes linear or n log n time on every input. AddressSanitizer builds poison unused arena space and the gaps between allocations
- Annotated for the compiler: GCC and Clang's static analyzer report a document that is leaked, freed twice, or used after it is freed, and GCC reports a length past the end of its buffer
- No global state, and an allocator you can replace at run time
- Passes the language-neutral [conformance suite](test/conformance) of over 900 cases, with the same error positions as the reference implementation, and is tested with sanitizers, fuzzing, and differential testing against the reference implementation

## Install

Copy [`soml.h`](soml.h) into your project. In exactly one C file, such as `soml.c`, define `SOML_IMPLEMENTATION` before you include it:

```c
#define SOML_IMPLEMENTATION
#include "soml.h"
```

Other files include it without the define. To keep the library private to one file, also define `SOML_STATIC` there. Each file that does gets its own copy.

The implementation needs C23: Clang 19 or later, or GCC 14 or later, with `-std=c23` and 128-bit `_BitInt` support on the target. Clang on Windows with the MSVC target does not link the compiler-rt builtins that 128-bit division needs, so link with `-rtlib=compiler-rt`. The declarations alone also work from C99 or C++17 or later, so the rest of a project can use another standard or C++. Compile the implementation as C23 and link it with the caller:

```sh
cc -std=c23 -O2 -c soml.c -o soml.o
cc -std=c99 -O2 -c main.c -o main.o
cc main.o soml.o -o app
```

For a C++17 caller, use `c++ -std=c++17` for its source and the final link. The examples below use C23. In a C99 caller, use `NULL` instead of `nullptr`.

## Usage

### Read

```c
#include <stdio.h>
#include "soml.h"

int main(void) {
	const char text[] =
		"# The edge service.\n"
		"name: 'api-gateway'\n"
		"replicas: 3\n"
		"timeout: 30s\n"
		"postgres: {host: 'db.internal', port: 5432}\n";

	soml_error error;
	soml_document *document = soml_parse(text, sizeof(text) - 1, nullptr, &error);

	if (document == nullptr) {
		fprintf(stderr, "%s at line %zu, column %zu\n", error.message, error.line, error.column);
		return 1;
	}

	soml_value *root = soml_document_root(document);
	soml_string name;
	int64_t port;
	int64_t timeout;

	if (soml_get_string(soml_object_get(root, "name"), &name)) {
		fwrite(name.data, 1, name.length, stdout);
		fputc('\n', stdout);
		//=> api-gateway
	}

	if (soml_get_int(soml_object_get(soml_object_get(root, "postgres"), "port"), &port)) {
		printf("%lld\n", (long long)port);
		//=> 5432
	}

	if (soml_get_duration(soml_object_get(root, "timeout"), &timeout)) {
		printf("%lld ns\n", (long long)timeout);
		//=> 30000000000 ns
	}

	soml_document_free(document);
	return 0;
}
```

The text does not need a NUL terminator, and it can be freed once `soml_parse` returns. The document owns every value, string, and key in it, and `soml_document_free` releases all of them at once.

Every accessor accepts `nullptr`, so lookups chain without checks in between. A getter returns `false`, and leaves its output unchanged, for a missing value, a value of another type, or a `nullptr` output. There is no conversion: an int is not a float, and a float is not an int.

`soml_object_get` takes a NUL-terminated key. For a key containing U+0000, use its byte length:

```c
const char key[] = {'a', '\0', 'b'};
soml_value *value = soml_object_get_with_length(root, key, sizeof(key));
```

Separate documents can be used on separate threads. Accessors and callback writers can read one document concurrently while it is not changed. Callback writers can allocate temporary memory to sort built objects, so the allocator must then be thread-safe, and each needs its own output context or a synchronized one. Builder functions, `soml_write`, and `soml_write_canonical` change the document and need exclusive access.

### Iterate object members

```c
soml_value *root = soml_document_root(document);

soml_object_view members = soml_object_members(root);

for (size_t index = 0; index < members.count; index++) {
	soml_member member = members.data[index];
	fwrite(member.key.data, 1, member.key.length, stdout);
	printf(": type %d\n", soml_type_of(member.value));
}
```

Members are in the order they appear in the text. `soml_array_items` gives the corresponding borrowed range for an array. A view expires after any builder call on its collection, and when its document is freed.

### Write

```c
soml_document *document = soml_document_create(nullptr);
soml_value *root = soml_new_object(document);
soml_value *labels = soml_new_array(document);

soml_object_add(document, root, "name", 4, soml_new_string(document, "api-gateway", 11));
soml_object_add(document, root, "replicas", 8, soml_new_int(document, 3));
soml_object_add(document, root, "ratio", 5, soml_new_float(document, 0.75));
soml_object_add(document, root, "labels", 6, labels);
soml_array_append(document, labels, soml_new_string(document, "prod", 4));
soml_document_set_root(document, root);

soml_string text;
soml_error error;

if (soml_write(document, &text, &error)) {
	fwrite(text.data, 1, text.length, stdout);
} else {
	fprintf(stderr, "%s\n", error.message);
}
//=> name: 'api-gateway'
//   replicas: 3
//   ratio: 0.75
//   labels: [
//   	'prod'
//   ]

soml_document_free(document);
```

Members are written in the order they were added, or, for a parsed document, in the order of the text. Everything else follows canonical form: one entry per line, tab indentation, canonical spellings, no commas, and no comments. Sorted keys would put `description` before `name`, which is noise to a person who reads the file.

For exact canonical form, with the members of every object sorted by key, use `soml_write_canonical`. Two documents with the same value then give the same bytes, for hashing, signing, or comparing:

```c
if (soml_write_canonical(document, &text, &error)) {
	fwrite(text.data, 1, text.length, stdout);
}
//=> labels: [
//   	'prod'
//   ]
//   name: 'api-gateway'
//   ratio: 0.75
//   replicas: 3
```

The text is owned by the document and stays valid until it is freed, also after later writes and builder calls. Each output buffer is kept without a final copy, so it can take nearly twice the text length. Use a callback writer when the output should not stay in memory. A failed write leaves the output unchanged, and allocation and callback failures can be retried.

A builder function that fails, because the allocator returned `nullptr` or it got an argument it cannot use, makes every writer fail with that error, even when you handled the failure, so a builder can be checked once, at the end. The writers also check every value, because a built value did not go through the parser: valid UTF-8, no carriage return, no NaN, instants in range, no duplicate keys, and the nesting limit.

To write to a file or a socket, `soml_write_with_callback` and `soml_write_canonical_with_callback` pass the text in pieces to a callback. The bytes are not guaranteed to be NUL-terminated and are valid only during that call. The callback must not change or free the document. A failure can occur after some pieces have been delivered, and the delivered text can itself be a valid, shorter document without the later members, for example when a later string holds a carriage return. So discard or roll back partial output when a complete document is required: to save a file, write to a temporary file and rename it only after success.

For a `FILE *`, check the final flush too:

```c
#include <stdio.h>

static bool write_file(void *context, const char *bytes, size_t length) {
	FILE *file = context;
	return fwrite(bytes, 1, length, file) == length;
}

// In a function, with document and error from the examples above:
if (!soml_write_with_callback(document, write_file, stdout, &error)) {
	fprintf(stderr, "%s\n", error.message);
} else if (fflush(stdout) == EOF) {
	perror("Writing SOML");
}
```

A parsed document can be changed with the builder functions and written again. Added members are written after the existing ones, in the order they were added.

A value can be in one place only: in one container, or the root, as in a tree. A value in several places would be written once for each, which grows exponentially with nesting, so the builder refuses a value that is already in a container. It also refuses a value from another document, because that document could be freed first. It does not check ancestors, so a value added to itself or to one of its own descendants makes a cycle. Nothing outside the cycle can reach it, so it is never written, but `soml_clone_value` of a value in it fails with `SOML_ERROR_TOO_DEEP`.

A new document has no root. Set an object or array, which can be empty, as its root before writing. Setting the current root again succeeds. Replacing the root detaches the previous one, so it can then be added to a container in the same document.

### Edit and copy

`soml_array_reserve` and `soml_object_reserve` reserve capacity without changing the count, so filling a large collection does not copy it into ever larger allocations.

| Operation | Array | Object |
|---|---|---|
| Add | `soml_array_append`, `soml_array_insert` | `soml_object_add` |
| Replace | `soml_array_set` | `soml_object_set` (adds when missing) |
| Remove | `soml_array_remove` | `soml_object_remove` (succeeds when missing) |
| Remove and return | `soml_array_pop` | `soml_object_take` |
| Empty and reuse capacity | `soml_array_clear` | `soml_object_clear` |

Object edits take a key and its byte length, so they support U+0000. Replacing a value keeps its member position and key. A removed or replaced value is detached and can be added elsewhere in the same document. Its memory stays with the document until it is freed. Setting a slot to its current value succeeds. Other rules are the same as for adding.

```c
soml_object_set(document, root, "replicas", 8, soml_new_int(document, 5));
soml_object_remove(document, root, "ratio", 5);
soml_value *label = soml_array_pop(document, labels);

if (label != nullptr) {
	soml_object_set(document, root, "label", 5, label);
}
```

Lookup uses binary search in an unchanged parsed object and in an object built with strictly sorted keys, and a linear scan otherwise. `soml_object_index` builds a lookup index for an object with unsorted keys, without changing member order. It checks for duplicate keys and records a builder failure when it fails. Replacement and removal keep the index, but adding a member can invalidate it. The document keeps each index until the document is freed, so index an object once after a batch of additions, not after each one, which would use quadratic memory. `soml_object_set`, `soml_object_remove`, and `soml_object_take` find the key as lookup does, so each one is a linear scan in an object with unsorted keys and no index. A removal also moves the members after it and rewrites any index, so it can take linear time in any object.

`soml_clone_value(destination, value)` copies a value and its descendants into the destination document, which can be the same document, and returns it unattached. `soml_document_clone(source, allocator, &error)` validates and copies a whole document. Copies own their strings and keys and keep member order and lookup indexes, so the source can be freed right after. A failure leaves the source unchanged. A failed value clone records a builder failure in the destination, which keeps any partial copy until it is freed.

### Validate and write without retaining output

`soml_validate(document, &error)` checks representability, duplicate keys, nesting, the root, and any persistent builder failure without formatting output. It can use temporary memory for unsorted object keys. `soml_document_get_error(document, &error)` reports only the first persistent builder failure, and leaves `error` unchanged when there is none. Validation and writer errors can be retried.

`soml_measure(document, &length, &error)` gives the output length in bytes, which is the same for regular and canonical form. `soml_write_to_buffer` and `soml_write_canonical_to_buffer` write into caller-owned storage:

```c
#include <stdlib.h>

// In a function, with document and error:
size_t length;

if (soml_measure(document, &length, &error) && length < SIZE_MAX) {
	char *buffer = malloc(length + 1);

	if (buffer != nullptr) {
		if (soml_write_canonical_to_buffer(document, buffer, length + 1, &length, &error)) {
			fwrite(buffer, 1, length, stdout);
		}

		free(buffer);
	}
}
```

The capacity includes the final NUL, and the returned length excludes it. A buffer that is too small gives `SOML_ERROR_WRITE`. On any failure, the length is unchanged, but the buffer can hold a partial prefix. The buffer must not overlap document storage, and the document must not change between measuring and writing. Neither function keeps output or changes the document, but sorting still uses its allocator.

`soml_document_allocated_bytes` reports the memory a document retains: the document, its arena blocks, and stored outputs, but not allocator overhead or temporary memory. Reserving, removing, and clearing do not release memory, so clone a much smaller edited document to release the old storage.

### Allocator

Pass an allocator to `soml_parse` or `soml_document_create`, or `nullptr` for `malloc`, `realloc`, and `free`:

```c
#include <stdlib.h>

static void *reallocate(void *context, void *pointer, size_t old_size, size_t new_size) {
	(void)context;
	(void)old_size;

	if (new_size == 0) {
		free(pointer);
		return nullptr;
	}

	return realloc(pointer, new_size);
}

soml_allocator allocator = {.reallocate = reallocate, .context = nullptr};
soml_document *document = soml_parse(text, length, &allocator, &error);
```

A document allocates its memory in large blocks and frees all of them at once. It copies the allocator struct, but the callback and context must stay valid until the document is freed. The callback gets `nullptr` and an old size of 0 for an allocation, and a new size of 0 for a free, which must release the block and return `nullptr`. As with `malloc` and `realloc`, memory must be aligned for `max_align_t`, and a failed reallocation must leave the original block unchanged. A `nullptr` callback uses the default allocator.

## Types

| SOML | C |
|---|---|
| string | `soml_string`: `data` and `length`, in UTF-8. The data is followed by a NUL, but a string can also hold U+0000, so use `length`. |
| int | `int64_t` |
| float | `double`, finite or infinite |
| bool | `bool` |
| null | `soml_is_null()` |
| instant | `soml_instant`: `seconds` since 1970-01-01T00:00:00Z and `nanoseconds` |
| duration | `int64_t` nanoseconds |
| array | `soml_count()` and `soml_array_at()` |
| object | `soml_count()`, `soml_object_get()`, `soml_object_key_at()`, and `soml_object_value_at()` |

Float conversion rounds to nearest, with ties to even, independently of the thread's rounding and subnormal modes. Negative zero is stored as zero, and NaN is rejected.

## Errors

`soml_parse` returns `nullptr` and fills in a `soml_error`:

- `code`: `SOML_ERROR_SYNTAX`, `SOML_ERROR_TOO_DEEP`, `SOML_ERROR_OUT_OF_MEMORY`, or `SOML_ERROR_INVALID_ARGUMENT`. Writing can also give `SOML_ERROR_INVALID_VALUE` and `SOML_ERROR_WRITE`.
- `message`: what is wrong, such as `Duplicate key`. It is a static string.
- `offset`: the byte offset in the text.
- `line` and `column`: 1-based, with the column counted in Unicode scalar values.

Syntax and depth errors from parsing always have a text position, and allocation errors during parsing can have one. Other errors have `offset`, `line`, and `column` set to 0, so show a location only when `error.line` is nonzero. Pass `nullptr` for `error` to discard the details.

The position is the same as the reference implementation's for every invalid case in the conformance suite. When a document has more than one error, the first one in the text is reported, except that characters that are errors anywhere are checked in the whole text first: invalid UTF-8, then a byte order mark, then a raw control character. The rule holds for a duplicate key too, which is found when its object closes, or when an error stops the object before it closes.

## Untrusted input

Reading is safe for hostile input: every read is checked against the text's length, nesting is limited to 100 levels, time grows linearly or as n log n with the input, and memory linearly. Stack usage depends on the compiler and build flags: the deepest documents are tested on a 64 KB stack in native builds without sanitizers, and sanitizer instrumentation can require more. Three costs matter for a service that reads untrusted documents:

- **Floats near the ends of the range are slow.** Floats outside the arithmetic and bounded 128-bit integer fast paths, such as `5e-324` or `1.7976931348623157e308`, are converted exactly with a high-precision decimal, which takes 5 to 30 µs to read or write. A document of a million such floats, 7 MB, takes about 30 seconds to read and write. Typical floats take a fast path.
- **Memory can exceed 35 times the size of the text** for hostile shapes, such as dense arrays of small values, including temporary parsing buffers. A typical document of records takes 8 to 15 times its size, and every parsed document takes at least 4 KB, the first block of its memory.
- **The written text can be about 50 times the size of the text it was read from**, because each level of nesting is two characters in the text but indented lines in the output, and `soml_write` retains an output buffer with up to nearly twice the output length, plus its 4 KB callback buffer. To write canonical form of untrusted text, as for a hash, use `soml_write_canonical_with_callback` with a callback that returns `false` past a budget.

Limit the size of untrusted text before reading it, or pass an allocator that refuses to allocate past a budget, which makes `soml_parse` fail with `SOML_ERROR_OUT_OF_MEMORY`.

This allocator limits the live bytes the library requests, including parsing and writing buffers, but not the system allocator's overhead or the space `realloc` can use while it moves a block:

```c
#include <stdlib.h>

typedef struct memory_budget {
	size_t used;
	size_t limit;
} memory_budget;

static void *budget_reallocate(void *context, void *pointer, size_t old_size, size_t new_size) {
	memory_budget *budget = context;

	if (new_size == 0) {
		free(pointer);
		budget->used -= old_size;
		return nullptr;
	}

	size_t retained = budget->used - old_size;

	if (new_size > budget->limit - retained) {
		return nullptr;
	}

	void *resized = realloc(pointer, new_size);

	if (resized != nullptr) {
		budget->used = retained + new_size;
	}

	return resized;
}

// In a function, with text, length, and error:
memory_budget budget = {.limit = 1024 * 1024};
soml_allocator allocator = {.reallocate = budget_reallocate, .context = &budget};
soml_document *document = soml_parse(text, length, &allocator, &error);
// Keep budget alive until this call:
soml_document_free(document);
```

This budget is for one thread. Shared callback writers need a thread-safe allocator.

## Performance

The time to read each of the [shared benchmark fixtures](https://github.com/soml-lang/soml/tree/main/benchmark) into a tree of values, and to write it back, compared with the most used JSON and TOML libraries in C, on the same data in their format. Apple Silicon (M-series), Apple Clang with `-O2`, `make bench-compare`, in CPU time.

| Same data | soml.h, SOML | [yyjson](https://github.com/ibireme/yyjson), JSON | [cJSON](https://github.com/DaveGamble/cJSON), JSON | [tomlc17](https://github.com/cktan/tomlc17), TOML |
|---|---|---|---|---|
| Read `service` (3 KB) | 6.7 µs | 1.3 µs | 6.5 µs | 16.6 µs |
| Write `service` | 3.1 µs | 0.9 µs | 7.5 µs | – |
| Read `platform` (35 KB) | 46.8 µs | 8.9 µs | 45.2 µs | 124.9 µs |
| Write `platform` | 20.1 µs | 6.6 µs | 34.3 µs | – |
| Read `earthquakes` (733 KB) | 1.96 ms | 0.37 ms | 2.04 ms | 4.38 ms |
| Write `earthquakes` | 0.91 ms | 0.25 ms | 3.09 ms | – |

yyjson is the fastest of the widely used JSON libraries in C. tomlc17 has no writer.

## Limitations

- **The implementation needs C23**, so MSVC cannot build it. Clang on Windows can.
- **Nesting is limited to 100 levels**, as the specification requires. The top-level object counts as a level with or without its braces.
- **`-fbounds-safety`**: the declarations carry bounds annotations, so a caller that uses Clang's `-fbounds-safety` gets its buffers checked. A caller declares handles as `soml_document *__single`, because a local pointer to an incomplete type cannot have bounds. Apple clang 21 gives a false `-Wnull-pointer-subtraction` warning in its own bounds check for `nullptr` with a length of 0, as in `soml_parse(nullptr, 0, …)`, so pass `""` there instead. The implementation does not support the flag, so build the file that defines `SOML_IMPLEMENTATION` without it.
- **`-ffast-math`**: the arithmetic float parsing fast path is disabled. Exact integer conversion remains active, with high-precision conversion for values outside its range.
- `soml_object_add` does not check for a duplicate key, because a check on every add would make building a large object quadratic. `soml_write` reports it. Lookup in an object whose keys were not added in sorted order is a linear scan until `soml_object_index` builds an index. `soml_object_set` finds the key the same way, so setting many keys in unsorted order is quadratic.
- There is no formatter that keeps comments. The writer writes a value, so it keeps member order but not comments or spellings.

## Development

```sh
make test           # Unit tests, conformance suite and tool checks (need Node.js), and C99/C++17 clients
make clients        # C99/C++17 declaration and linkage checks (needs a C++ compiler)
make sanitize       # Address and undefined behavior, plus integer conversions, unsigned arithmetic and shifts, nullability, local bounds, and stack use-after-return on Clang
make thread-sanitize # Data races in concurrent accessors and callback writers (needs pthreads and ThreadSanitizer)
make fast-math      # The same with -ffast-math, including NaN rejection
make fuzz           # libFuzzer, for reading and building, with and without allocation failure (Homebrew's LLVM on macOS)
make wasm32         # The tests as a 32-bit program, in WebAssembly with wasmtime
make differential   # Differential testing against ../soml-javascript (needs Node.js 26 or later, set with NODE=)
make bounds-safety  # A -fbounds-safety client: valid calls succeed and invalid lengths and accesses trap
make annotations    # The compiler reports each misuse in test/annotations.c (needs GCC 14 and Clang 21 or later)
make analyze        # GCC and Clang analyze the implementation; any finding fails (needs both compilers)
make bench          # Reading and writing speed
make bench-compare  # The same, and yyjson, cJSON, and tomlc17 on the same data (downloads them)
```

For faster iteration, run `make build/bench` followed by `build/bench --quick`. It uses two rounds and smaller inputs, and also reports retained memory, writer strategies, preallocation, lookup indexing, and cloning.

## Conformance suite

[`test/conformance`](test/conformance) is a copy of the suite in the [`soml` spec repository](https://github.com/soml-lang/soml/tree/main/conformance):

- `valid/**/name.soml` must parse to the value in `name.json`, and serialize with `soml_write_canonical` to exactly `name.canonical.soml`.
- `valid/**/name.formatted.soml` is the expected output of a formatter. soml.h has no formatter, so it does not check these files.
- `invalid/**/name.soml` must be rejected, at the line and column in `test/conformance/invalid-reasons.json`.
- `edit/name.json` is a case for a document editor that sets or removes one value. The harness does not run these.

To update the copy, check out the spec repository next to this one and run `../soml/sync-conformance.sh test/conformance`.
