#!/usr/bin/env bash
# 结构守卫的 macOS 入口。
#
# 现状（第 44 轮查明）：完整检查链只有 Windows 版本——`tools/testing/Run-StructureChecks.ps1`
# 需要 pwsh 7 + clang-cl + MSVC 前端与 CTest，由 `Get-TestInventory.ps1` 生成
# `--inventory` / `--trace`。这三个产物在 macOS 上不存在，所以守卫在本机「从没跑过」，
# S-13 登记的脏基线也就一直没人发现。
#
# 本脚本跑不了那道完整门，但能跑掉其中不依赖构建产物的两段：
#   ① 合成夹具自检 `test_structure_guard.py`（366 例，约 1.3 秒）——守卫本身的判据还准不准
#   ② 基线引用完整性 `check_baseline_refs.py`——基线里还有没有指向已删文件的死条目
#
# 用法：
#   bash tools/structure/run.sh              # 报告模式（悬空只报不拦）
#   bash tools/structure/run.sh --strict     # 悬空即失败（接门禁时用这个）
#   bash tools/structure/run.sh --baseline <json>   # 指向另一份基线（负向验证用）
#   bash tools/structure/run.sh --inventory <inventory.json> --trace <configure-trace.jsonl>
#                                            # 有 Windows 侧产物时顺带跑完整 scan
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PY=${PYTHON:-python3}
STRICT=0
INVENTORY=""
TRACE=""
OUTPUT=""
BASELINE="$ROOT/tools/structure/structure-baseline.json"

while [ $# -gt 0 ]; do
  case "$1" in
    --strict) STRICT=1; shift ;;
    --inventory) INVENTORY=${2:?--inventory 需要路径}; shift 2 ;;
    --trace) TRACE=${2:?--trace 需要路径}; shift 2 ;;
    --baseline) BASELINE=${2:?--baseline 需要路径}; shift 2 ;;
    --output) OUTPUT=${2:?--output 需要路径}; shift 2 ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

if [ -z "$OUTPUT" ]; then
  OUTPUT=$(mktemp -d "${TMPDIR:-/tmp}/concord-structure-XXXXXX")
fi
mkdir -p "$OUTPUT"

status=0

echo "== ① 守卫合成夹具"
if ! "$PY" -I -B "$ROOT/tools/structure/test_structure_guard.py" --output "$OUTPUT/fixtures" \
     > "$OUTPUT/fixtures.log" 2>&1; then
  echo "FAIL  夹具自检未通过，日志 $OUTPUT/fixtures.log" >&2
  tail -20 "$OUTPUT/fixtures.log" >&2 || true
  status=1
else
  echo "PASS  $(tail -3 "$OUTPUT/fixtures.log" | tr '\n' ' ')"
fi

echo "== ② 基线引用完整性"
# macOS 自带 bash 3.2：`set -u` 下空数组展开 `${a[@]}` 会报 unbound variable，
# 所以这里用空字符串而不是数组（本轮实测踩到）。
refs=""
if [ "$STRICT" -eq 1 ]; then refs="--strict"; fi
if ! "$PY" -I -B "$ROOT/tools/structure/check_baseline_refs.py" \
     --root "$ROOT" --baseline "$BASELINE" \
     --output "$OUTPUT/baseline-refs.json" ${refs}; then
  echo "FAIL  基线仍有指向已删文件的例外（收紧方式见 baseline 的 policy：人工审过再删）" >&2
  status=1
fi

if [ -n "$INVENTORY" ] && [ -n "$TRACE" ]; then
  echo "== ③ 完整 scan（Windows 侧产物）"
  if ! "$PY" -I -B "$ROOT/tools/structure/structure_guard.py" --root "$ROOT" \
       --build "$ROOT/build-product-arm" --inventory "$INVENTORY" --trace "$TRACE" \
       --baseline "$BASELINE" --output "$OUTPUT/scan"; then
    echo "FAIL  完整 scan 报回归或需收紧例外，见 $OUTPUT/scan/result.json" >&2
    status=1
  fi
else
  echo "== ③ 完整 scan 跳过（缺 --inventory/--trace：只有 Windows 侧能生成）"
fi

echo "证据目录：$OUTPUT"
exit "$status"
