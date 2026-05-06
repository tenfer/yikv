#!/usr/bin/env bash
# yikv 编译脚本
#
# 用法：
#   ./build.sh              # 编译所有目标（默认 opt 模式）
#   ./build.sh server       # 只编译 server
#   ./build.sh client       # 只编译 client
#   ./build.sh test         # 运行 //tests:all_tests 单元测试套件
#   ./build.sh debug        # Debug 模式（含 ASan）
#   ./build.sh clean        # 清理构建产物

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── 颜色输出 ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[yikv]${NC} $*"; }
warn()  { echo -e "${YELLOW}[yikv]${NC} $*"; }
error() { echo -e "${RED}[yikv]${NC} $*" >&2; }

# ── 参数解析 ───────────────────────────────────────────────────────────────────
TARGET="${1:-all}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

BAZEL_COMMON_FLAGS=(
    "--jobs=${JOBS}"
    "--show_progress_rate_limit=5"
)

# ── 目标映射 ───────────────────────────────────────────────────────────────────
build_opt() {
    local targets=("$@")
    info "Building: ${targets[*]}  [opt, jobs=${JOBS}]"
    bazel build -c opt \
        "${BAZEL_COMMON_FLAGS[@]}" \
        "${targets[@]}"
}

build_debug() {
    local targets=("$@")
    info "Building: ${targets[*]}  [debug + ASan, jobs=${JOBS}]"
    bazel build --config=debug \
        "${BAZEL_COMMON_FLAGS[@]}" \
        "${targets[@]}"
}

# ── 输出路径提示 ───────────────────────────────────────────────────────────────
show_outputs() {
    echo ""
    info "Build outputs:"
    local bins=(
        "bazel-bin/src/server/yikv_server"
        "bazel-bin/client/yikv_client"
    )
    for b in "${bins[@]}"; do
        [[ -f "$b" ]] && echo "  $b"
    done
}

# ── 主逻辑 ────────────────────────────────────────────────────────────────────
case "$TARGET" in
    all)
        build_opt \
            //src/server:yikv_server \
            //client:yikv_client \
            //tests:all_tests
        show_outputs
        ;;
    server)
        build_opt //src/server:yikv_server
        show_outputs
        ;;
    client)
        build_opt //client:yikv_client
        show_outputs
        ;;
    test)
        info "Running tests: //tests:all_tests  [opt, jobs=${JOBS}]"
        bazel test -c opt \
            "${BAZEL_COMMON_FLAGS[@]}" \
            //tests:all_tests
        ;;
    debug)
        build_debug \
            //src/server:yikv_server \
            //client:yikv_client \
            //tests:all_tests
        show_outputs
        ;;
    clean)
        info "Cleaning build cache..."
        bazel clean
        info "Done"
        ;;
    *)
        error "Unknown target: $TARGET"
        echo "Usage: $0 [all|server|client|test|debug|clean]"
        exit 1
        ;;
esac

info "Done"
