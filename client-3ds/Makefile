# Based on the current devkitPro 3DS application template.
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set. Install the official devkitPro 3DS toolchain and restart your shell")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET      := 3gent
BUILD       := build
SOURCES     := source
INCLUDES    := include

APP_TITLE       := 3gent
APP_DESCRIPTION := citro2d interface hardware-test build
APP_AUTHOR      := 3gent contributors

SERVER_HOST ?= 192.168.1.2
SERVER_PORT ?= 8080
PUSH_PORT ?= 8081

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS := -g -Wall -Wextra -Werror -O2 -mword-relocations \
          -ffunction-sections $(ARCH)
CFLAGS += $(INCLUDE) -D__3DS__ -std=gnu11 \
          -DTHREEGENT_SERVER_HOST=\"$(SERVER_HOST)\" \
          -DTHREEGENT_SERVER_PORT=$(SERVER_PORT) \
          -DTHREEGENT_PUSH_PORT=$(PUSH_PORT)

ASFLAGS := -g $(ARCH)
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS    := -lcitro2d -lcitro3d -lctru -lm
LIBDIRS := $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CC)
export OFILES := $(SFILES:.s=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXDEPS := $(OUTPUT).smdh
export _3DSXFLAGS += --smdh=$(OUTPUT).smdh

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).map

else

$(OUTPUT).3dsx: $(OUTPUT).elf $(_3DSXDEPS)
$(OFILES):
$(OUTPUT).elf: $(OFILES)

-include $(DEPSDIR)/*.d

endif
