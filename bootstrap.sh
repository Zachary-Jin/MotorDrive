#!/bin/sh
# 拉取编译所需的 HAL 依赖。
#
# 为什么不直接 `git submodule update --init --recursive`：
# STM32CubeF1 自己内部还挂着一堆嵌套 submodule（各评估板的 BSP、FreeRTOS、
# LwIP、FatFs、USB 库……），本工程一个都用不到。递归会把它们全部拉下来。
# 编译真正需要的只有两个：
#
#   Drivers/STM32F1xx_HAL_Driver        HAL 驱动源码与头文件
#   Drivers/CMSIS/Device/ST/STM32F1xx   CMSIS 器件头（stm32f1xx.h 等）
#
# 其余需要的文件（CMSIS 内核头、启动文件、system_stm32f1xx.c）都在 CubeF1
# 主仓库里，不是嵌套 submodule，`git submodule update --init` 一层就够了。
#
# 不要改成 `--depth 1` 浅克隆：CubeF1 浅克隆后，它的嵌套 submodule 只会建出
# 一个空的目录（里面仅有一个 .git 文件），`git submodule status` 却报告已检出，
# 直到编译报 "stm32f1xx.h: No such file or directory" 才会发现。实测过，别试。
#
# 脚本最后会逐个校验编译真正用到的文件是否到位。

set -eu

cd "$(dirname "$0")"

CUBE=STM32CubeF1

echo "==> 1/3 拉取 STM32CubeF1（子模块本身，约 240 MB，首次较慢）"
git submodule update --init "$CUBE"

echo
echo "==> 2/3 拉取编译必需的嵌套 submodule"
# 用具体的头文件判断是否已拉过，而不是判断目录非空：submodule 没检出时目录里
# 仍会留下一个 .git 文件，只看"目录非空"会把空壳误判成已就绪。
for dep in \
    "Drivers/STM32F1xx_HAL_Driver:Inc/stm32f1xx_hal.h" \
    "Drivers/CMSIS/Device/ST/STM32F1xx:Include/stm32f1xx.h"
do
    path="${dep%%:*}"
    probe="${dep##*:}"
    if [ -f "$CUBE/$path/$probe" ]; then
        echo "    跳过（已存在）：$path"
    else
        echo "    拉取：$path"
        git -C "$CUBE" submodule update --init "$path"
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
