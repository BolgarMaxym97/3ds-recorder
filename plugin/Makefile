# rec3ds-launch.3gx: a tiny Luma plugin that starts the recorder module again inside a game that
# rebooted an Old 3DS into its extended memory mode (see source/main.c). Embedded into the app.
# Build in the dev image: tools/dkp.sh plugin make
.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment")
endif
include $(DEVKITARM)/3ds_rules

TARGET  := rec3ds-launch
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -Wall -Wextra -Os -mword-relocations -ffunction-sections -fdata-sections $(ARCH) -std=gnu11 \
           -D__3DS__ -I$(CTRULIB)/include
LDFLAGS := -T 3gx.ld $(ARCH) -nostartfiles -nostdlib \
           -Wl,--gc-sections,--strip-discarded,--strip-debug,--section-start,.text=0x07000100

.PHONY: all clean
all: $(TARGET).3gx

build/%.o: source/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: source/%.s
	@mkdir -p build
	$(CC) $(ARCH) -c $< -o $@

build/$(TARGET).elf: build/start.o build/main.o 3gx.ld
	$(CC) $(LDFLAGS) build/start.o build/main.o -o $@

$(TARGET).3gx: build/$(TARGET).elf $(TARGET).plgInfo
	3gxtool -s $< $(TARGET).plgInfo $@
	@echo built ... $@

clean:
	@rm -fr build $(TARGET).3gx
