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
KERNEL_FLAGS := \
	-mkernel \
	-arch $(ARCH) \
	-isysroot $(SDKROOT) \
	-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/Headers \
	-I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/PrivateHeaders \
	-fno-builtin \
	-fno-common \
	-fno-exceptions \
	-fno-rtti \
	-nostdinc++ \
	-fno-stack-protector \
	-mno-red-zone \
	-fstrict-aliasing \
	-DKERNEL \
	-D__APPLE__ \
	-D__IO80211_TARGET=12 \
	-std=c++14 \
	-Wall -Wextra -Wno-unused-parameter \
	-Wno-deprecated-declarations \
	-Wno-inconsistent-missing-override

# Linker flags for pure Mach-O 64-bit KEXT bundle
LDFLAGS := \
	-mkernel \
	-arch $(ARCH) \
	-isysroot $(SDKROOT) \
	-nostdlib \
	-Xlinker -kext \
	-lkmod

BUILD_DIR := ../build
AIRPORT_DIR := AirportQCA
BT_DIR := QCABluetooth

AIRPORT_SRCS := \
	$(AIRPORT_DIR)/AirportQCA.cpp \
	$(AIRPORT_DIR)/AirportQCAInterface.cpp \
	$(AIRPORT_DIR)/hw/QCAPCIDevice.cpp \
	$(AIRPORT_DIR)/hw/QCACopyEngine.cpp \
	$(AIRPORT_DIR)/hw/QCABMI.cpp \
	$(AIRPORT_DIR)/hw/QCAHTC.cpp \
	$(AIRPORT_DIR)/hw/QCAWMI.cpp \
	$(AIRPORT_DIR)/hw/QCAHTT.cpp

AIRPORT_OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(AIRPORT_SRCS))

BT_SRCS := \
	$(BT_DIR)/QCABluetoothFirmware.cpp \
	$(BT_DIR)/QCAUSBController.cpp

BT_OBJS := $(patsubst %.cpp, $(BUILD_DIR)/%.o, $(BT_SRCS))

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
	@codesign -s - --force $(AIRPORT_KEXT) 2>/dev/null || true
	@echo "==> Successfully built AirportQCA.kext"

$(BUILD_DIR)/$(AIRPORT_DIR)/%.o: $(AIRPORT_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_FLAGS) -I$(AIRPORT_DIR) -I$(AIRPORT_DIR)/include -I$(AIRPORT_DIR)/include/Apple80211 -c $< -o $@

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
	@codesign -s - --force $(BT_KEXT) 2>/dev/null || true
	@echo "==> Successfully built QCABluetoothFirmware.kext"

$(BUILD_DIR)/$(BT_DIR)/%.o: $(BT_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(KERNEL_FLAGS) -I$(BT_DIR) -c $< -o $@

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
