# Convenience wrapper around the single CMake/CTest graph.  This Makefile
# never implements a second test or benchmark runner: `make test` runs the
# CTest `correctness` preset, `make bench` runs the CTest `benchmark` preset,
# and the sanitizer targets reuse the same graph through their presets.

SRCDIR=packages/markdown-core/core
EXTDIR=packages/markdown-core/extensions
BUILDDIR=build/cmake
ASAN_BUILDDIR=build/asan
UBSAN_BUILDDIR=build/ubsan
TSAN_BUILDDIR=build/tsan
MARKDOWN_CORE=$(BUILDDIR)/packages/markdown-core/core/markdown-core
MARKDOWN_CORE_FUZZ=$(BUILDDIR)/packages/markdown-core/core/markdown-core-fuzz
SPEC=packages/markdown-core/tests/fixtures/spec.txt
CLANG_CHECK?=clang-check
AFL_PATH?=/usr/local/bin

.PHONY: all build test bench asan-test ubsan-test tsan-test install clean distclean \
	afl libFuzzer clang-check archive update-spec

all: build

build:
	cmake --preset default
	cmake --build --preset default --parallel

test: build
	ctest --preset correctness

bench: build
	ctest --preset benchmark

asan-test:
	cmake --preset asan
	cmake --build --preset asan --parallel
	ctest --preset correctness-asan

ubsan-test:
	cmake --preset ubsan
	cmake --build --preset ubsan --parallel
	ctest --preset correctness-ubsan

tsan-test:
	cmake --preset tsan
	cmake --build --preset tsan --parallel
	ctest --preset correctness-tsan

install: build
	cmake --install $(BUILDDIR)

$(MARKDOWN_CORE): build

# Explicit, non-default fuzz campaigns.  They reuse the corpus and dictionary
# under packages/markdown-core/tests/core and write findings into the build
# tree only.
afl:
	@[ -n "$(AFL_PATH)" ] || { echo '$$AFL_PATH not set'; false; }
	cmake --preset default -DMARKDOWN_CORE_TESTS=0 -DCMAKE_C_COMPILER=$(AFL_PATH)/afl-clang
	cmake --build --preset default --parallel
	$(AFL_PATH)/afl-fuzz \
	    -i packages/markdown-core/tests/core/afl_test_cases \
	    -o $(BUILDDIR)/afl_results \
	    -x packages/markdown-core/tests/core/fuzzing_dictionary \
	    $(AFL_OPTIONS) \
	    -t 100 \
	    $(MARKDOWN_CORE) -e table -e strikethrough -e autolink $(MARKDOWN_CORE_OPTS)

libFuzzer:
	@[ -n "$(LIB_FUZZER_PATH)" ] || { echo '$$LIB_FUZZER_PATH not set'; false; }
	cmake --preset default -DCMAKE_BUILD_TYPE=Asan -DMARKDOWN_CORE_LIB_FUZZER=ON \
	    -DCMAKE_LIB_FUZZER_PATH=$(LIB_FUZZER_PATH)
	cmake --build --preset default --parallel --target markdown-core-fuzz
	packages/markdown-core/tests/core/run-markdown-core-fuzz $(MARKDOWN_CORE_FUZZ)

clang-check: all
	${CLANG_CHECK} -p $(BUILDDIR) -analyze $(SRCDIR)/*.c

archive:
	git archive --prefix=markdown-core/ -o markdown-core.tar.gz HEAD
	git archive --prefix=markdown-core/ -o markdown-core.zip HEAD

clean:
	rm -rf build

distclean: clean
	-rm -rf *.dSYM
	-rm -f README.html

# Maintenance-only source generation; the generated files are tracked, so
# these never run during normal build or test.
$(SRCDIR)/scanners.c: $(SRCDIR)/scanners.re
	@case "$$(re2c -v)" in \
	    *\ 0.13.*|*\ 0.14|*\ 0.14.1) \
		echo "re2c >= 0.14.2 is required"; \
		false; \
		;; \
	esac
	re2c -W -Werror --case-insensitive -b -i --no-generation-date -8 \
		--encoding-policy substitute -o $@ $<

$(EXTDIR)/ext_scanners.c: $(EXTDIR)/ext_scanners.re
	@case "$$(re2c -v)" in \
	    *\ 0.13.*|*\ 0.14|*\ 0.14.1) \
		echo "re2c >= 0.14.2 is required"; \
		false; \
		;; \
	esac
	re2c --case-insensitive -b -i --no-generation-date -8 \
		--encoding-policy substitute -o $@ $<

# Explicit maintenance command; normal test and bench runs never touch the
# network.
update-spec:
	curl 'https://raw.githubusercontent.com/jgm/CommonMark/master/spec.txt'\
 > $(SPEC)
