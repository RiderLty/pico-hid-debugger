#!/usr/bin/env bash
# pico-hid-debugger 一键构建脚本（git clone 后无需任何手工配置）
#
# 用法：
#   ./build.sh                                  # SDK 自动探测
#   ./build.sh --update-hidkit                  # 跟最新 + 提交/推送子模块指针，再构建
#   ./build.sh --update-hidkit-only             # 只做子模块更新（+提交/推送），不编译
#   PICO_SDK_PATH=/path/to/pico-sdk ./build.sh  # 显式指定 SDK 路径
#   UART_BAUD=921600 ./build.sh                 # 可选：覆盖 UART 波特率（默认 2M）
set -euo pipefail

cd "$(dirname "$0")"

# ---- 参数 ----
UPDATE_HIDKIT=0
UPDATE_HIDKIT_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --update-hidkit)      UPDATE_HIDKIT=1 ;;
        --update-hidkit-only) UPDATE_HIDKIT=1; UPDATE_HIDKIT_ONLY=1 ;;
        -h|--help)
            cat <<'USAGE'
用法：./build.sh [选项]
  无选项                 按父仓库记录的提交构建
  --update-hidkit        把 lib/hidkit 与 lib/hidkit-tusb-xinput 跟到远端最新，
                         有变化则提交并推送父仓库指针，然后继续构建
  --update-hidkit-only   只做上面的子模块更新（+提交/推送），不编译
  -h, --help             显示本帮助

环境变量：
  PICO_SDK_PATH=<路径>   SDK 位置（默认探测 ../pico-sdk 与 ~/pico-sdk）
  UART_BAUD=<波特率>     覆盖固件默认 2000000（须与上位机一致）
  HIDKIT_NO_PUSH=1       更新时只提交指针、不 push（离线/手动推送时用）
USAGE
            exit 0
            ;;
        *)
            echo "错误：未知参数 '$arg'（可用：--update-hidkit / --update-hidkit-only；./build.sh --help 看全部）" >&2
            exit 1
            ;;
    esac
done

# ---- 定位 Pico SDK：环境变量优先，其次探测常见位置 ----
if [ -z "${PICO_SDK_PATH:-}" ]; then
    for cand in ../pico-sdk "$HOME/pico-sdk"; do
        if [ -d "$cand" ]; then
            PICO_SDK_PATH="$(cd "$cand" && pwd)"
            echo ">> 未设置 PICO_SDK_PATH，使用探测到的：$PICO_SDK_PATH"
            break
        fi
    done
fi
if [ -z "${PICO_SDK_PATH:-}" ] || [ ! -d "$PICO_SDK_PATH" ]; then
    echo "错误：未找到 Pico SDK。请安装并指定路径后重试：" >&2
    echo "  git clone https://github.com/raspberrypi/pico-sdk" >&2
    echo "  PICO_SDK_PATH=/path/to/pico-sdk ./build.sh" >&2
    exit 1
fi
export PICO_SDK_PATH

# ---- 可选：把两个 hidkit 子模块跟到远端最新，并把指针变更提交/推送 ----
# 只在显式传参时执行：构建本身绝不偷偷动子模块，版本变更要能落在 git 历史里。
#
# 行为：对 lib/hidkit 与 lib/hidkit-tusb-xinput
#   1) fetch origin 并切到 origin/main
#   2) 指针有变化 → git add + commit（消息含 旧 -> 新）
#   3) 有 upstream → git push（HIDKIT_NO_PUSH=1 时只提交不推送）
# 这样"更新 + 构建"不会被构建期的 git submodule update 复位回旧指针。
#
# **不含 lib/Pico-PIO-USB**：那个是故意钉死在旧血脉 9510f79 的（0.6.0 重写会
# 破坏 hub 上设备拔出，见 README「PIO-USB 版本」），跟远端走等于把它升坏。
# 所以这里逐个点名子模块，不用 --remote 的全量形式。
update_hidkit() {
    if ! git -C . rev-parse --git-dir >/dev/null 2>&1; then
        echo "错误：--update-hidkit 需要 git 仓库（子模块方式）" >&2
        exit 1
    fi
    branch="$(git symbolic-ref --quiet --short HEAD || true)"
    if [ -z "$branch" ]; then
        echo "错误：父仓库处于 detached HEAD，无法提交/推送子模块指针" >&2
        exit 1
    fi

    echo ">> 更新 hidkit 子模块到远端最新（跟 origin/main；本地未 push 的提交带不进来）"
    changed=""
    detail=""
    for sub in lib/hidkit lib/hidkit-tusb-xinput; do
        # 先归位到父仓库记录的提交（顺带完成首次初始化）
        git submodule update --init --recursive "$sub" >/dev/null 2>&1 || true
        old="$(git -C "$sub" rev-parse --short HEAD 2>/dev/null || echo none)"

        if [ -n "$(git -C "$sub" status --porcelain)" ]; then
            echo "错误：$sub 有未提交改动，先 commit / stash 再更新" >&2
            exit 1
        fi
        if ! git -C "$sub" fetch --quiet origin; then
            echo "错误：$sub fetch 失败（网络或权限问题？）" >&2
            exit 1
        fi
        # 显式跟 origin/main，不依赖 origin/HEAD 是否设置过
        if ! git -C "$sub" rev-parse --verify --quiet origin/main >/dev/null; then
            echo "错误：$sub 取不到 origin/main" >&2
            exit 1
        fi
        git -C "$sub" checkout --quiet origin/main
        new="$(git -C "$sub" rev-parse --short HEAD)"

        # 父仓库记录的提交若不在远端，本次更新会把它从指针上甩掉。
        # 常见成因：上一次是用"本地 fetch"把未推送的提交记进来的
        if ! git -C "$sub" merge-base --is-ancestor "$old" origin/main 2>/dev/null; then
            echo "   ⚠ $sub 原指针 $old 不在远端（本地未 push 的提交？）"
        fi

        if [ "$old" = "$new" ]; then
            echo "   $sub: 已是最新（${new}）"
        else
            echo "   $sub: $old -> $new"
            changed="$changed $sub"
            detail="$detail
$sub: $old -> $new"
        fi
    done

    if [ -z "$changed" ]; then
        echo "   指针已在远端最新，无需提交"
        # 上次 push 失败时在这里补一次
        if [ "${HIDKIT_NO_PUSH:-0}" != "1" ] && git rev-parse --verify --quiet '@{u}' >/dev/null; then
            if [ "$(git rev-list --count '@{u}..HEAD')" != "0" ]; then
                push_ok=1
                git push || push_ok=0
                if [ "$push_ok" = "1" ]; then
                    echo "   -> 已补 push 之前的本地提交"
                else
                    echo "   ⚠ push 失败：本地提交仍在，稍后手动 git push" >&2
                fi
            fi
        fi
    else
        git add $changed
        git commit -m "chore: 子模块跟进到远端最新（hidkit）$detail"
        echo "   -> 已提交到父仓库（${branch}）"

        if [ "${HIDKIT_NO_PUSH:-0}" = "1" ]; then
            echo "   HIDKIT_NO_PUSH=1：跳过 push（稍后手动 git push）"
        else
            push_ok=1
            if git rev-parse --verify --quiet '@{u}' >/dev/null; then
                git push || push_ok=0
            else
                git push -u origin "$branch" || push_ok=0
            fi
            if [ "$push_ok" = "1" ]; then
                echo "   -> 已 push 到远端"
            else
                echo "   ⚠ push 失败：本地提交已保留，网络恢复后手动 git push 即可" >&2
            fi
        fi
    fi

    echo "   未改动 lib/Pico-PIO-USB（故意钉死，见 README「PIO-USB 版本」）"
}

if [ "$UPDATE_HIDKIT" = 1 ]; then
    update_hidkit
    if [ "${UPDATE_HIDKIT_ONLY:-0}" = "1" ]; then
        exit 0
    fi
fi

# ---- 子模块与 PIO-USB 补丁（幂等；补丁原因见 patches/pio_usb/README.md）----
# clone 时没带 --recurse-submodules 也能一键跑通：这里补初始化 + 打补丁。
# 非 git 方式（源码包）获取时跳过，交由 CMake 的检查给出提示。
if git -C . rev-parse --git-dir >/dev/null 2>&1; then
    echo ">> 同步子模块与 PIO-USB 补丁"
    ./scripts/apply-patches.sh
fi

# ---- 可选参数：UART_BAUD 等环境变量转发为 CMake 缓存变量 ----
# 未设置时显式清空缓存项：缓存值会跨次构建持久存在，
# 不清空则历史上的 UART_BAUD 会一直覆盖固件默认值（uart_output.h）
CMAKE_ARGS=()
if [ -n "${UART_BAUD:-}" ]; then
    CMAKE_ARGS+=("-DUART_BAUD=${UART_BAUD}")
else
    CMAKE_ARGS+=("-DUART_BAUD=")
fi

echo ">> 配置（SDK：${PICO_SDK_PATH}）"
# 兼容 bash 3.2（macOS 自带）：空数组在 set -u 下的安全展开
cmake -S . -B build ${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}

echo ">> 编译"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build build -j "$NPROC"

UF2="build/src/pico-hid-debugger.uf2"
if [ -f "$UF2" ]; then
    echo ">> 完成：$UF2"
    echo "   按住 BOOTSEL 插入 Pico，将 .uf2 拷贝到出现的 U 盘即可刷入"
fi
