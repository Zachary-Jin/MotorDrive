#!/bin/sh
# 拉取编译所需的 HAL 依赖。
#
# 为什么要这个脚本，而不是一句 `git submodule update --init --recursive`：
# STM32CubeF1 自己内部还挂着一堆嵌套 submodule（各评估板的 BSP、FreeRTOS、
# LwIP、FatFs、USB 库……），本工程一个都用不到。全部递归拉下来是 200 MB 以上，
# 而编译真正需要的只有下面这两个：
#
#   Drivers/STM32F1xx_HAL_Driver        HAL 驱动源码与头文件
#   Drivers/CMSIS/Device/ST/STM32F1xx   CMSIS 器件头（stm32f1xx.h 等）
#
# 其余需要的文件（CMSIS 内核头、启动文件、system_stm32f1xx.c）都在 CubeF1
# 主仓库里，不是嵌套 submodule，`git submodule update --init` 一层就够了。
#
# 合起来约 18 MB。脚本最后会逐个校验编译真正用到的文件是否到位。

set -eu

cd "$(dirname "$0")"

CUBE=STM32CubeF1
NESTED_DEPS="Drivers/STM32F1xx_HAL_Driver Drivers/CMSIS/Device/ST/STM32F1xx"

echo "==> 1/3 拉取 STM32CubeF1（子模块本身）"
git submodule update --init "$CUBE"

echo
echo "==> 2/3 拉取编译必需的嵌套 submodule"
# 只在目录还是空的时候拉，已初始化过的跳过，避免重复下载。
for dep in $NESTED_DEPS; do
    if [ -n "$(ls -A "$CUBE/$dep" 2>/dev/null)" ]; then
        echo "    跳过（已存在）：$dep"
    else
        echo "    拉取：$dep"
        git -C "$CUBE" submodule update --init "$dep"
    fi
done

echo
echo "==> 3/3 校验编译需要的文件"
missing=0
for f in \
    "$CUBE/Drivers/STM32F1xx_HAL_Driver/Inc/stm32f1xx_hal.h" \
    "$CUBE/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c" \
    "$CUBE/Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f1xx.h" \
    "$CUBE/Drivers/CMSIS/Include/core_cm3.h" \
    "$CUBE/Projects/STM32F103RB-Nucleo/Templates/SW4STM32/startup_stm32f103xb.s" \
    "$CUBE/Projects/STM32F103RB-Nucleo/Templates/Src/system_stm32f1xx.c"
do
    if [ -f "$f" ]; then
        echo "    OK   $f"
    else
        echo "    缺失 $f"
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    echo
    echo "依赖不完整。先确认网络通畅，再重跑本脚本。" >&2
    exit 1
fi

echo
echo "依赖就绪，现在可以编译："
echo "    make -C motor_controller"
