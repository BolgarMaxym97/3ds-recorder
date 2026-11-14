# 3DS Recorder control app: .3dsx and .cia from the same ELF and RomFS.
# Embeds sysmod/rec3ds.cxi as romfs:/rec3ds.cxi and plugin/rec3ds-launch.3gx; build both first.
#   tools/dkp.sh app make        -> 3ds-recorder.3dsx
#   tools/dkp.sh app make cia    -> 3ds-recorder.cia (needs assets/banner.bnr: tools/make-art.sh on the Mac)
# Fonts (romfs/font/*.bcfnt) and art (assets/icon.png, banner.*) are made by tools/make-art.sh.
.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET          := 3ds-recorder
# The CIA's title version: raise it with each build that goes to a console, or the HOME Menu
# keeps showing the banner it cached for the installed version.
APP_VERSION := 1.1.0
# "beta" for the open beta, empty for a release: shown next to the version on the panel
APP_CHANNEL     :=
APP_TITLE       := 3DS Recorder
APP_DESCRIPTION := Records the whole console
APP_AUTHOR      := 3ds-screen-recorder
BUILD           := build
AAC             := ../third_party/vo-aacenc
SOURCES         := source ../codec/source ../prep/source ../third_party/qrcodegen \
                   $(AAC)/aacenc/src $(AAC)/aacenc/basic_op $(AAC)/common
INCLUDES        := source ../codec/include ../prep/include ../tools/r3dconv ../third_party/qrcodegen \
                   ../third_party/minih264 $(AAC)/aacenc/inc $(AAC)/aacenc/basic_op $(AAC)/common/include

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections $(ARCH) -std=gnu11
CFLAGS   += $(INCLUDE) -D__3DS__ -DAPP_VERSION=\"$(APP_VERSION)\" -DAPP_CHANNEL=\"$(APP_CHANNEL)\"
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS     := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS  := $(CTRULIB) $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
export LD := $(CC)
export OFILES := $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_ICON := $(CURDIR)/assets/icon.png
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh --romfs=$(CURDIR)/romfs

.PHONY: all clean cia
all: $(BUILD)
	@cp -p ../sysmod/rec3ds.cxi romfs/rec3ds.cxi
	@cp -p ../plugin/rec3ds-launch.3gx romfs/rec3ds-launch.3gx
	@python3 ../tools/build-web.py ../web romfs/web/index.html.gz
	@# the 3dsx embeds romfs but its rule only follows the ELF: rebuild it when romfs changed
	@if [ -f $(TARGET).3dsx ] && [ -n "$$(find romfs -newer $(TARGET).3dsx -type f)" ]; then rm -f $(TARGET).3dsx; fi
	@# the objects that print the version: rebuilt when APP_VERSION / APP_CHANNEL change (make does not
	@# follow CFLAGS; 0.9.12-0.9.14 showed "0.9.11" on the panel, 2026-11-08)
	@mkdir -p $(BUILD); if [ "$$(cat $(BUILD)/.app_version 2>/dev/null)" != "$(APP_VERSION) $(APP_CHANNEL)" ]; then \
		rm -f $(BUILD)/app.o $(BUILD)/recorder.o; echo "$(APP_VERSION) $(APP_CHANNEL)" > $(BUILD)/.app_version; fi
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

# The CIA wraps the same ELF and RomFS (cia.rsf: title, services, system calls). Its splash is
# assets/logo.bcma.lz (signed, from tools/make-art.sh); without it, makerom's stock homebrew one.
# cia-stock: the same with the stock splash, which tools/make-art.sh reworks.
CIA_LOGO := $(if $(wildcard assets/logo.bcma.lz),-logo assets/logo.bcma.lz)
VERSION_PARTS := $(subst ., ,$(APP_VERSION))
cia: all
	@test -f assets/banner.bnr || { echo "assets/banner.bnr missing: run tools/make-art.sh on the Mac"; exit 1; }
	makerom -f cia -o $(TARGET).cia -target t -exefslogo $(CIA_LOGO) -rsf cia.rsf -elf $(TARGET).elf \
		-icon $(TARGET).smdh -banner assets/banner.bnr \
		-major $(word 1,$(VERSION_PARTS)) -minor $(word 2,$(VERSION_PARTS)) -micro $(word 3,$(VERSION_PARTS))
	python3 ../tools/cia_version.py $(TARGET).cia $(APP_VERSION)
	@echo "built ... $(TARGET).cia $(if $(CIA_LOGO),(own splash),(stock splash))"

cia-stock: all
	makerom -f cia -o ../build/logo/stock.cia -target t -exefslogo -rsf cia.rsf -elf $(TARGET).elf \
		-icon $(TARGET).smdh -banner assets/banner.bnr

clean:
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).cia romfs/rec3ds.cxi romfs/rec3ds-launch.3gx romfs/web
else
DEPENDS := $(OFILES:.o=.d)
qrcodegen.o: CFLAGS += -Wno-type-limits   # third-party (third_party/qrcodegen)
prep.o: CFLAGS += -O3 -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function  # minih264e inside
# third-party AAC encoder (third_party/vo-aacenc): its own style, not ours to warn about
AAC_OBJS := $(patsubst %.c,%.o,$(notdir $(wildcard $(TOPDIR)/$(AAC)/aacenc/src/*.c $(TOPDIR)/$(AAC)/aacenc/basic_op/*.c))) cmnMemory.o
$(AAC_OBJS): CFLAGS += -O3 -w
$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
