PS5_HOST ?= ps5
PS5_PORT ?= 9021

# The host compiler builds the tests; prospero.mk replaces CC with the SDK's.
HOSTCC ?= cc

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

BUILD := build

# Release version; releases are tagged v$(VERSION). Built into the ELF.
VERSION := $(shell cat VERSION)

CFLAGS := -Wall -Werror -g -Icommon -DAPP_VERSION=\"$(VERSION)\"

# Shared payload framework: web server, logs, service lifecycle, page assets.
COMMON_SRCS := common/http.c common/log.c common/util.c common/service.c
COMMON_HDRS := $(wildcard common/*.h)
COMMON_UI   := common/ui/common.css common/ui/common.js
# Console side of common/sys.h; tests and demos use common/sys_host.c.
COMMON_PS5  := common/sys_ps5.c
COMMON_LIBS := -lSceNotification -lSceAppInstUtil -lpthread

NETDIS_SRCS := netdis/main.c netdis/routes.c netdis/sockets.c
NETDIS_HDRS := $(wildcard netdis/*.h) $(COMMON_HDRS)
TEST_SRCS   := tests/test_netdis.c netdis/routes.c netdis/sockets.c \
               $(COMMON_SRCS) common/sys_host.c

all: NetDis.elf

sdk:
	@test -n "$(PS5_PAYLOAD_SDK)" || { echo "PS5_PAYLOAD_SDK is undefined"; exit 1; }

# netdis/assets/icon0.png is rendered from icon.svg; see netdis/assets/README.md.
$(BUILD)/netdis_embedded.c: tools/embed.py netdis/ui.html netdis/assets/icon0.png \
                            $(COMMON_UI)
	@mkdir -p $(BUILD)
	python3 tools/embed.py $@ netdis_ui_html=netdis/ui.html \
	  netdis_icon_png:bin=netdis/assets/icon0.png \
	  common_ui_css=common/ui/common.css common_ui_js=common/ui/common.js

NetDis.elf: VERSION $(NETDIS_SRCS) netdis/platform_ps5.c $(COMMON_SRCS) $(COMMON_PS5) \
            $(BUILD)/netdis_embedded.c $(NETDIS_HDRS) | sdk
	$(CC) $(CFLAGS) -o $@ $(NETDIS_SRCS) netdis/platform_ps5.c $(COMMON_SRCS) \
	  $(COMMON_PS5) $(BUILD)/netdis_embedded.c $(COMMON_LIBS)

# Host tests: no SDK or console needed.
$(BUILD)/test_netdis: $(TEST_SRCS) $(NETDIS_HDRS)
	@mkdir -p $(BUILD)
	$(HOSTCC) $(CFLAGS) -Inetdis -fsanitize=address,undefined -o $@ \
	  $(TEST_SRCS) -lpthread

check: $(BUILD)/test_netdis
	$(BUILD)/test_netdis

# Release package: the ELF with the README and changelog.
DIST := $(BUILD)/NetDis-v$(VERSION).zip
dist: NetDis.elf
	@mkdir -p $(BUILD)
	rm -f $(DIST)
	zip -j $(DIST) NetDis.elf README.md CHANGELOG.md

clean:
	rm -f *.elf $(BUILD)/*_embedded.c $(BUILD)/test_*

# Sends NetDis to the loader on the PS5 and streams its log back.
test: NetDis.elf | sdk
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) NetDis.elf

.PHONY: all sdk check clean test dist
