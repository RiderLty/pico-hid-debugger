#!/usr/bin/env bash
# 给 lib/pico_pio_usb 子模块打补丁（幂等）。
#
# 当前只有一枚补丁：0001-sdk2-compat（旧血脉缺的 Pico SDK 2 构建兼容）。
# 子模块固定在旧血脉顶端 9510f79，说明见 patches/pio_usb/README.md。
#
# 用法：
#   ./scripts/apply-patches.sh            # 初始化子模块并应用（已应用的跳过）
#   ./scripts/apply-patches.sh --status   # 只报告每个补丁的状态
#   ./scripts/apply-patches.sh --revert   # 还原子模块工作区（丢弃全部补丁）
#
# 说明：补丁打在子模块工作区，不进入任何提交；`git submodule update --checkout`
# 会把它清掉，重跑本脚本即可（CMake 配置期也会检测并提示）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUB_REL="lib/pico_pio_usb"
SUB="$ROOT/$SUB_REL"
PATCH_DIR="$ROOT/patches/pio_usb"
# 补丁基线：子模块锁定提交（patches/pio_usb/*.patch 以此为 context）
EXPECTED_SHA="9510f79"

MODE="apply"
case "${1:-}" in
    "")           MODE="apply" ;;
    --status)     MODE="status" ;;
    --revert)     MODE="revert" ;;
    -h|--help)
        sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0 ;;
    *) echo "未知参数：$1（-h 查看用法）" >&2; exit 2 ;;
esac

# ---- 前置检查：git 仓库 + 子模块就位 ----
if ! git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    echo "错误：$ROOT 不是 git 仓库（子模块方式需要 git）。" >&2
    exit 1
fi

if [ ! -f "$SUB/src/pio_usb.c" ]; then
    if [ "$MODE" = "status" ]; then
        echo "子模块 $SUB_REL 未初始化（先执行 git submodule update --init --recursive）"
        exit 1
    fi
    echo ">> 初始化子模块 $SUB_REL"
    git -C "$ROOT" submodule update --init --recursive "$SUB_REL"
fi

# ---- 子模块提交与补丁基线是否一致（不一致时只提醒，能否应用由 git apply 判定）----
head_sha="$(git -C "$SUB" rev-parse --short HEAD 2>/dev/null || echo '?')"
if [ "${head_sha:0:7}" != "$EXPECTED_SHA" ]; then
    echo "！警告：子模块位于 ${head_sha}，补丁基线是 ${EXPECTED_SHA} —— 若补丁打不上，" >&2
    echo "        请确认 patches/pio_usb/README.md 中「何时可以删掉」的升级步骤。" >&2
fi

# ---- --revert：直接丢弃工作区改动（补丁只改已跟踪文件）----
if [ "$MODE" = "revert" ]; then
    git -C "$SUB" checkout -- .
    echo ">> 已还原子模块工作区到 ${head_sha}（补丁全部撤销）"
    exit 0
fi

# ---- 逐个补丁：已应用 / 可应用 / 打不上 ----
failed=0
applied_now=0
for patch in "$PATCH_DIR"/*.patch; do
    [ -e "$patch" ] || continue
    name="$(basename "$patch")"

    # 已应用判定：优先 reverse-check（最准）。但补丁是**叠加**的——后续补丁改到
    # 同一段上下文时，reverse-check 会失效（实测 0004 改了 0003 的相邻行）。
    # 那就退化为"看这条补丁新增的一行是否还在位"：够用且不会误判为可应用
    # （真漏打时下面的 CMake 守卫也会直接报错，不会静默编出坏固件）。
    applied="no"
    if git -C "$SUB" apply --reverse --check "$patch" >/dev/null 2>&1; then
        applied="yes"
    else
        # 取前 3 行"够长"的新增行作候选标记，任一仍在位即认为已应用。
        # 长度门槛必不可少：像 "+//" 这种短注释在**任何**文件里都能匹配上，
        # 会造成"其实没打却判定已应用"（实测踩过，CMake 守卫兜底才发现）。
        tried=0
        while IFS= read -r cand; do
            [ "${#cand}" -ge 12 ] || continue
            tried=$((tried + 1))
            [ "$tried" -gt 3 ] && break
            if grep -Fq -- "$cand" "$SUB"/src/*.c "$SUB"/src/*.h 2>/dev/null; then
                applied="yes"
                break
            fi
        done < <(grep -v '^+++' "$patch" | grep '^+' | sed 's/^+//' \
                 | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    fi

    if [ "$applied" = "yes" ]; then
        echo "   [已应用] $name"
        continue
    fi

    if [ "$MODE" = "status" ]; then
        if git -C "$SUB" apply --check "$patch" >/dev/null 2>&1; then
            echo "   [未应用] $name"
        else
            echo "   [打不上] ${name}（基线与子模块提交不一致？）"
            failed=1
        fi
        continue
    fi

    if git -C "$SUB" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$SUB" apply "$patch"
        echo "   [已打上] $name"
        applied_now=$((applied_now + 1))
    else
        echo "   错误：$name 无法应用（子模块提交 ${head_sha} 与补丁基线 $EXPECTED_SHA 不一致？）" >&2
        failed=1
    fi
done

if [ "$failed" -ne 0 ]; then
    echo ">> 有补丁未能应用，详见 patches/pio_usb/README.md" >&2
    exit 1
fi

if [ "$MODE" = "status" ]; then
    exit 0
fi

if [ "$applied_now" -eq 0 ]; then
    echo ">> 补丁已全部在位，无需改动"
else
    echo ">> 本次新打上 $applied_now 个补丁，可以构建了：./build.sh"
fi
