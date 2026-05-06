JOBS ?= $(shell nproc 2>/dev/null || echo 4)
BAZEL_FLAGS = --jobs=$(JOBS) --show_progress_rate_limit=5

.PHONY: all db_tool benchmark test debug clean

all:
	bazel build -c opt $(BAZEL_FLAGS) \
		//src/db:db_tool \
		//tests:kv_index_benchmark \
		//tests:db_benchmark \
		//tests:all_tests

db_tool:
	bazel build -c opt $(BAZEL_FLAGS) //src/db:db_tool

benchmark:
	bazel build -c opt $(BAZEL_FLAGS) \
		//tests:kv_index_benchmark \
		//tests:db_benchmark

test:
	bazel test -c opt $(BAZEL_FLAGS) //tests:all_tests

debug:
	bazel build --config=debug $(BAZEL_FLAGS) \
		//src/db:db_tool \
		//tests:kv_index_benchmark \
		//tests:db_benchmark \
		//tests:all_tests

clean:
	bazel clean
