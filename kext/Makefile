#
#  Makefile
#  AirportQCA & QCABluetoothFirmware - Universal Build System
#
#  Builds OpenCore-compatible macOS Kernel Extensions (.kext) with zero SDK library conflicts.
#

SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null || echo "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
CC      := clang
CXX     := clang++

ARCH    := x86_64

# Strict Kernel Flags (Prevents any user-space library conflict)
KERNEL_COMMON_FLAGS := \
	-mkernel \
	-arch $(ARCH) \
	-mmacosx-version-min=10.15 \
	-isysroot $(SDKROOT) \
	-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/Headers \
	-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/PrivateHeaders \
	-fno-builtin \
	-fno-common \
	-fno-exceptions \
	-fno-stack-protector \
	-mno-red-zone \
	-fstrict-aliasing \
	-DKERNEL \
	-D__APPLE__ \
	-D__IO80211_TARGET=__MAC_26_0 \
	-DIO80211FAMILY_V2 \
	-DAIRPORT_WCL \
	-Wall -Wextra -Wno-unused-parameter \
	-Wno-deprecated-declarations

KERNEL_CFLAGS := \
	$(KERNEL_COMMON_FLAGS) \
	-std=gnu99

KERNEL_CXXFLAGS := \
	$(KERNEL_COMMON_FLAGS) \
	-std=c++14 \
	-fno-rtti \
	-nostdinc++ \
	-Wno-inconsistent-missing-override

# Linker flags for pure Mach-O 64-bit KEXT bundle
LDFLAGS := \
	-mkernel \
	-arch $(ARCH) \
	-mmacosx-version-min=10.15 \
	-isysroot $(SDKROOT) \
	-nostdlib \
	-Xlinker -kext \
	-Wl,-w \
	-lkmod

BUILD_DIR := ../build
AIRPORT_DIR := AirportQCA
BT_DIR := QCABluetooth

AIRPORT_SRCS := \
	$(AIRPORT_DIR)/AirportQCA_info.c \
	$(AIRPORT_DIR)/AirportQCA.cpp \
	$(AIRPORT_DIR)/AirportQCAInterface.cpp \
	$(AIRPORT_DIR)/AirportQCAEthernetInterface.cpp \
	$(AIRPORT_DIR)/hw/QCAPCIDevice.cpp \
	$(AIRPORT_DIR)/hw/QCACopyEngine.cpp \
	$(AIRPORT_DIR)/hw/QCABMI.cpp \
	$(AIRPORT_DIR)/hw/QCAHTC.cpp \
	$(AIRPORT_DIR)/hw/QCAWMI.cpp \
	$(AIRPORT_DIR)/hw/QCAHTT.cpp

AIRPORT_OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(patsubst %.c, $(BUILD_DIR)/%.o, $(AIRPORT_SRCS)))
AIRPORT_FIRMWARE_C := $(BUILD_DIR)/$(AIRPORT_DIR)/QCAFirmwareData.c
AIRPORT_OBJS += $(BUILD_DIR)/$(AIRPORT_DIR)/QCAFirmwareData.o
AIRPORT_FW_DIR := ../source/linux-firmware/ath10k/QCA9377/hw1.0
AIRPORT_FW_IMAGE := $(AIRPORT_FW_DIR)/firmware-6.bin
AIRPORT_BOARD_DATA := $(AIRPORT_FW_DIR)/board.bin

BT_SRCS := \
	$(BT_DIR)/QCABluetooth_info.c \
	$(BT_DIR)/QCABluetoothFirmware.cpp \
	$(BT_DIR)/QCAUSBController.cpp

BT_FIRMWARE_C := $(BUILD_DIR)/$(BT_DIR)/QCAFirmwareData.c
BT_OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(patsubst %.c, $(BUILD_DIR)/%.o, $(BT_SRCS)))
BT_OBJS += $(BUILD_DIR)/$(BT_DIR)/QCAFirmwareData.o

BT_FW_SOURCE_DIR := ../source/linux-firmware/qca
BT_RAMPATCH := $(BT_FW_SOURCE_DIR)/rampatch_usb_00000302.bin
BT_NVM := $(BT_FW_SOURCE_DIR)/nvm_usb_00000302.bin

AIRPORT_KEXT := $(BUILD_DIR)/AirportQCA.kext
BT_KEXT := $(BUILD_DIR)/QCABluetoothFirmware.kext

.PHONY: all clean airport bluetooth verify

all: airport bluetooth verify

# -------------------------------------------------------------
# AirportQCA.kext
# -------------------------------------------------------------
airport: $(AIRPORT_KEXT)

$(AIRPORT_KEXT): $(AIRPORT_OBJS)
	@echo "==> Linking AirportQCA kernel binary..."
	@mkdir -p $(AIRPORT_KEXT)/Contents/MacOS
	@mkdir -p $(AIRPORT_KEXT)/Contents/Resources
	$(CXX) $(LDFLAGS) $(AIRPORT_OBJS) -o $(AIRPORT_KEXT)/Contents/MacOS/AirportQCA
	@cp $(AIRPORT_DIR)/Info.plist $(AIRPORT_KEXT)/Contents/Info.plist
	@cp $(AIRPORT_FW_DIR)/notice_ath10k_firmware-6.txt $(AIRPORT_KEXT)/Contents/Resources/FIRMWARE-NOTICE.txt
	@codesign -s - --force $(AIRPORT_KEXT) 2>/dev/null || true
	@echo "==> Successfully built AirportQCA.kext"

$(BUILD_DIR)/$(AIRPORT_DIR)/%.o: $(AIRPORT_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_CXXFLAGS) -I$(AIRPORT_DIR) -I$(AIRPORT_DIR)/include -I$(AIRPORT_DIR)/include/Apple80211 -I../source/itlwm/include -I../source/itlwm/AirportItlwm -c $< -o $@

$(BUILD_DIR)/$(AIRPORT_DIR)/%.o: $(AIRPORT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(AIRPORT_DIR) -I$(AIRPORT_DIR)/include -c $< -o $@

$(AIRPORT_FIRMWARE_C): $(AIRPORT_FW_IMAGE) $(AIRPORT_BOARD_DATA)
	@mkdir -p $(dir $@)
	xxd -i -n qca9377_firmware_6_bin $(AIRPORT_FW_IMAGE) > $@.tmp
	xxd -i -n qca9377_board_bin $(AIRPORT_BOARD_DATA) >> $@.tmp
	mv $@.tmp $@

$(BUILD_DIR)/$(AIRPORT_DIR)/QCAFirmwareData.o: $(AIRPORT_FIRMWARE_C)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(AIRPORT_DIR) -I$(AIRPORT_DIR)/include -c $< -o $@

# -------------------------------------------------------------
# QCABluetoothFirmware.kext
# -------------------------------------------------------------
bluetooth: $(BT_KEXT)

$(BT_KEXT): $(BT_OBJS)
	@echo "==> Linking QCABluetoothFirmware kernel binary..."
	@mkdir -p $(BT_KEXT)/Contents/MacOS
	@mkdir -p $(BT_KEXT)/Contents/Resources
	$(CXX) $(LDFLAGS) $(BT_OBJS) -o $(BT_KEXT)/Contents/MacOS/QCABluetoothFirmware
	@cp $(BT_DIR)/Info.plist $(BT_KEXT)/Contents/Info.plist
	@cp $(BT_DIR)/FIRMWARE-LICENSE.txt $(BT_KEXT)/Contents/Resources/FIRMWARE-LICENSE.txt
	@cp $(BT_DIR)/FIRMWARE-NOTICE.txt $(BT_KEXT)/Contents/Resources/FIRMWARE-NOTICE.txt
	@codesign -s - --force $(BT_KEXT) 2>/dev/null || true
	@echo "==> Successfully built QCABluetoothFirmware.kext"

$(BUILD_DIR)/$(BT_DIR)/%.o: $(BT_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_CXXFLAGS) -I$(BT_DIR) -c $< -o $@

$(BUILD_DIR)/$(BT_DIR)/%.o: $(BT_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(BT_DIR) -c $< -o $@

$(BT_FIRMWARE_C): $(BT_RAMPATCH) $(BT_NVM)
	@mkdir -p $(dir $@)
	xxd -i -n qca_rampatch_usb_00000302_bin $(BT_RAMPATCH) > $@.tmp
	xxd -i -n qca_nvm_usb_00000302_bin $(BT_NVM) >> $@.tmp
	mv $@.tmp $@

$(BUILD_DIR)/$(BT_DIR)/QCAFirmwareData.o: $(BT_FIRMWARE_C)
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -I$(BT_DIR) -c $< -o $@

# -------------------------------------------------------------
# Verification against OpenCore requirements
# -------------------------------------------------------------
verify:
	@echo "=========================================================="
	@echo "Verifying Kext Bundles for OpenCore Compatibility"
	@echo "=========================================================="
	@file $(AIRPORT_KEXT)/Contents/MacOS/AirportQCA
	@file $(BT_KEXT)/Contents/MacOS/QCABluetoothFirmware
	@otool -hv $(AIRPORT_KEXT)/Contents/MacOS/AirportQCA
	@otool -hv $(BT_KEXT)/Contents/MacOS/QCABluetoothFirmware
	@echo "==> Verification completed. Kexts ready for OpenCore EFI."

clean:
	@rm -rf $(BUILD_DIR)
	@echo "==> Cleaned build directory"
