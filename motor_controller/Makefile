PROJECT := motor_controller
MCU := cortex-m3
TOOLCHAIN ?= $(ARM_GNU_TOOLCHAIN)
ifeq ($(strip $(TOOLCHAIN)),)
TOOLCHAIN := $(firstword $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi))
endif
ifeq ($(strip $(TOOLCHAIN)),)
CC := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE := arm-none-eabi-size
else
CC := $(TOOLCHAIN)/bin/arm-none-eabi-gcc
OBJCOPY := $(TOOLCHAIN)/bin/arm-none-eabi-objcopy
SIZE := $(TOOLCHAIN)/bin/arm-none-eabi-size
endif

CUBE := ../STM32CubeF1
HAL := $(CUBE)/Drivers/STM32F1xx_HAL_Driver
CMSIS := $(CUBE)/Drivers/CMSIS
DEVICE := $(CMSIS)/Device/ST/STM32F1xx
STARTUP := $(CUBE)/Projects/STM32F103RB-Nucleo/Templates/SW4STM32/startup_stm32f103xb.s
SYSTEM := $(CUBE)/Projects/STM32F103RB-Nucleo/Templates/Src/system_stm32f1xx.c

CFLAGS := -mcpu=$(MCU) -mthumb -mfloat-abi=soft -DSTM32F103xB -DUSE_HAL_DRIVER -Os -g3 \
  -ffunction-sections -fdata-sections -Wall -Wextra \
  -IInc -I$(HAL)/Inc -I$(CMSIS)/Include -I$(DEVICE)/Include
LDFLAGS := -mcpu=$(MCU) -mthumb -mfloat-abi=soft -TSTM32F103C8Tx_FLASH.ld \
  -Wl,--gc-sections -Wl,-Map=$(PROJECT).map --specs=nano.specs --specs=nosys.specs

HAL_SRC := $(addprefix $(HAL)/Src/, \
  stm32f1xx_hal.c stm32f1xx_hal_cortex.c stm32f1xx_hal_dma.c stm32f1xx_hal_flash.c \
  stm32f1xx_hal_flash_ex.c stm32f1xx_hal_gpio.c stm32f1xx_hal_rcc.c \
  stm32f1xx_hal_rcc_ex.c stm32f1xx_hal_tim.c stm32f1xx_hal_tim_ex.c \
  stm32f1xx_hal_uart.c)
SRC := Src/main.c $(SYSTEM) $(HAL_SRC)
# Keep object names flat; VPATH below resolves each source directory.
SRC_NAMES := $(notdir $(SRC))
OBJ := $(addprefix build/,$(SRC_NAMES:.c=.o)) build/startup.o
VPATH := Src:$(dir $(SYSTEM)):$(HAL)/Src

.PHONY: all clean flash
all: $(PROJECT).elf $(PROJECT).bin

$(PROJECT).elf: $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ -o $@
	$(SIZE) $@

$(PROJECT).bin: $(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/startup.o: $(STARTUP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

flash: $(PROJECT).bin
	@if [ "$(PROGRAMMER)" = "jlink" ]; then \
		JLinkExe -device STM32F103C8 -if SWD -speed 4000 -autoconnect 1 -CommanderScript jlink_flash.jlink; \
	elif [ "$(PROGRAMMER)" = "openocd" ]; then \
		openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
		-c "program $(PROJECT).bin 0x08000000 verify reset exit"; \
	elif [ -z "$(PROGRAMMER)" ] || [ "$(PROGRAMMER)" = "stlink" ]; then \
		st-flash write $(PROJECT).bin 0x08000000; \
	else \
		echo "Unknown PROGRAMMER='$(PROGRAMMER)'; use jlink, stlink, or openocd"; exit 2; \
	fi

clean:
	rm -rf build $(PROJECT).elf $(PROJECT).bin $(PROJECT).map
