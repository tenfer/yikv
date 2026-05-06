JOBS ?= $(shell nproc 2>/dev/null || echo 4)
BAZEL_FLAGS = --jobs=$(JOBS) --show_progress_rate_limit=5

# install-headers uses `read -d ''` (needs bash, not plain POSIX sh).
SHELL := /bin/bash

# Optional CPU tuning (RocksDB-style PORTABLE): empty = Bazel defaults;
# PORTABLE=1 -> generic x86-64-ish; PORTABLE=haswell -> compiler -march=haswell.
EXTRA_BAZEL_FLAGS ?=
ifneq ($(strip $(PORTABLE)),)
ifeq ($(strip $(PORTABLE)),1)
override EXTRA_BAZEL_FLAGS += --copt=-mtune=generic --copt=-march=x86-64
else
override EXTRA_BAZEL_FLAGS += --copt=-march=$(strip $(PORTABLE))
endif
endif

# Compilation mode for libraries (opt|dbg).
YIKV_COMPILATION_MODE ?= opt

export EXTRA_BAZEL_FLAGS
export BAZEL_FLAGS
export YIKV_COMPILATION_MODE

# GNU-style install layout (FHS).
PREFIX       ?= /usr/local
EXEC_PREFIX  ?= $(PREFIX)
BINDIR       ?= $(EXEC_PREFIX)/bin
LIBDIR       ?= $(EXEC_PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include
DATAROOTDIR  ?= $(PREFIX)/share
DOCDIR       ?= $(DATAROOTDIR)/doc/yikv
DESTDIR      ?=
INSTALL      ?= install

INST_BINDIR  := $(DESTDIR)$(BINDIR)
INST_LIBDIR  := $(DESTDIR)$(LIBDIR)
INST_INCDIR  := $(DESTDIR)$(INCLUDEDIR)/yikv
INST_DOCDIR  := $(DESTDIR)$(DOCDIR)

BUNDLE_SCRIPT := $(CURDIR)/scripts/bundle-libyikv.sh

# After bundle-lib, artifacts live under workspace bazel-bin/ (symlink).
LIBYIKV_A := $(CURDIR)/bazel-bin/libyikv.a
LIBYIKV_SO := $(CURDIR)/bazel-bin/libyikv.so

# Install libraries under $(DESTDIR)$(LIBDIR): default installs libyikv.so and libyikv.a.
# INSTALL_STATIC=0 → shared only; INSTALL_SHARED=0 → static only.
INSTALL_STATIC ?= 1
INSTALL_SHARED ?= 1
INSTALL_HEADERS ?= 1

.PHONY: all db_tool benchmark bundle-lib static_lib shared_lib install install-lib install-headers install-db uninstall uninstall-lib uninstall-db uninstall-docs test debug clean check

all:
	bazel build -c $(YIKV_COMPILATION_MODE) $(BAZEL_FLAGS) \
		//src/db:db_tool \
		//tests:kv_index_benchmark \
		//tests:db_benchmark \
		//tests:all_tests
	@test -x "$(BUNDLE_SCRIPT)" || { echo "missing $(BUNDLE_SCRIPT)" >&2; exit 1; }
	@"$(BUNDLE_SCRIPT)"

# Produce bazel-bin/libyikv.{a,so} by merging pic archives from src/** (needs //src/db:db transitive build).
bundle-lib static_lib shared_lib:
	@test -x "$(BUNDLE_SCRIPT)" || { echo "missing $(BUNDLE_SCRIPT)" >&2; exit 1; }
	@"$(BUNDLE_SCRIPT)"

install-headers:
	@if [ "$(INSTALL_HEADERS)" != 0 ]; then \
		install -d "$(INST_INCDIR)"; \
		cd "$(CURDIR)" && find src -type f -name '*.h' -print0 | while IFS= read -r -d '' h; do \
			install -d "$(INST_INCDIR)/$$(dirname "$$h")"; \
			$(INSTALL) -m 644 "$$h" "$(INST_INCDIR)/$$h"; \
		done; \
	fi

install-lib: bundle-lib install-headers
	@if [ "$(INSTALL_STATIC)" != 0 ] || [ "$(INSTALL_SHARED)" != 0 ]; then \
		$(INSTALL) -d "$(INST_LIBDIR)"; \
	fi
	@if [ "$(INSTALL_STATIC)" != 0 ]; then \
		test -f "$(LIBYIKV_A)" || { echo 'run make bundle-lib first' >&2; exit 1; }; \
		$(INSTALL) -m 644 "$(LIBYIKV_A)" "$(INST_LIBDIR)/libyikv.a"; \
	fi
	@if [ "$(INSTALL_SHARED)" != 0 ]; then \
		test -f "$(LIBYIKV_SO)" || { echo 'run make bundle-lib first' >&2; exit 1; }; \
		$(INSTALL) -m 755 "$(LIBYIKV_SO)" "$(INST_LIBDIR)/libyikv.so"; \
	fi

INSTALL_DOCS_CLI ?= 1

install-db: db_tool
	$(INSTALL) -d "$(INST_BINDIR)"
	$(INSTALL) -m 755 "$(CURDIR)/bazel-bin/src/db/db_tool" "$(INST_BINDIR)/yikv-db_tool"

# Full install → system paths: PREFIX (default /usr/local), via $(DESTDIR)/$(PREFIX)/...
# Copies libyikv.so and libyikv.a (unless INSTALL_* knobs say otherwise), headers, yikv-db_tool, README.
install: install-lib install-db
	@if [ "$(INSTALL_DOCS_CLI)" != 0 ] && [ -f README.md ]; then \
		$(INSTALL) -d "$(INST_DOCDIR)" && $(INSTALL) -m 644 README.md "$(INST_DOCDIR)/"; \
	fi

uninstall-lib:
	rm -f "$(INST_LIBDIR)/libyikv.a" "$(INST_LIBDIR)/libyikv.so"
	rm -rf "$(INST_INCDIR)"

uninstall-db:
	rm -f "$(INST_BINDIR)/yikv-db_tool"

uninstall-docs:
	rm -rf "$(INST_DOCDIR)"

uninstall: uninstall-docs uninstall-db uninstall-lib

db_tool:
	bazel build -c $(YIKV_COMPILATION_MODE) $(BAZEL_FLAGS) //src/db:db_tool

benchmark:
	bazel build -c $(YIKV_COMPILATION_MODE) $(BAZEL_FLAGS) \
		//tests:kv_index_benchmark \
		//tests:db_benchmark

# Like RocksDB make check (debug compilation for tests).
check:
	bazel test -c dbg $(BAZEL_FLAGS) //tests:all_tests

test:
	bazel test -c $(YIKV_COMPILATION_MODE) $(BAZEL_FLAGS) //tests:all_tests

debug:
	bazel build --config=debug $(BAZEL_FLAGS) \
		//src/db:db_tool \
		//tests:kv_index_benchmark \
		//tests:db_benchmark \
		//tests:all_tests

clean:
	bazel clean
