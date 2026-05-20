#!/usr/bin/env bash
# Product 闭环验证：必须使用 ~/.venv；统一 Markdown 报告在仓库根 reports/。
set -euo pipefail

PYTHON="${HOME}/.venv/bin/python"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YIKV_SERVER_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
REPORTS_DIR="${YIKV_SERVER_ROOT}/reports"
mkdir -p "${REPORTS_DIR}"

TS="$(date -u +%Y%m%dT%H%M%SZ)"
START_EPOCH="$(date +%s)"
START_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

RUN_LOG="${REPORTS_DIR}/product-closed-loop-${TS}-console.log"

TIER0_REPORT="${VERIFY_PRODUCT_REPORT:-}"
if [[ -z "${TIER0_REPORT}" ]]; then
  TIER0_REPORT="${REPORTS_DIR}/product-parquet-verify-${TS}.md"
fi
export VERIFY_PRODUCT_REPORT="${TIER0_REPORT}"

SUMMARY="${PRODUCT_CLOSED_LOOP_REPORT:-${REPORTS_DIR}/product-closed-loop-${TS}.md}"

write_no_venv_report() {
  local out="${REPORTS_DIR}/product-closed-loop-${TS}-NO-VENV.md"
  mkdir -p "${REPORTS_DIR}"
  {
    echo "# Product 闭环验证报告 — 未找到 ~/.venv"
    echo
    echo "## 运行环境"
    echo "- **期望 Python**: \`${PYTHON}\`"
    echo "- **存在**: $([[ -x "${PYTHON}" ]] && echo 是 || echo 否)"
    echo "- **hostname**: $(hostname 2>/dev/null || true)"
    echo "- **uname**: $(uname -a 2>/dev/null || true)"
    echo "- **开始 UTC**: ${START_UTC}"
    echo
    echo "## 运行结果"
    echo "- **退出码**: 2"
    echo "- **结论**: 失败（未使用 ~/.venv）"
    echo
    echo "## 错误"
    echo '请创建并安装依赖: `python3 -m venv ~/.venv && ~/.venv/bin/pip install -r requirements.txt`（在仓库根）'
    echo
  } > "${out}"
  echo "verify_product_e2e: NO_VENV_REPORT=${out}" >&2
}

if [[ ! -x "${PYTHON}" ]]; then
  write_no_venv_report
  exit 2
fi

if [[ $# -lt 1 ]]; then
  echo "usage: $0 path/to/product.parquet" >&2
  exit 2
fi

tier0_ec=0
set -o pipefail
"${PYTHON}" "${SCRIPT_DIR}/verify_product_parquet.py" "$1" 2>&1 | tee "${RUN_LOG}" || tier0_ec=${PIPESTATUS[0]}
set +o pipefail

tier1_publish_ec=0
tier1_publish_status="skipped"

if [[ "${VERIFY_TIER1_PUBLISH:-}" == "1" ]]; then
  echo "==> VERIFY_TIER1_PUBLISH=1 → run-publish-job.sh" | tee -a "${RUN_LOG}"
  set +e
  set -o pipefail
  "${SCRIPT_DIR}/../run-publish-job.sh" --wait \
    --spec-file "${SCRIPT_DIR}/../examples/publish-spec.product.json" 2>&1 | tee -a "${RUN_LOG}"
  rc_pub=${PIPESTATUS[0]}
  set +o pipefail
  set -e
  if [[ "${rc_pub}" -eq 0 ]]; then
    tier1_publish_status="passed"
  else
    tier1_publish_status="failed"
    tier1_publish_ec="${rc_pub}"
  fi
fi

tier1_deploy_status="skipped"
tier1_deploy_ec=0
if [[ "${VERIFY_TIER1_DEPLOY:-}" == "1" ]]; then
  if [[ -n "${ONLINE_AGENT_URL:-}" ]]; then
    echo "==> VERIFY_TIER1_DEPLOY deploy-only" | tee -a "${RUN_LOG}"
    set +e
    set -o pipefail
    PYTHONPATH="${YIKV_SERVER_ROOT}/tools/pipeline_agent${PYTHONPATH:+:${PYTHONPATH}}" \
      "${PYTHON}" "${YIKV_SERVER_ROOT}/tools/pipeline_agent/pipeline_http_client.py" \
      --table product --deploy-only 2>&1 | tee -a "${RUN_LOG}"
    rc_dep=${PIPESTATUS[0]}
    set +o pipefail
    set -e
    if [[ "${rc_dep}" -eq 0 ]]; then
      tier1_deploy_status="passed"
    else
      tier1_deploy_status="failed"
      tier1_deploy_ec="${rc_dep}"
    fi
  elif [[ "${REQUIRE_ONLINE_AGENT:-}" == "1" ]]; then
    echo "REQUIRE_ONLINE_AGENT=1 but ONLINE_AGENT_URL is unset" | tee -a "${RUN_LOG}" >&2
    tier1_deploy_status="failed_required_unset"
    tier1_deploy_ec=2
  else
    tier1_deploy_status="skipped_no_url"
  fi
fi

final_ec="${tier0_ec}"
[[ "${tier1_publish_ec}" -gt "${final_ec}" ]] && final_ec="${tier1_publish_ec}"
[[ "${tier1_deploy_ec}" -gt "${final_ec}" ]] && final_ec="${tier1_deploy_ec}"

END_EPOCH="$(date +%s)"
END_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
DUR=$((END_EPOCH - START_EPOCH))

PQ_ABS="$("${PYTHON}" -c "import os, sys; print(os.path.realpath(sys.argv[1]))" "$1")"
OPS_MD="${REPORTS_DIR}/product-closed-loop-${TS}-operations.md"
{
  echo "# Product 闭环测试 — 执行的操作记录"
  echo
  echo "由 \`verify_product_e2e.sh\` 生成本文件，列出**本轮实际调用的命令**与每一阶段的用途。"
  echo "**Tier-0** 为必选：完整 **契约 + 行级** 校验（非零散冒烟）。**Tier-1 / Tier-2** 由环境变量按需开启，用于集群发布与在线 deploy。"
  echo
  echo "## 相关环境变量（启动前快照）"
  echo
  echo "| 变量 | 值 |"
  echo "|------|-----|"
  echo "| \`VERIFY_TIER1_PUBLISH\` | \`${VERIFY_TIER1_PUBLISH:-<未设置>}\` |"
  echo "| \`VERIFY_TIER1_DEPLOY\` | \`${VERIFY_TIER1_DEPLOY:-<未设置>}\` |"
  echo "| \`ONLINE_AGENT_URL\` | \`${ONLINE_AGENT_URL:-<未设置>}\` |"
  echo "| \`REQUIRE_ONLINE_AGENT\` | \`${REQUIRE_ONLINE_AGENT:-<未设置>}\` |"
  echo
  echo "## 按顺序执行的操作"
  echo
  echo "### 步骤 1 — Tier-0（离线闭环数据门禁，必选）"
  echo
  echo "**命令**："
  echo
  echo "\`\`\`text"
  echo "${PYTHON} ${SCRIPT_DIR}/verify_product_parquet.py ${PQ_ABS}"
  echo "\`\`\`"
  echo
  echo "- **作用**：对照 \`deploy/e2e/fixtures/product/schema.json\` 校验 Parquet 列类型与行级业务规则；生成 Tier-0 Markdown 报告：\`${TIER0_REPORT}\`。"
  echo "- **本轮退出码**：${tier0_ec}"
  echo
  if [[ "${VERIFY_TIER1_PUBLISH:-}" == "1" ]]; then
    echo "### 步骤 2 — Tier-1 集群发布（\`run-publish-job.sh\` → publish Job）"
    echo
    echo "**命令**："
    echo
    echo "\`\`\`text"
    echo "${SCRIPT_DIR}/../run-publish-job.sh --wait --spec-file ${SCRIPT_DIR}/../examples/publish-spec.product.json"
    echo "\`\`\`"
    echo
    echo "- **作用**：在 Kubernetes 内构建 pipeline-worker 镜像（如需要）、提交 **publishIndex** 任务，按 \`publish-spec.product.json\` 拉取输入并发布制品。"
    echo "- **本轮退出码**：${tier1_publish_ec}，状态：**${tier1_publish_status}**"
    echo
  else
    echo "### 步骤 2 — Tier-1 集群发布（未执行）"
    echo
    echo "- **说明**：未设置 \`VERIFY_TIER1_PUBLISH=1\`，**未**调用 \`run-publish-job.sh\`。要跑完整「构建+发布」闭环，请开启该变量并确保集群/MinIO 等先决条件就绪。"
    echo
  fi
  if [[ "${VERIFY_TIER1_DEPLOY:-}" == "1" ]]; then
    if [[ -n "${ONLINE_AGENT_URL:-}" ]]; then
      echo "### 步骤 3 — Tier-1 在线 deployIndex（HTTP）"
      echo
      echo "**命令**："
      echo
      echo "\`\`\`text"
      echo "PYTHONPATH=${YIKV_SERVER_ROOT}/tools/pipeline_agent${PYTHONPATH:+:${PYTHONPATH}} \\"
      echo "  ${PYTHON} ${YIKV_SERVER_ROOT}/tools/pipeline_agent/pipeline_http_client.py --table product --deploy-only"
      echo "\`\`\`"
      echo
      echo "- **作用**：通过 \`pipeline_http_client\` 对在线 \`pipeline_agent\` 调用 **deployIndex**（\`ONLINE_AGENT_URL\` / \`BUILD_AGENT_URL\` 见客户端逻辑）。"
      echo "- **本轮退出码**：${tier1_deploy_ec}，状态：**${tier1_deploy_status}**"
    else
      echo "### 步骤 3 — Tier-1 在线 deployIndex（未执行或未满足）"
      echo
      echo "- **说明**：已设 \`VERIFY_TIER1_DEPLOY=1\` 但未设置 \`ONLINE_AGENT_URL\`（或 \`REQUIRE_ONLINE_AGENT\` 导致中止）：本轮未对在线服务发起 deploy。"
    fi
  else
    echo "### 步骤 3 — 在线 deployIndex（未执行）"
    echo
    echo "- **说明**：未设置 \`VERIFY_TIER1_DEPLOY=1\`，未调用 \`pipeline_http_client.py --deploy-only\`。"
  fi
} > "${OPS_MD}"

export E2E_SUMMARY_OUT="${SUMMARY}"
export E2E_OPERATIONS_MD="${OPS_MD}"
export E2E_RUN_LOG="${RUN_LOG}"
export E2E_START_UTC="${START_UTC}"
export E2E_END_UTC="${END_UTC}"
export E2E_DURATION_S="${DUR}"
export E2E_PYTHON="${PYTHON}"
export E2E_TIER0_REPORT="${TIER0_REPORT}"
export E2E_TIER0_EC="${tier0_ec}"
export E2E_TIER1P_EC="${tier1_publish_ec}"
export E2E_TIER1P_ST="${tier1_publish_status}"
export E2E_TIER1D_EC="${tier1_deploy_ec}"
export E2E_TIER1D_ST="${tier1_deploy_status}"
export E2E_FINAL_EC="${final_ec}"
export E2E_REPO_ROOT="${YIKV_SERVER_ROOT}"
export E2E_PARQUET="$1"

set +e
"${PYTHON}" - <<'PY'
import os
import platform
from pathlib import Path

out = Path(os.environ["E2E_SUMMARY_OUT"])
run_log_path = os.environ["E2E_RUN_LOG"]
wall_s = int(os.environ.get("E2E_DURATION_S", "0"))
py = os.environ["E2E_PYTHON"]

repo = os.environ.get("E2E_REPO_ROOT", "")
git_h = ""
if repo:
    try:
        import subprocess
        git_h = subprocess.check_output(
            ["git", "-C", repo, "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        git_h = ""
try:
    uname_s = " ".join(os.uname())
except (AttributeError, ValueError):
    uname_s = platform.platform()
tier0_path = os.environ["E2E_TIER0_REPORT"]
tier0_ec = int(os.environ["E2E_TIER0_EC"])
tp_ec = int(os.environ["E2E_TIER1P_EC"])
tp_st = os.environ["E2E_TIER1P_ST"]
td_ec = int(os.environ["E2E_TIER1D_EC"])
td_st = os.environ["E2E_TIER1D_ST"]
final_ec = int(os.environ["E2E_FINAL_EC"])
pq = os.environ["E2E_PARQUET"]
ops_path = os.environ.get("E2E_OPERATIONS_MD", "").strip()
if ops_path:
    op_p = Path(ops_path)
    try:
        ops_embed = op_p.read_text(encoding="utf-8") if op_p.is_file() else f"*（操作记录文件不存在：`{ops_path}`）*"
    except OSError as exc:
        ops_embed = f"*（读取操作记录失败：{exc}）*"
else:
    ops_embed = "*（未生成操作记录路径）*"

rl = Path(run_log_path)
try:
    log_full = rl.read_text(encoding="utf-8")
except OSError:
    log_full = "(无法读取终端日志)"
tp = Path(tier0_path)
try:
    tier0_embed = tp.read_text(encoding="utf-8") if tp.is_file() else ""
except OSError:
    tier0_embed = ""

status = "通过" if final_ec == 0 else "失败"

def fence(s: str) -> str:
    return s.replace("```", "`\u200b``")

summary_ops_note = f"\n> 执行的操作另存为独立文件：`{ops_path}`\n" if ops_path else "\n"

md = f"""# Product 闭环验证报告 (Tier-0 + 可选 Tier-1)

## 摘要

| 项 | 值 |
|----|----|
| 开始 (UTC) | {os.environ['E2E_START_UTC']} |
| 结束 (UTC) | {os.environ['E2E_END_UTC']} |
| **墙钟耗时** | **{wall_s} s** |
| **汇总退出码** | **{final_ec}** |
| **结论** | **{status}** |

{summary_ops_note}

## 闭环测试执行的操作（记录）

以下为**本轮实际执行的步骤与命令**（闭环，非冒烟）；与仅跑 Tier-0 的区别见小节说明。

{ops_embed}

## 运行环境

- **仓库根目录**: `{repo}`
- **git HEAD (short)**: `{git_h or '(n/a)'}`
- **uname**: `{uname_s}`
- **Python**: `{py}`
- **platform**: `{platform.platform()}`
- **Tier-0 报告文件**: `{tier0_path}`
- **本次终端日志文件**: `{run_log_path}`

## 各阶段结果

| 阶段 | 退出码 | 状态 |
|------|--------|------|
| Tier-0 Parquet | {tier0_ec} | {'通过' if tier0_ec == 0 else '失败'} |
| Tier-1 publish (可选) | {tp_ec} | {tp_st} |
| Tier-1 deploy (可选) | {td_ec} | {td_st} |

## 输入

- Parquet: `{pq}`

## 完整终端日志（stdout+stderr）

```text
{fence(log_full)}
```

## Tier-0 Markdown 报告（嵌入）

以下为 **`{tier0_path}`** 的完整内容：

---

{tier0_embed if tier0_embed else '*（未能读取 Tier-0 报告）*'}

---

*由 `verify_product_e2e.sh` + 内嵌 Python 生成*
"""

out.parent.mkdir(parents=True, exist_ok=True)
tmp = out.with_suffix(out.suffix + ".tmp")
tmp.write_text(md, encoding="utf-8")
tmp.replace(out)
PY
sum_ec=$?
set -e

if [[ "${sum_ec}" -ne 0 ]]; then
  echo "verify_product_e2e: failed writing summary markdown (${sum_ec}) → ${SUMMARY}" >&2
  exit 5
fi

echo "verify_product_e2e: summary_written=${SUMMARY}" >&2
echo "verify_product_e2e: operations_written=${OPS_MD}" >&2
echo "verify_product_e2e: console_log=${RUN_LOG}" >&2
exit "${final_ec}"
