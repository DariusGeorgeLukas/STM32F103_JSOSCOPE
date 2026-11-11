# Builds the Blue Pill firmware. Run from this directory, or from the project
# root with `make -C firmware`. The toolchain comes from PATH unless GCC_PATH
# points at another one.

TARGET = stm32f103_jsoscope
BUILD_DIR = build
OPT = -Os

C_SOURCES = \
Core/Src/main.c \
Core/Src/stm32f1xx_it.c \
Core/Src/stm32f1xx_hal_msp.c \
Core/Src/system_stm32f1xx.c \
Core/Src/sysmem.c \
Core/Src/syscalls.c \
USB_DEVICE/App/usb_device.c \
USB_DEVICE/App/usbd_desc.c \
USB_DEVICE/App/usbd_cdc_if.c \
USB_DEVICE/Target/usbd_conf.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_exti.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pcd.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pcd_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_ll_usb.c \
Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_core.c \
Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c \
Middlewares/ST/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c \
Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Src/usbd_cdc.c

CPP_SOURCES = \
src/app.cpp \
src/sampler.cpp \
src/scope_config.cpp \
../lib/jsoscope-device/src/protocol.cpp \
../lib/jsoscope-device/src/device_session.cpp

ASM_SOURCES = startup_stm32f103xb.s

PREFIX = arm-none-eabi-
ifdef GCC_PATH
PREFIX := $(GCC_PATH)/$(PREFIX)
endif
CC = $(PREFIX)gcc
CXX = $(PREFIX)g++
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size

MCU = -mcpu=cortex-m3 -mthumb
DEFINES = -DUSE_HAL_DRIVER -DSTM32F103xB
INCLUDES = \
-Isrc \
-I../lib/jsoscope-device/include \
-ICore/Inc \
-IUSB_DEVICE/App \
-IUSB_DEVICE/Target \
-isystem Drivers/STM32F1xx_HAL_Driver/Inc \
-isystem Drivers/STM32F1xx_HAL_Driver/Inc/Legacy \
-isystem Middlewares/ST/STM32_USB_Device_Library/Core/Inc \
-isystem Middlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
-isystem Drivers/CMSIS/Device/ST/STM32F1xx/Include \
-isystem Drivers/CMSIS/Include

COMMON_FLAGS = $(MCU) $(DEFINES) $(INCLUDES) $(OPT) -g -Wall -fdata-sections -ffunction-sections
DEPENDENCY_FLAGS = -MMD -MP -MF"$(@:%.o=%.d)"
CFLAGS = $(COMMON_FLAGS) -std=c11 $(DEPENDENCY_FLAGS)
CXXFLAGS = $(COMMON_FLAGS) -std=c++20 -Wextra -fno-exceptions -fno-rtti \
           -fno-threadsafe-statics $(DEPENDENCY_FLAGS)
ASFLAGS = $(MCU) $(DEFINES) -Wall

LDSCRIPT = STM32F103XX_FLASH.ld
LDFLAGS = $(MCU) -specs=nano.specs -specs=nosys.specs -T$(LDSCRIPT) \
          -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(CPP_SOURCES:.cpp=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
vpath %.cpp $(sort $(dir $(CPP_SOURCES)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.cpp Makefile | $(BUILD_DIR)
	$(CXX) -c $(CXXFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CXX) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf
	$(CP) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf
	$(CP) -O binary -S $< $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	-rm -fR $(BUILD_DIR)

# Flash with an ST-Link.
flash: $(BUILD_DIR)/$(TARGET).bin
	st-flash --reset write $< 0x8000000

flash_ocd: $(BUILD_DIR)/$(TARGET).elf
	openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
	    -c "program $< verify reset exit"

.PHONY: all clean flash flash_ocd

-include $(wildcard $(BUILD_DIR)/*.d)
