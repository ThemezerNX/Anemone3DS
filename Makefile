#---------------------------------------------------------------------------------
# Themezer-3DS
#
#   make            the 3dsx and the cia, in out/
#   make 3dsx       only the 3dsx
#   make cia        only the cia
#   make test       build and run the unit tests on this computer
#   make send       send the 3dsx to a console waiting in the Homebrew Launcher
#   make preview    open the 3dsx in the Azahar emulator
#   make update-certs  replace romfs/cacert.pem with curl's latest copy of Mozilla's CA list
#   make clean
#
# VERSION=x.y.z overrides the version, which otherwise comes from the git tag on
# HEAD (vX.Y.Z), or BASE_VERSION plus the commit for untagged builds.
# DEVTOOLS=1 builds in the autopilot (docs/DEVELOPMENT.md), apart from the normal
# build: in build/dev and out/dev.
#---------------------------------------------------------------------------------
.SUFFIXES:
.DELETE_ON_ERROR:

# the unit tests are built for this computer and don't need devkitARM
ifneq ($(filter-out test update-certs,$(or $(MAKECMDGOALS),all)),)
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
include $(DEVKITARM)/3ds_rules
endif

#---------------------------------------------------------------------------------
# The app
#---------------------------------------------------------------------------------
APP_TITLE        := Themezer
APP_DESCRIPTION  := Themes, splashes and badges for your 3DS
APP_AUTHOR       := Themezer
APP_UNIQUE_ID    := 0xC7E20
APP_PRODUCT_CODE := CTR-P-TZ3D
# where the update check reads the latest release, in the format of GitHub's releases API
UPDATE_URL       ?= https://3ds.themezer.net/app/releases/latest

BASE_VERSION     := 4.0.0

TARGET   := Themezer
BUILD    := build
OUT      := out
ifeq ($(DEVTOOLS),1)
  BUILD  := build/dev
  OUT    := out/dev
endif
SOURCES  := source third_party/quirc
INCLUDES := source third_party
ROMFS    := romfs
META     := meta
GFX      := gfx

#---------------------------------------------------------------------------------
# Version
#---------------------------------------------------------------------------------
ifeq ($(strip $(VERSION)),)
  GIT_TAG  := $(shell git describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null)
  GIT_HASH := $(shell git rev-parse --short=7 HEAD 2>/dev/null)
  ifneq ($(strip $(GIT_TAG)),)
    VERSION := $(GIT_TAG:v%=%)
  else ifneq ($(strip $(GIT_HASH)),)
    VERSION := $(BASE_VERSION)-dev+$(GIT_HASH)
  else
    VERSION := $(BASE_VERSION)-dev
  endif
endif

VERSION_CORE  := $(firstword $(subst -, ,$(subst +, ,$(VERSION))))
VERSION_MAJOR := $(or $(word 1,$(subst ., ,$(VERSION_CORE))),0)
VERSION_MINOR := $(or $(word 2,$(subst ., ,$(VERSION_CORE))),0)
VERSION_MICRO := $(or $(word 3,$(subst ., ,$(VERSION_CORE))),0)

#---------------------------------------------------------------------------------
# Flags
#---------------------------------------------------------------------------------
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

PKGS     := libcurl jansson libpng zlib
PKGCONFIG:= $(wildcard $(PORTLIBS)/bin/arm-none-eabi-pkg-config)
INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
            -I$(CURDIR)/$(BUILD)/gen \
            -I$(CTRULIB)/include -I$(PORTLIBS)/include \
            $(if $(PKGCONFIG),$(shell $(PKGCONFIG) --cflags-only-I $(PKGS)))

DEFINES  := -D__3DS__ \
            -DAPP_TITLE='"$(APP_TITLE)"' \
            -DAPP_VERSION='"$(VERSION)"' \
            -DUPDATE_URL='"$(UPDATE_URL)"' \
            $(if $(filter 1,$(DEVTOOLS)),-DTZ_DEVTOOLS) \
            $(if $(PKGCONFIG),$(shell $(PKGCONFIG) --cflags-only-other $(PKGS)))

WARNINGS := -Wall -Wextra -Wno-missing-field-initializers -Wno-psabi

CFLAGS   := -g -O2 -mword-relocations -ffunction-sections -fdata-sections \
            $(ARCH) $(DEFINES) $(INCLUDE) $(WARNINGS)
CXXFLAGS := $(CFLAGS) -std=gnu++23 -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map -Wl,--gc-sections

LIBS     := $(if $(PKGCONFIG),$(shell $(PKGCONFIG) --libs $(PKGS))) -lcitro2d -lcitro3d -lctru -lm
LIBPATHS := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib

#---------------------------------------------------------------------------------
# Files
#---------------------------------------------------------------------------------
CFILES   := $(foreach dir,$(SOURCES),$(shell find $(dir) -name '*.c' 2>/dev/null))
CPPFILES := $(foreach dir,$(SOURCES),$(shell find $(dir) -name '*.cpp' 2>/dev/null))
OFILES   := $(addprefix $(BUILD)/,$(CPPFILES:.cpp=.o) $(CFILES:.c=.o))
DEPFILES := $(OFILES:.o=.d)

GFXFILES := $(wildcard $(GFX)/*.t3s)
T3XFILES := $(patsubst $(GFX)/%.t3s,$(ROMFS)/gfx/%.t3x,$(GFXFILES))
T3XHFILES:= $(patsubst $(GFX)/%.t3s,$(BUILD)/gen/%.h,$(GFXFILES))

# files copied into the RomFS when building
ROMFS_GEN := $(ROMFS)/licenses.txt

ELF      := $(OUT)/$(TARGET).elf
SMDH     := $(OUT)/$(TARGET).smdh
3DSX     := $(OUT)/$(TARGET).3dsx
CIA      := $(OUT)/$(TARGET).cia

MAKEROM    ?= makerom
BANNERTOOL ?= bannertool

.PHONY: all 3dsx cia test send preview clean update-certs

all: 3dsx cia

3dsx: $(3DSX)

cia: $(CIA)

#---------------------------------------------------------------------------------
# Compiling
#---------------------------------------------------------------------------------
$(OFILES): | $(T3XHFILES)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo $<
	@$(CXX) -MMD -MP -MF $(BUILD)/$*.d $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo $<
	@$(CC) -MMD -MP -MF $(BUILD)/$*.d $(CFLAGS) -std=gnu17 -c $< -o $@

# code from elsewhere is kept as it is, warnings and all
$(BUILD)/third_party/%.o: CFLAGS += -w

$(ROMFS)/gfx/%.t3x $(BUILD)/gen/%.h: $(GFX)/%.t3s
	@mkdir -p $(ROMFS)/gfx $(BUILD)/gen
	@echo $<
	@tex3ds -i $< -H $(BUILD)/gen/$*.h -d $(BUILD)/gen/$*.d -o $(ROMFS)/gfx/$*.t3x

$(ROMFS)/licenses.txt: THIRD_PARTY_NOTICES.md LICENSE
	@echo $@
	@cat THIRD_PARTY_NOTICES.md > $@

#---------------------------------------------------------------------------------
# Linking and packaging
#---------------------------------------------------------------------------------
$(ELF): $(OFILES)
	@mkdir -p $(OUT)
	@echo linking $(notdir $@)
	@$(CXX) $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

$(SMDH): $(META)/icon.png Makefile
	@mkdir -p $(OUT)
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $< $@

$(3DSX): $(ELF) $(SMDH) $(T3XFILES) $(ROMFS_GEN) $(shell find $(ROMFS) -type f 2>/dev/null)
	@3dsxtool $(ELF) $@ --smdh=$(SMDH) --romfs=$(ROMFS)
	@echo built ... $(notdir $@) $(VERSION)

$(BUILD)/banner.bnr: $(META)/banner.cgfx $(META)/audio.wav
	@mkdir -p $(BUILD)
	@$(BANNERTOOL) makebanner -ci $(META)/banner.cgfx -a $(META)/audio.wav -o $@ > /dev/null

$(BUILD)/icon.icn: $(META)/icon.png Makefile
	@mkdir -p $(BUILD)
	@$(BANNERTOOL) makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" \
		-i $< -f "nosavebackups,visible" -o $@ > /dev/null

$(CIA): $(ELF) $(BUILD)/banner.bnr $(BUILD)/icon.icn $(META)/app.rsf $(META)/logo.bin $(T3XFILES) $(ROMFS_GEN) $(shell find $(ROMFS) -type f 2>/dev/null)
	@$(MAKEROM) -f cia -o $@ -target t -exefslogo -elf $(ELF) -rsf $(META)/app.rsf \
		-banner $(BUILD)/banner.bnr -icon $(BUILD)/icon.icn -logo $(META)/logo.bin \
		-DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" \
		-DAPP_UNIQUE_ID="$(APP_UNIQUE_ID)" -DAPP_ROMFS="$(ROMFS)" \
		-major $(VERSION_MAJOR) -minor $(VERSION_MINOR) -micro $(VERSION_MICRO)
	@echo built ... $(notdir $@) $(VERSION)

#---------------------------------------------------------------------------------
# Unit tests, built for this computer
#---------------------------------------------------------------------------------
HOST_CXX      ?= c++
# libpng and zlib for the computer: from pkg-config, or Homebrew's prefix
HOST_PKG      := $(shell pkg-config --cflags --libs libpng zlib 2>/dev/null)
ifeq ($(strip $(HOST_PKG)),)
  HOST_PREFIX := $(firstword $(wildcard /opt/homebrew /usr/local))
  HOST_PKG    := -I$(HOST_PREFIX)/include -L$(HOST_PREFIX)/lib -lpng -lz
endif
HOST_CXXFLAGS := -std=c++23 -g -O1 -Wall -Wextra -Wno-missing-field-initializers \
                 -fsanitize=address,undefined -Isource -Itests
HOST_SOURCES  := $(shell find tests source/core source/domain -name "*.cpp" 2>/dev/null)

test:
	@mkdir -p $(BUILD)/host
	@$(HOST_CXX) $(HOST_CXXFLAGS) $(HOST_SOURCES) $(HOST_PKG) -o $(BUILD)/host/tests
	@$(BUILD)/host/tests

#---------------------------------------------------------------------------------
# The CA certificates HTTPS is checked against. The app stops checking once its copy is three years old (the app has
# not been updated for that long), so a build should always carry a fresh one; the release workflow fetches it too.
update-certs:
	@curl -fsSL --retry 3 -o $(BUILD)/cacert.pem https://curl.se/ca/cacert.pem --create-dirs
	@curl -fsSL --retry 3 -o $(BUILD)/cacert.pem.sha256 https://curl.se/ca/cacert.pem.sha256
	@cd $(BUILD) && shasum -a 256 -c cacert.pem.sha256
	@mv $(BUILD)/cacert.pem $(ROMFS)/cacert.pem
	@grep -m1 'as of:' $(ROMFS)/cacert.pem

#---------------------------------------------------------------------------------
send: $(3DSX)
	@3dslink $(if $(THREEDS_IP),-a $(THREEDS_IP)) $(3DSX)

# Azahar from https://azahar-emu.org: on a Mac the app, opened by name; elsewhere the azahar command.
# AZAHAR=<path> when it's somewhere else.
ifeq ($(shell uname),Darwin)
AZAHAR ?= Azahar
preview: $(3DSX)
	@open -n -a "$(AZAHAR)" --args "$(CURDIR)/$(3DSX)"
else
AZAHAR ?= azahar
preview: $(3DSX)
	@"$(AZAHAR)" "$(CURDIR)/$(3DSX)" > /dev/null 2>&1 &
endif

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(OUT) $(ROMFS)/gfx $(ROMFS_GEN)

-include $(DEPFILES)
