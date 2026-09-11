#!/usr/bin/env bash
# pico-hid-debugger 一键构建脚本（git clone 后无需任何手工配置）
#
# 用法：
#   ./build.sh                                  # SDK 自动探测
#   ./build.sh --update-hidkit                  # 先把两个 hidkit 子模块跟到远端最新
#   PICO_SDK_PATH=/path/to/pico-sdk ./build.sh  # 显式指定 SDK 路径
#   UART_BAUD=921600 ./build.sh                 # 可选：覆盖 UART 波特率（默认 2M）
set -euo pipefail

cd "$(dirname "$0")"

# ---- 参数 ----
UPDATE_HIDKIT=0
for arg in "$@"; do
    case "$arg" in
        --update-hidkit) UPDATE_HIDKIT=1 ;;
        -h|--help)
            cat <<'USAGE'
用法：./build.sh [选项]
  无选项                 按父仓库记录的提交构建
  --update-hidkit        先把 lib/hidkit 与 lib/hidkit-tusb-xinput 跟到远端最新，
                         再构建（改完 hidkit 想同步到本工程时用这个）
  -h, --help             显示本帮助

环境变量：
  PICO_SDK_PATH=<路径>   SDK 位置（默认探测 ../pico-sdk 与 ~/pico-sdk）
  UART_BAUD=<波特率>     覆盖固件默认 2000000（须与上位机一致）
USAGE
            exit 0
            ;;
        *)
            echo "错误：未知参数 '$arg'（可用：--update-hidkit；./build.sh --help 看全部）" >&2
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

# ---- 可选：把两个 hidkit 子模块跟到远端最新（--update-hidkit）----
# 只在显式传参时执行：构建本身绝不偷偷动子模块，版本变更要能落在 git 历史里。
#
# **不含 lib/pico_pio_usb**：那个是故意钉死在旧血脉 9510f79 的（0.6.0 重写会
# 破坏 hub 上设备拔出，见 README「PIO-USB 版本」），跟远端走等于把它升坏。
# 所以这里逐个点名子模块，不用 --remote 的全量形式。
if [ "$UPDATE_HIDKIT" = 1 ]; then
    if ! git -C . rev-parse --git-dir >/dev/null 2>&1; then
        echo "错误：--update-hidkit 需要 git 仓库（子模块方式）" >&2
        exit 1
    fi
    echo ">> 更新 hidkit 子模块到远端最新"
    # 注意跟的是 **origin/main**：在 ~/hidkit 里改了但没 push 的提交，这里拿不到
    echo "   （跟 origin/main；本地未 push 的 hidkit 提交不会被带进来）"
    for sub in lib/hidkit lib/hidkit-tusb-xinput; do
        # 先归位到父仓库记录的提交（顺带完成首次初始化），再往前推 ——
        # 这样下面报的 "旧 -> 新" 就是"指针记录值 -> 远端最新"
        git submodule update --init --recursive "$sub"
        old="$(git -C "$sub" rev-parse --short HEAD)"

        git -C "$sub" fetch origin
        # 显式跟 origin/main，不依赖 origin/HEAD 是否设置过
        if ! git -C "$sub" rev-parse --verify --quiet origin/main >/dev/null; then
            echo "错误：$sub 取不到 origin/main（网络或权限问题？）" >&2
            exit 1
        fi
        git -C "$sub" checkout --quiet origin/main
        new="$(git -C "$sub" rev-parse --short HEAD)"

        if [ "$old" = "$new" ]; then
            echo "   $sub: 已是最新（${new}）"
        else
            echo "   $sub: $old -> $new"
        fi

        # 父仓库记录的提交若不在远端，本次更新会把它从指针上甩掉。
        # 常见成因：上一次是用"本地 fetch"把未推送的提交记进来的 —— 那种指针
        # 别人 clone 后 submodule update 会直接报 "reference is not a git commit"
        if ! git -C "$sub" merge-base --is-ancestor "$old" origin/main 2>/dev/null; then
            echo "   ⚠ 父仓库记录的 $old 不在远端：本次更新会把它甩掉"
            echo "     （若那是本地未推的提交，先 push 再跑本参数）"
        fi
    done
    echo "   未改动 lib/pico_pio_usb（故意钉死，见 README「PIO-USB 版本」）"
    echo "   注意：这只是把子模块工作区切过去了，父仓库还没记录。构建完记得："
    echo "     git add lib/hidkit lib/hidkit-tusb-xinput"
    echo "     git commit -m \"子模块跟进 hidkit <提交>\""
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
