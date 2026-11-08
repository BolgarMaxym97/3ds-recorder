# bench.3gx — phase-0 in-game benchmark plugin (CTRPluginFramework).
# Builds two variants that differ only in plugin memory: bench-5M.3gx, bench-10M.3gx.
# Build in the dev image: tools/dkp.sh tests/benchplugin make
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

export TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

CTRPFLIB	?=	$(DEVKITPRO)/libctrpf

# Includes/ holds the types.h shim that CTRPF headers expect.
INCLUDES	:=	Includes Sources ../../libbench/include ../../codec/include
SOURCES		:=	Sources ../../libbench/source ../../codec/source

ARCH		:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS		:=	-Wall -Wextra -O2 -mword-relocations \
				-ffunction-sections -fdata-sections -fno-strict-aliasing \
				$(ARCH) $(INCLUDE) -D__3DS__ -DNDEBUG=1
CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS		:=	$(ARCH)
LDFLAGS		:=	-T $(TOPDIR)/3gx.ld $(ARCH) -Os \
				-Wl,--strip-discarded,--strip-debug,--gc-sections,--section-start,.text=0x07000100

LIBS		:=	-lctrpf -lctru -lm
LIBDIRS		:=	$(CTRPFLIB) $(CTRULIB) $(PORTLIBS)

BUILD		?=	build

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD		:=	$(CXX)
export OFILES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile bench.elf
	@3gxtool -s $(BUILD)/bench.elf bench-5M.plgInfo bench-5M.3gx
	@3gxtool -s $(BUILD)/bench.elf bench-10M.plgInfo bench-10M.3gx
	@echo built ... bench-5M.3gx bench-10M.3gx

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -rf $(BUILD) *.3gx

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
DEPENDS	:=	$(OFILES:.o=.d)

bench.elf: $(OFILES)

-include $(DEPENDS)
#---------------------------------------------------------------------------------
endif
