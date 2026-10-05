CC ?= cc
CXX ?= c++
SHARED_WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes -Wvla -Wimplicit-fallthrough -Wundef -Wdouble-promotion -Wwrite-strings -Wnull-dereference -Wformat=2 -Wbad-function-cast -Wredundant-decls -Wnested-externs -Walloca -Wswitch-enum -Wmissing-variable-declarations -Werror
# Warnings that only one compiler has, which the other one refuses as unknown.
CLANG_WARNINGS = -Wconditional-uninitialized -Wsometimes-uninitialized -Wshorten-64-to-32 -Wimplicit-int-float-conversion -Wcomma -Wassign-enum -Wloop-analysis -Wunreachable-code-aggressive -Wextra-semi-stmt
GCC_WARNINGS = -Wjump-misses-init -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuse-after-free=3
CC_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -i clang)
WARNINGS = $(SHARED_WARNINGS) $(if $(CC_IS_CLANG),$(CLANG_WARNINGS),$(GCC_WARNINGS))
CLIENT_WARNINGS = -Wall -Wextra -Wpedantic -Werror
CFLAGS ?= -O2 -g
COMPILE_FLAGS = $(CPPFLAGS) $(STANDARD) $(CFLAGS) $(WARNINGS) $(EXTRA_CFLAGS)
SANITIZER_FLAGS = -fsanitize=address,undefined,float-divide-by-zero -fno-sanitize-recover=all
CLANG_SANITIZERS = -fsanitize=implicit-integer-conversion,unsigned-integer-overflow,unsigned-shift-base,nullability,local-bounds -fsanitize-address-use-after-return=always
SANITIZE = $(SANITIZER_FLAGS) $(if $(CC_IS_CLANG),$(CLANG_SANITIZERS)) -fno-omit-frame-pointer
STANDARD = -std=c23

# Each set of command-line flags needs its own build folder, because Make only tracks source and Makefile changes.
BUILD ?= build

# The Node.js that runs the tests in JavaScript. The differential test needs Node.js 26 or later, because the reference implementation uses Temporal, so set it as in `make differential NODE=/path/to/node26`.
NODE ?= node

.DELETE_ON_ERROR:

.PHONY: test unit conformance tool-test clients sanitize thread-sanitize fast-math bounds-safety annotations analyze wasm32 fuzz differential bench bench-compare clean

test: unit conformance tool-test clients

$(BUILD) $(BUILD)/wasm32:
	mkdir -p $@

$(BUILD)/soml-tagged: test/soml-tagged.c soml.h Makefile | $(BUILD)
	$(CC) $(COMPILE_FLAGS) $< $(LDFLAGS) -o $@ $(LDLIBS)

# The tests link libm for `fesetround`, which the library itself does not need.
$(BUILD)/test: test/test.c soml.h fuzz/allocator.h Makefile | $(BUILD)
	$(CC) $(COMPILE_FLAGS) $< $(LDFLAGS) -o $@ -lm $(LDLIBS)

unit: $(BUILD)/test
	$(BUILD)/test

conformance: $(BUILD)/soml-tagged
	SOML_TAGGED="$(BUILD)/soml-tagged" $(NODE) --test test/conformance.js

tool-test: $(BUILD)/soml-tagged
	SOML_TAGGED="$(BUILD)/soml-tagged" $(NODE) --test test/tool.js

# The implementation stays C23, while callers can use C99 or C++17.
$(BUILD)/soml.o: soml.h Makefile | $(BUILD)
	$(CC) $(COMPILE_FLAGS) -DSOML_IMPLEMENTATION -x c -c $< -o $@

$(BUILD)/client-c99: test/client.c soml.h $(BUILD)/soml.o Makefile
	$(CC) $(CPPFLAGS) -std=c99 $(CFLAGS) $(CLIENT_WARNINGS) $(EXTRA_CFLAGS) $< $(BUILD)/soml.o $(LDFLAGS) -o $@ $(LDLIBS)

$(BUILD)/client-cpp17: test/client.c soml.h $(BUILD)/soml.o Makefile
	$(CXX) $(CPPFLAGS) -std=c++17 $(CFLAGS) $(CLIENT_WARNINGS) $(EXTRA_CFLAGS) -x c++ $< -x none $(BUILD)/soml.o $(LDFLAGS) -o $@ $(LDLIBS)

clients: $(BUILD)/client-c99 $(BUILD)/client-cpp17
	$(BUILD)/client-c99
	$(BUILD)/client-cpp17

# The tests again with address and undefined-behavior checks, plus integer conversions, unsigned arithmetic and shifts, nullability, local bounds, and stack use-after-return on Clang.
sanitize:
	$(MAKE) test BUILD="$(BUILD)/sanitize" CFLAGS="-O1 -g" EXTRA_CFLAGS="$(EXTRA_CFLAGS) $(SANITIZE)"

# Data races in the shared-document accessors and callback writers, on platforms with pthreads and ThreadSanitizer.
thread-sanitize:
	$(MAKE) "$(BUILD)/thread-sanitize/test" BUILD="$(BUILD)/thread-sanitize" CFLAGS="-O1 -g" EXTRA_CFLAGS="$(EXTRA_CFLAGS) -fsanitize=thread -fno-sanitize-recover=all -fno-omit-frame-pointer -pthread"
	$(BUILD)/thread-sanitize/test --concurrent

# Float conversion and NaN rejection must also hold when the host project enables fast math.
fast-math:
	$(MAKE) test BUILD="$(BUILD)/fast-math" CFLAGS="$(CFLAGS) -ffast-math"

# The declarations as a client compiled with Clang's -fbounds-safety, which needs Apple Clang, linked to the implementation built without it and run.
$(BUILD)/bounds-safety: test/bounds-safety.c soml.h $(BUILD)/soml.o Makefile
	$(CC) $(COMPILE_FLAGS) -fbounds-safety $< $(BUILD)/soml.o $(LDFLAGS) -o $@ $(LDLIBS)

bounds-safety: $(BUILD)/bounds-safety
	$(BUILD)/bounds-safety

# The annotations in the declarations, checked by misusing the API on purpose in test/annotations.c: GCC's warnings and analyzer, and Clang's analyzer, must report exactly these kinds of problems, as many times as listed, which also catches a false report. It needs GCC 14 or later, such as gcc-16 from Homebrew, which ANNOTATIONS_GCC sets, and Clang 21 or later, which ANNOTATIONS_CLANG sets, because the analyzer of older versions tracks the ownership attributes only for `malloc`.
ANNOTATIONS_GCC ?= $(shell for compiler in gcc-16 gcc-15 gcc-14 gcc; do if command -v $$compiler >/dev/null; then echo $$compiler; break; fi; done)
ANNOTATIONS_CLANG ?= clang
ANNOTATIONS_UNUSED_RESULT_EXPECTED = unused-result unused-result unused-result unused-result unused-result unused-result unused-result unused-result unused-result unused-result unused-result unused-result unused-result unused-result unused-result
ANNOTATIONS_GCC_EXPECTED = analyzer-double-free analyzer-malloc-leak analyzer-mismatching-deallocation analyzer-out-of-bounds mismatched-dealloc nonnull stringop-overread stringop-overread stringop-overread stringop-overread $(ANNOTATIONS_UNUSED_RESULT_EXPECTED) use-after-free use-after-free
ANNOTATIONS_CLANG_EXPECTED = unix.Malloc unix.Malloc unix.Malloc unix.MismatchedDeallocator
# Runs the command in $(1), which must succeed, and checks that the kinds of diagnostics it reports, which the `sed` expression in $(2) extracts, are exactly those in $(3).
expect_diagnostics = diagnostics="$$($(1) 2>&1)" || { printf '%s\n' "$$diagnostics"; exit 1; }; found="$$(printf '%s\n' "$$diagnostics" | sed -n '$(2)' | sort | xargs)"; if [ "$$found" != "$(3)" ]; then printf '%s\n' "$$diagnostics"; echo "Reported: $$found"; echo "Expected: $(3)"; exit 1; fi
WARNING_KIND = s/.*\[-W\([a-z-]*\)=*\]$$/\1/p

# The implementation itself, checked by both analyzers with every finding treated as an error. Runs without optimization so paths are not removed before analysis. Clang's check that asks for the C11 Annex K functions, such as `memcpy_s` instead of `memcpy`, is off, because most C libraries, such as glibc, do not have them. Shares the compiler overrides with `annotations`.
analyze:
	$(ANNOTATIONS_GCC) $(STANDARD) -O0 $(SHARED_WARNINGS) $(GCC_WARNINGS) -fanalyzer -DSOML_IMPLEMENTATION -x c -c soml.h -o /dev/null
	$(ANNOTATIONS_CLANG) $(STANDARD) $(SHARED_WARNINGS) $(CLANG_WARNINGS) --analyze -Xanalyzer -analyzer-werror -Xanalyzer -analyzer-checker=core,unix,security -Xanalyzer -analyzer-disable-checker=security.insecureAPI.DeprecatedOrUnsafeBufferHandling -DSOML_IMPLEMENTATION -x c soml.h -o /dev/null

annotations:
	$(call expect_diagnostics,$(ANNOTATIONS_GCC) -std=c23 -O2 -Wall -Wextra -fanalyzer -c test/annotations.c -o /dev/null,$(WARNING_KIND),$(ANNOTATIONS_GCC_EXPECTED))
	$(call expect_diagnostics,$(ANNOTATIONS_CLANG) -std=c23 --analyze test/annotations.c -o /dev/null,s/.*warning: .*\[\([^][]*\)\]$$/\1/p,$(ANNOTATIONS_CLANG_EXPECTED))
	$(call expect_diagnostics,$(ANNOTATIONS_CLANG) $(STANDARD) -fsyntax-only test/annotations.c,$(WARNING_KIND),$(ANNOTATIONS_UNUSED_RESULT_EXPECTED))

# The tests as a 32-bit program, where `size_t` and pointers have 32 bits, compiled to WebAssembly and run with wasmtime. With Homebrew: `brew install llvm lld wasi-libc wasi-runtimes wasmtime`.
WASM_CC ?= /opt/homebrew/opt/llvm/bin/clang
WASMTIME ?= wasmtime
WASI_SYSROOT ?= /opt/homebrew/share/wasi-sysroot
WASI_RESOURCE_DIR ?= /opt/homebrew/opt/wasi-runtimes/share/wasi-runtimes
WASM_FLAGS = --target=wasm32-wasip1 --sysroot=$(WASI_SYSROOT) -resource-dir=$(WASI_RESOURCE_DIR) $(STANDARD) -O2 $(SHARED_WARNINGS) $(CLANG_WARNINGS) -D_WASI_EMULATED_PROCESS_CLOCKS -lwasi-emulated-process-clocks

$(BUILD)/wasm32/test.wasm: fuzz/allocator.h

$(BUILD)/wasm32/test.wasm $(BUILD)/wasm32/soml-tagged.wasm: $(BUILD)/wasm32/%.wasm: test/%.c soml.h Makefile | $(BUILD)/wasm32
	$(WASM_CC) $(WASM_FLAGS) $< -o $@

wasm32: $(BUILD)/wasm32/test.wasm $(BUILD)/wasm32/soml-tagged.wasm
	$(WASMTIME) run --dir=. $(BUILD)/wasm32/test.wasm
	printf '#!/bin/sh\nexec %s run --dir="%s/test/conformance" "%s/soml-tagged.wasm" "$$@"\n' "$(WASMTIME)" "$(CURDIR)" "$(abspath $(BUILD)/wasm32)" > $(BUILD)/wasm32/soml-tagged
	chmod +x $(BUILD)/wasm32/soml-tagged
	SOML_TAGGED="$(BUILD)/wasm32/soml-tagged" $(NODE) --test test/conformance.js

clean:
	if [ -d "$(BUILD)" ]; then trash "$(BUILD)"; fi

# Fuzzing needs a Clang with libFuzzer, which Xcode does not include, so it uses Homebrew's LLVM when it is there, and `clang` otherwise. FUZZ_SECONDS sets how long each of the four targets runs.
FUZZ_CC ?= $(shell if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; else echo clang; fi)
FUZZ_SECONDS ?= 60
FUZZ_FLAGS = $(STANDARD) -g -O1 -fsanitize=fuzzer $(SANITIZER_FLAGS) $(CLANG_SANITIZERS)
FUZZ_TARGETS = $(addprefix $(BUILD)/,fuzz fuzz-allocator fuzz-builder fuzz-builder-allocator)

$(BUILD)/fuzz $(BUILD)/fuzz-allocator: fuzz/fuzz.c
$(BUILD)/fuzz-builder $(BUILD)/fuzz-builder-allocator: fuzz/builder.c
$(BUILD)/fuzz-allocator $(BUILD)/fuzz-builder-allocator: FUZZ_DEFINES = -DFUZZ_FAILING_ALLOCATOR

$(FUZZ_TARGETS): fuzz/allocator.h soml.h Makefile | $(BUILD)
	$(FUZZ_CC) $(CPPFLAGS) $(FUZZ_FLAGS) $(EXTRA_CFLAGS) $(FUZZ_DEFINES) $(filter %.c,$^) $(LDFLAGS) -o $@ $(LDLIBS)

# The seeds are the conformance cases, named by their path so that none overwrites another. A crash is saved in the build folder. The allocator fuzzer reads the failing allocation from the first two bytes, so its seeds start with a small one, 0 to 7, which a document reaches. A text's own first bytes would be 8224 or more, which no conformance case reaches.
fuzz: $(FUZZ_TARGETS)
	mkdir -p $(BUILD)/fuzz-corpus $(BUILD)/fuzz-allocator-corpus $(BUILD)/fuzz-allocator-seeds $(BUILD)/fuzz-builder-corpus $(BUILD)/fuzz-builder-allocator-corpus
	find test/conformance -name '*.soml' | while read -r file; do cp "$$file" "$(BUILD)/fuzz-corpus/$$(echo "$$file" | tr / _)"; done
	index=0; find test/conformance -name '*.soml' | while read -r file; do { printf '\000'; printf "\\0$$(printf %o $$((index % 8)))"; cat "$$file"; } > "$(BUILD)/fuzz-allocator-seeds/$$(echo "$$file" | tr / _)"; index=$$((index + 1)); done
	$(BUILD)/fuzz -dict=fuzz/soml.dict -max_total_time=$(FUZZ_SECONDS) -max_len=4096 -artifact_prefix=$(BUILD)/ $(BUILD)/fuzz-corpus
	$(BUILD)/fuzz-allocator -dict=fuzz/soml.dict -max_total_time=$(FUZZ_SECONDS) -max_len=4096 -artifact_prefix=$(BUILD)/allocator- $(BUILD)/fuzz-allocator-corpus $(BUILD)/fuzz-allocator-seeds
	$(BUILD)/fuzz-builder -max_total_time=$(FUZZ_SECONDS) -max_len=4096 -artifact_prefix=$(BUILD)/builder- $(BUILD)/fuzz-builder-corpus
	$(BUILD)/fuzz-builder-allocator -max_total_time=$(FUZZ_SECONDS) -max_len=4096 -artifact_prefix=$(BUILD)/builder-allocator- $(BUILD)/fuzz-builder-allocator-corpus

# Differential testing against the JavaScript reference implementation, which must be checked out in ../soml-javascript. DIFFERENTIAL_COUNT sets the number of inputs, and DIFFERENTIAL_SEED the seed of the random inputs.
DIFFERENTIAL_COUNT ?= 20000
DIFFERENTIAL_SEED ?= 1

differential: $(BUILD)/soml-tagged
	$(NODE) --test test/differential-tool.js
	SOML_TAGGED="$(BUILD)/soml-tagged" $(NODE) test/differential.js $(DIFFERENTIAL_COUNT) $(DIFFERENTIAL_SEED)

$(BUILD)/bench: test/bench.c soml.h Makefile | $(BUILD)
	$(CC) $(COMPILE_FLAGS) $< $(LDFLAGS) -o $@ $(LDLIBS)

bench: $(BUILD)/bench
	$(BUILD)/bench

# The most used JSON and TOML libraries in C, at pinned releases, which `make bench-compare` measures too. They are downloaded into the build folder and checked against `test/bench-libraries.sha256`, so the repository does not hold them. They use the same build flags without the standard and warnings above, which are for soml.h.
BENCH_LIBRARIES = ibireme/yyjson/0.13.0/src/yyjson DaveGamble/cJSON/v1.7.19/cJSON cktan/tomlc17/R261003/src/tomlc17
BENCH_OBJECTS = $(addprefix $(BUILD)/bench-libraries/,$(addsuffix .o,$(notdir $(BENCH_LIBRARIES))))

$(BUILD)/bench-libraries/sources: test/bench-libraries.sha256 Makefile | $(BUILD)
	mkdir -p $(BUILD)/bench-libraries
	for library in $(BENCH_LIBRARIES); do for extension in c h; do curl -sSfL --retry 3 -o "$(BUILD)/bench-libraries/$$(basename $$library).$$extension" "https://raw.githubusercontent.com/$$library.$$extension" || exit 1; done; done
	cd $(BUILD)/bench-libraries && shasum -a 256 -c "$(CURDIR)/test/bench-libraries.sha256"
	touch $@

$(BENCH_OBJECTS): $(BUILD)/bench-libraries/%.o: $(BUILD)/bench-libraries/sources Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -c $(BUILD)/bench-libraries/$*.c -o $@

$(BUILD)/bench-compare: test/bench.c soml.h $(BENCH_OBJECTS) Makefile
	$(CC) $(COMPILE_FLAGS) -DBENCH_COMPARE -I$(BUILD)/bench-libraries $< $(BENCH_OBJECTS) $(LDFLAGS) -o $@ $(LDLIBS)

bench-compare: $(BUILD)/bench-compare
	$(BUILD)/bench-compare
