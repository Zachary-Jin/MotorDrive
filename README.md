# MotorDrive — STM32F103 双路电机升降控制

基于 **STM32F103C8T6**（Blue Pill）和 **HC-160A S2** 双路 H 桥模块的开环升降控制固件。

两路电机**同时启动、同步换向**，每 5 秒反转一次，上电即开始运行。
方向由四个 GPIO 直接控制；速度脚输出 **15 kHz、98% 占空比**的方波（硬件要求，见下），
占空比是编译期常量，**运行时不做任何调速**。

> ⚠️ **这是开环控制，没有位置反馈、没有过流保护、没有软件急停。**
> 上电瞬间两路电机立即全速运行。首次上电请先拔掉电机线。详见[安全说明](#安全说明)。

---

## 功能

- 上电自动进入往复模式：两路同步**正转 5 秒 → 反转 5 秒**，循环。
- 上/下限位开关触发时，对应方向立即刹车。
- `UP` / `DOWN` 按钮可手动控制（按住运行，松开停止），并退出自动模式。
- USART1（115200 8N1）串口命令：定时运行、手动方向、刹车、恢复自动模式。
- 启动时通过串口打印**上次复位原因**，用于诊断换向冲击是否把供电拉跌。
- 方向脚用单次 `GPIOB->BSRR` 写入成对更新，换向过程中**绝不会**出现
  `A=1,B=1` 这个厂家真值表里未定义的状态。

## 硬件

| 部件 | 型号 | 说明 |
| --- | --- | --- |
| MCU 开发板 | STM32F103C8T6 | Blue Pill，64 KiB Flash / 20 KiB RAM |
| 电机驱动 | HC-160A S2 | 双路 H 桥，控制排针 + 功率排针 |
| 调试器 | J-Link（SWD） | 也支持 ST-LINK / OpenOCD |
| 电源 | DC 9–30 V | 接驱动板功率排针 `+9V-30V`，**不能给 STM32 供电** |

### HC-160A S2 接口

- **控制排针**（接开发板）：`+`、`-`、`B`、`A`、`a`、`b`，以及**两个 `P` 脚**
- **功率排针**（接电源和电机）：`GND(-)`、`A`、`B`、`a`、`b`、`+9V-30V`

`A`/`B` 是通道 1 的两个方向输入，`a`/`b` 是通道 2 的；每路各有一个 `P` 脚作为**速度输入**。

| A | B | 通道 1 输出 |
| --- | --- | --- |
| 1 | 0 | 正转 |
| 0 | 1 | 反转 |
| 0 | 0 | **刹车**（绕组被低边管短接） |
| 1 | 1 | **未定义，固件绝不会产生** |

#### `P` 脚要的是 PWM 方波，不是静态电平

厂家文档原文：

> **PA为PWM波输入**；G控制板共地引脚；**频率最高为60KHZ；占空比最高达98%**；
> 5V.3.3V单片机均可：单电源工作，只需接一个电机电源，和控制板需要共地即可，**不用5V等电源**。

原文出自商家商品详情图 [`docs/hc-160a-s2-vendor-spec.jpg`](docs/hc-160a-s2-vendor-spec.jpg)
（这块板没有官方数据手册，这是目前唯一的厂家资料）。

给静态高电平就是 **100% 占空比，超出 98% 上限**，模块会**完全不驱动桥臂**——
电机端恒为 0V，方向脚怎么翻都没反应。**这是实测确认过的现象。**

所以固件用 TIM1 产生真方波：**15 kHz、98% 占空比**（厂家上限），两个通道各一路。
98% 下电机看到 `0.98 × Vs`，实际就是全速。

`+` 是模块自己的 **5V 输出**（从电机电源转换），**悬空即可**；模块逻辑不需要外部供电。

## 接线

接线分两部分：**接到驱动板的只有 7 根**，其余是 STM32 自己的外设，
跟驱动板没有关系。

### A. 接 HC-160A S2 控制排针

| STM32F103C8 | HC-160A S2 | 说明 |
| --- | --- | --- |
| PA8 | `P`（通道 1 速度） | TIM1_CH1，**15 kHz 方波，98% 占空比** |
| PA11 | `P`（通道 2 速度） | TIM1_CH4，**15 kHz 方波，98% 占空比** |
| PB8 | `A`（通道 1 方向） | 推挽输出，每 5 秒翻转 |
| PB9 | `B`（通道 1 方向） | 推挽输出，每 5 秒翻转 |
| PB6 | `a`（通道 2 方向） | 推挽输出，每 5 秒翻转 |
| PB7 | `b`（通道 2 方向） | 推挽输出，每 5 秒翻转 |
| GND | 控制排针 `-` | **必须共地** |

电机两根线接**功率排针**对应通道的端子（`A`/`B` 或 `a`/`b`）。
电机大电流回路直接走功率侧 `GND(-)`，**不要**过控制排针的细线。

### B. STM32 自己的外设（都不接驱动板）

这四个脚在驱动板上**没有对应端子**，是直接接在 STM32 上的。
全部使用内部上拉，所以每个开关只要两根线（信号 + GND），不需要外部电阻。

| STM32F103C8 | 接到 | 说明 |
| --- | --- | --- |
| PB10 | 下限位开关 | 装在机构行程下端，另一端接 GND，触发时拉低 |
| PB11 | 上限位开关 | 装在机构行程上端，另一端接 GND，触发时拉低 |
| PB12 | UP 按钮 | 另一端接 GND，按下拉低 |
| PB13 | DOWN 按钮 | 另一端接 GND，按下拉低 |
| PA9 / PA10 | USB-TTL 模块的 RX / TX | 115200 8N1，接电脑用 |

> **这四样全都不接也能跑。** 内部上拉会让它们恒为"未触发/未按下"：
> 5 秒自动反转照常工作，只是没有行程保护、没有手动按钮、看不到串口输出。
> 想让电机转起来，只要接好 A 表里那 7 根线就够了。
>
> ⚠️ 但**不接限位就没有行程保护**：如果机构会撞到机械死点，电机要堵转到
> 5 秒时间到才反转，期间会发热、可能拉跌供电。行程短于 5 秒能走的距离时，
> 要么接上限位开关，要么把 `AUTO_INTERVAL_MS` 改小。

### 为什么六个信号脚都选在带 FT 的引脚上

STM32F103 上 **PA0–PA7、PB0、PB1、PC13–PC15 不带 FT（5V 容忍）标记**，
输入绝对最大额定只有 `VDD+0.3V ≈ 3.6V`。从上电复位到 `GPIO_Init()` 跑完约有
**2–3 ms**，这期间所有 GPIO 都是**浮空输入**。如果驱动模块的控制输入自带 5V 上拉，
非 FT 脚就会被拉到 5V 而超出额定。

把六个信号脚全部放在 FT 脚（PB6–PB9、PA8、PA11）上，整条控制接口在复位窗口期
都不怕 5V，不必去赌模块内部有没有上拉。**不要改回 PA0/PB0/PB1。**

> **实测更正（2026-09-16）**：给功率侧上电、控制排针悬空时，六个控制脚对 `-` 都只有
> **0.2 V 左右**，**这块板子上并没有 5V 上拉**。所以当初挪脚是**保守选择，不是修复一个
> 已观测到的故障**——之前文档把它写成"已确认的危险"，是过头了。
> 不建议改回去：挪脚没有代价，万一换一批板子带了上拉，这个选择能救命。

## 上电前用万用表确认

**2026-09-16 已实测确认**：`+` 是 **5V 输出**（悬空即可，模块不需要外部逻辑供电）；
控制输入**没有** 5V 上拉；**3.3V 逻辑可以直接驱动**；`P` 脚是 PWM 输入
（最高 60 kHz，占空比**上限 98%**）。完整实测表见
[`motor_controller/README.md`](motor_controller/README.md)。

接线前仍要确认：

1. **断电**，通断档：控制 `-` 应与功率侧 `GND(-)` 相通；`GND(-)` 应与大电解电容负极相通。
2. **绝不把控制排针的 `+` 接进 STM32 的 5V 或 3.3V** —— 它是 5V 输出，会和 Blue Pill
   的 3.3V 稳压器对打。
3. 若是"单电机"调试，把 `CHANNEL_B_ENABLED` 改成 `0` 重新编译。

## 快速开始

### 1. 克隆并拉取依赖

HAL 库以 git submodule 形式引入，指向 ST 官方的
[STM32CubeF1](https://github.com/STMicroelectronics/STM32CubeF1)。
**不拉取子模块就无法编译。**

```sh
git clone https://github.com/Zachary-Jin/MotorDrive.git
cd MotorDrive
./bootstrap.sh
```

> **不要用 `git clone --recurse-submodules`。**
> STM32CubeF1 内部还挂着一堆嵌套 submodule（各评估板的 BSP、FreeRTOS、LwIP、
> FatFs、USB 库……），本工程一个都用不到。`bootstrap.sh` 只拉编译真正需要的
> 那两个（HAL 驱动、CMSIS 器件头），最后会逐个校验文件是否到位。
>
> STM32CubeF1 本身约 **240 MB**（绝大部分是官方仓库的完整工作区，历史只占约
> 40 MB），首次拉取需要几分钟，属正常现象。
> **不要改用 `--depth 1` 浅克隆来省这几十 MB** —— 实测 CubeF1 一旦浅克隆，
> 它的嵌套 submodule 只会建出一个空目录（里面仅剩一个 `.git` 文件），
> `git submodule status` 却报告"已检出"，一直要等到编译报
> `stm32f1xx.h: No such file or directory` 才会暴露。详见 `bootstrap.sh` 注释。

### 2. 安装工具链

需要**包含 newlib** 的 GNU Arm Embedded Toolchain、`make`，以及可选的 SEGGER J-Link 软件。

> Homebrew 的 `arm-none-eabi-gcc` 公式是按 `--without-headers` 构建的 compiler-only 包，
> 缺少 newlib，**不能用于本工程**。请安装官方 ARM GNU Toolchain：

```sh
brew install --cask gcc-arm-embedded   # 会打开需要管理员密码的 .pkg 安装器
# 或从 ARM 官网下载 .pkg 手动安装
```

安装后确认（必须输出完整路径，不能只回显 `libc.a`）：

```sh
export ARM_GNU_TOOLCHAIN=/Applications/ArmGNUToolchain/15.3.rel1/arm-none-eabi
export PATH="$ARM_GNU_TOOLCHAIN/bin:$PATH"
arm-none-eabi-gcc -print-file-name=libc.a
```

### 3. 编译

```sh
make -C motor_controller clean
make -C motor_controller
```

产物为 `motor_controller/motor_controller.elf` 和 `.bin`。
链接阶段会出现 `_close`/`_read`/`_write`/`_lseek` not implemented 和 RWX segment
两类提示，来自 `--specs=nosys.specs`，属既有现象，与本工程代码无关。

未设置 `ARM_GNU_TOOLCHAIN` 时 Makefile 会自动搜索 `/Applications/ArmGNUToolchain/*/`，
也可以显式指定：

```sh
make -C motor_controller TOOLCHAIN=/Applications/ArmGNUToolchain/15.3.rel1/arm-none-eabi
```

### 4. 烧录

```sh
make -C motor_controller flash PROGRAMMER=jlink    # 默认，需 JLinkExe 在 PATH 中
make -C motor_controller flash PROGRAMMER=stlink   # st-flash
make -C motor_controller flash PROGRAMMER=openocd  # openocd
```

J-Link SWD 接线（开发板底部 4 针排针）：

| J-Link | STM32F103C8T6 | 说明 |
| --- | --- | --- |
| SWDIO | PA13 | 数据 |
| SWCLK | PA14 | 时钟 |
| GND | GND | 共地 |
| VTref | 3.3V | 电平参考；是否给板供电取决于 J-Link 型号 |
| nRESET | NRST | 建议连接 |

## 串口命令

115200 8N1，命令以回车或换行结束，时间单位为毫秒。

| 命令 | 作用 |
| --- | --- |
| `A+5000` / `A-3000` | A 路定时正转 / 反转 |
| `B+5000` / `B-3000` | B 路定时正转 / 反转 |
| `X+10000` / `X-10000` | 两路同时定时正转 / 反转 |
| `A0` / `B0` / `X0` | **刹车**对应路（不是断电） |
| `u` / `d` / `s` | 两路持续正转 / 反转 / 刹车，并退出自动模式 |
| `r` | 重新进入自动模式：正反各 5 秒循环 |

不写时间表示持续运行，直到发送停止命令。到达限位会提前刹车。

## 编译期开关

定义在 `motor_controller/Src/main.c` 顶部。

| 宏 | 默认 | 作用 |
| --- | --- | --- |
| `CHANNEL_B_ENABLED` | `1` | 置 `0` 停用通道 2（TIM1_CH4 占空比设为 0，`a`/`b` 停在刹车），用于单电机调试 |
| `REVERSE_DEAD_TIME_MS` | `0U` | 换向前先刹车多少毫秒；`0` = 直接反向 |
| `AUTO_START_DIRECTION` | `1` | `1` = 上电先正转，`-1` = 先反转 |
| `PWM_FREQ_HZ` | `15000U` | 速度脚方波频率；厂家上限 60 kHz |
| `PWM_DUTY_PERCENT` | `98U` | 速度脚占空比；**厂家上限就是 98，不要改大** |

**关于 `REVERSE_DEAD_TIME_MS`**：瞬间反转时电源电压与电机反电动势叠加，电流峰值约为
**2 倍堵转**；先刹车一段时间可降到约 **1 倍堵转**并让其衰减。但死区**不能消除**冲击 ——
刹车阶段本身的电流就是 `反电动势/内阻`，同样是 1 倍堵转。设置后周期变为
`5000 + REVERSE_DEAD_TIME_MS`。

**判据**：若串口反复打印 `RESET: POR/BROWNOUT`，说明换向冲击确实把供电拉跌了，
把 `REVERSE_DEAD_TIME_MS` 改成 `200U` 重新编译即可。
（STM32F103 **没有**掉电复位 BOR 标志位，那是 F2/F4 才有的；F103 上供电跌落表现为 `PORRSTF`。）

## 仓库结构

```text
.
├── motor_controller/            应用工程
│   ├── Src/main.c               全部应用逻辑
│   ├── Inc/                     main.h、stm32f1xx_hal_conf.h
│   ├── STM32F103C8Tx_FLASH.ld   链接脚本（64K Flash / 20K RAM）
│   ├── Makefile                 构建与烧录
│   ├── jlink_flash.jlink        J-Link 烧录脚本
│   └── README.md                接线核验步骤与详细说明
├── docs/
│   └── hc-160a-s2-vendor-spec.jpg  厂家商品详情图（接口定义、真值表、频率/占空比规格）
├── bootstrap.sh                 拉取 HAL 依赖（见上）
├── STM32CubeF1/                 ST 官方 HAL 库（git submodule，锁定 bb2016e）
└── HANDOFF.md                   交接文档：硬件未确认项、已知风险、调试建议
```

## 安全说明

这是开环控制示例，**限位开关、急停和机械保护必须独立于软件设计**。

- **上电即两路全速运行**，没有软件急停。首次通电建议抬起电机或串入限流电源。
- **`s` / `X0` / `A0` 是刹车，不是断电。** 速度脚持续输出 98% 方波，桥臂一直带电，
  被负载反拖时会发热，不能替代机械限位。
- **换向冲击**：默认无死区且两路同时换向。建议限流电源先设 1–2 A、
  功率侧并大容量电解电容、串保险丝。
- **绝不从 `+9V-30V` 给 STM32 供电** —— Blue Pill 板载是 5V→3.3V LDO，9V 灌进去直接烧。
- **绝不把控制排针的 `+` 接进 STM32 的 5V 或 3.3V。** 它是模块自己的 5V 输出。
- **不要为了"更保险"把占空比改成 100%**（把速度脚写成静态高电平）。100% 超出厂家
  98% 的上限，模块会**完全不驱动桥臂**。这曾经是真实故障，见上文 «`P` 脚要的是 PWM 方波»。
- 不要在无人看守时让 `u`/`d` 命令长期保持。

## 文档

- [`motor_controller/README.md`](motor_controller/README.md) — 接线核验步骤、命令详解、工具链细节
- [`HANDOFF.md`](HANDOFF.md) — 硬件未确认事项、已知风险清单、按优先级的调试步骤

## 许可

本工程的应用代码可自由使用。`STM32CubeF1/` 子模块为 STMicroelectronics 所有，
遵循其自身的许可条款（见子模块内的 `LICENSE.md`）。
