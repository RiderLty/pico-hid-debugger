#!/usr/bin/env bash
# pico-hid-debugger 一键构建脚本（git clone 后无需任何手工配置）
#
# 用法：
#   ./build.sh                                  # SDK 自动探测
#   PICO_SDK_PATH=/path/to/pico-sdk ./build.sh  # 显式指定 SDK 路径
#   UART_BAUD=921600 ./build.sh                 # 可选：覆盖 UART 波特率（默认 2M）
set -euo pipefail

cd "$(dirname "$0")"

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
