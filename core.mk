# core.mk: make targets shared by every app on the common ESP32 base.
# Byte-identical in every repo. The app's Makefile sets these variables and then includes it:
#
#   APP         sketch name, firmware/$(APP)/$(APP).ino                  (required)
#   TITLE       name shown by `make help`                                (default: $(APP))
#   BOARDS      boards the app supports, a subset of: c6 s3 round amoled (required)
#   BOARD       board when none is given                                 (default: first of BOARDS)
#   GENERATORS  app generators under tools/; each accepts --check        (optional)
#               tools/gen_sounds.mjs runs too once shared/sounds.json exists
#   WEB_EMBED   extra NAME=path files that gen_web.mjs embeds            (optional)
#   SITE_EXTRA  shell command run by `make site` once _site/ is filled   (optional)
#   CORE_PEER   default OTHER for core-diff/core-sync: a sibling repo    (optional)
#
# Plain `make` lists the targets: any target followed by `## text` shows up, app ones included.
# CI runs exactly these targets, so a contributor can reproduce it locally.

SKETCH := firmware/$(APP)
BOARD  ?= $(firstword $(BOARDS))
BUILD  := build/$(BOARD)

# Pinned versions: a core or library update must never break the build without anyone
# touching the code. CI installs exactly these, and its cache key hashes this file.
CORE_VERSION := 3.2.0
# SensorLib and XPowersLib drive the touch and the PMIC of the AMOLED board (the versions of
# Waveshare's examples); the other boards do not include them.
LIBS ?= "GFX Library for Arduino@1.6.4" "ArduinoJson@7.4.2" "SensorLib@0.3.3" "XPowersLib@0.2.6"
# Board manager index of the ESP32 core: a clean arduino-cli install does not know it.
ESP32_URL := https://espressif.github.io/arduino-esp32/package_esp32_index.json

# Boards. The sketch picks its pinout in board.h from the compiler target, plus a define for
# boards that share a chip, so a board is an FQBN and maybe a define here: make build BOARD=s3.
# The first revision of the S3-Touch-LCD-1.69 (BOARD_S3_TOUCH169_V1) is not offered as
# BOARD=s3v1: its pins 33/35/36 overlap the octal PSRAM (33-37) and are still to be reviewed.
FQBN_c6    := esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=no_ota,FlashSize=4M
FQBN_s3    := esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB,FlashSize=16M,PSRAM=opi
# Round ESP32-S3-Touch-LCD-1.28: same chip as s3, quad PSRAM, serial through a CH343 (UART0).
FQBN_round := esp32:esp32:esp32s3:CDCOnBoot=default,PartitionScheme=app3M_fat9M_16MB,FlashSize=16M,PSRAM=enabled
DEFINES_round := -DBOARD_S3_LCD128
# AMOLED ESP32-S3-Touch-AMOLED-1.75C: octal PSRAM, 32 MB flash (4.5 MB app x2 + 22 MB FAT), native USB.
FQBN_amoled := esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=app5M_fat24M_32MB,FlashSize=32M,PSRAM=opi
DEFINES_amoled := -DBOARD_S3_AMOLED175
CHIP_c6  := esp32c6
CHIP_s3  := esp32s3
CHIP_round := esp32s3
CHIP_amoled := esp32s3
FLASH_c6 := 4MB
FLASH_s3 := 16MB
FLASH_round := 16MB
FLASH_amoled := 32MB
DESC_c6  := Waveshare ESP32-C6-LCD-1.47
DESC_s3  := Waveshare ESP32-S3-Touch-LCD-1.69
DESC_round := Waveshare ESP32-S3-Touch-LCD-1.28 (round)
DESC_amoled := Waveshare ESP32-S3-Touch-AMOLED-1.75C (round AMOLED)

ifeq ($(filter $(BOARD),$(BOARDS)),)
$(error Unknown BOARD '$(BOARD)'. Use one of: $(BOARDS))
endif
FQBN := $(FQBN_$(BOARD))
BUILD_FLAGS := $(if $(DEFINES_$(BOARD)),--build-property "compiler.cpp.extra_flags=$(DEFINES_$(BOARD))" --build-property "compiler.c.extra_flags=$(DEFINES_$(BOARD))")

# Native USB boards enumerate as usbmodem / ttyACM. With two boards plugged in, pass PORT=...
PORT ?= $(shell ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -n1)
BAUD ?= 115200
# Python with esptool installed, for flash-small.
PY   ?= python3

# Where arduino-cli keeps the cores (boot_app0.bin lives there).
DATA_DIR = $(shell arduino-cli config get directories.data 2>/dev/null || echo $$HOME/.arduino15)

# Landing URLs. CI passes the real ones; locally they come from the git remote, if any.
REPO_URL ?= $(shell git remote get-url origin 2>/dev/null | sed -E 's|^git@github.com:|https://github.com/|; s|\.git$$||')
REPO_URL := $(or $(REPO_URL),https://github.com/OWNER/$(APP))
OWNER     := $(word 3,$(subst /, ,$(REPO_URL)))
REPO_NAME := $(word 4,$(subst /, ,$(REPO_URL)))
SITE_URL  ?= http://localhost:8000/

# Another repo on the same base, for core-diff and core-sync (PEER= and FROM= also work).
OTHER ?= $(or $(PEER),$(FROM),$(CORE_PEER))
NEED_OTHER = @test -n "$(OTHER)" || { echo "Usage: make $@ OTHER=../other-repo"; exit 1; }

NEED_PORT = @test -n "$(PORT)" || { echo "No serial port found. Plug the board in, or pass PORT=/dev/cu.usbmodemXXXX"; exit 1; }

.DEFAULT_GOAL := help
.PHONY: help deps gen check build build-all flash flash-small monitor shot bootapp0 dist dist-all \
	og site serve bump print-build print-boards clean core-check core-diff core-sync core-lock

help: ## Show this help
	@echo
	@echo "$(or $(TITLE),$(APP))"
	@echo
	@grep -hE '^[a-zA-Z0-9_-]+:.*## ' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*## "} {printf "  \033[36m%-13s\033[0m %s\n", $$1, $$2}'
	@echo
	@echo "  Board: $(BOARD). Supported:"
	@$(foreach b,$(BOARDS),echo "    BOARD=$(b)  $(DESC_$(b))";)
	@echo "  Detected port: $(if $(PORT),$(PORT),none)"
	@echo

deps: ## Install the pinned ESP32 core and libraries (CI uses this too)
	arduino-cli core update-index --additional-urls $(ESP32_URL)
	arduino-cli core install esp32:esp32@$(CORE_VERSION) --additional-urls $(ESP32_URL)
	arduino-cli lib install $(LIBS)

# Optional pieces join gen/check once they exist in the repo.
SOUNDS_GEN  := $(if $(wildcard shared/sounds.json),tools/gen_sounds.mjs)
CONSISTENCY := $(wildcard tools/check_consistency.mjs)
MCP_TESTS   := $(wildcard mcp/test/*.test.mjs)

gen: ## Regenerate the embedded headers from shared/ and web/
	@for g in $(GENERATORS) $(SOUNDS_GEN); do node $$g || exit 1; done
	@node tools/gen_web.mjs $(WEB_EMBED)

check: ## Core files, manifests and version, generated files up to date, consistency, MCP tests
	@node tools/check_core.mjs check
	@python3 tools/check-manifest.py $(BOARDS)
	@for g in $(GENERATORS) $(SOUNDS_GEN); do node $$g --check || exit 1; done
	@node tools/gen_web.mjs --check $(WEB_EMBED)
	$(if $(CONSISTENCY),@node $(CONSISTENCY),@echo "consistency: no tools/check_consistency.mjs yet")
	$(if $(MCP_TESTS),@node --test $(MCP_TESTS),@echo "mcp: no tests yet")

build: gen ## Regenerate and compile BOARD into build/BOARD
	arduino-cli compile --fqbn "$(FQBN)" $(BUILD_FLAGS) --build-path $(BUILD) --warnings default $(SKETCH)

# In series: parallel builds of one sketch collide in the arduino-cli cache.
build-all: ## Compile every supported board, one after another
	@for b in $(BOARDS); do $(MAKE) --no-print-directory build BOARD=$$b || exit 1; done

flash: build ## Compile and upload BOARD (PORT=... with more than one board)
	$(NEED_PORT)
	@echo "Uploading to $(PORT)"
	@arduino-cli upload --fqbn "$(FQBN)" --input-dir $(BUILD) -p $(PORT) $(SKETCH) || { \
		echo; \
		echo "If it failed to connect: hold BOOT, tap RST, release BOOT and retry."; \
		echo "If the port is busy, close any open serial monitor (lsof $(PORT))."; \
		exit 1; }

# For USB-UART bridges whose large writes fail ("Failed to write to target RAM"), such as the
# CH343 of the round board with the macOS driver: esptool in 256-byte blocks.
flash-small: build bootapp0 ## Upload in small blocks, for the round board's CH343 (PY=python with esptool)
	$(NEED_PORT)
	$(PY) tools/esptool_small.py --chip $(CHIP_$(BOARD)) --port $(PORT) -b 460800 write_flash \
	  --flash_size $(FLASH_$(BOARD)) \
	  0x0 $(BUILD)/$(APP).ino.bootloader.bin 0x8000 $(BUILD)/$(APP).ino.partitions.bin \
	  0xe000 $(BUILD)/boot_app0.bin 0x10000 $(BUILD)/$(APP).ino.bin

monitor: ## Open the serial console (type 'help' inside; Ctrl-C to leave)
	$(NEED_PORT)
	arduino-cli monitor -p $(PORT) --config baudrate=$(BAUD)

shot: ## Save the real screen as shot-BOARD.png over serial (OUT=path to change)
	$(NEED_PORT)
	@mkdir -p $(dir $(or $(OUT),shot-$(BOARD).png))
	@python3 tools/screenshot.py $(PORT) $(or $(OUT),shot-$(BOARD).png)

# boot_app0.bin is not produced by the compiler: it ships with the core.
bootapp0: ## Copy boot_app0.bin from the installed core next to the binaries of BOARD
	@src="$(DATA_DIR)/packages/esp32/hardware/esp32/$(CORE_VERSION)/tools/partitions/boot_app0.bin"; \
	 test -f "$$src" || { echo "boot_app0.bin not found in the core. Run: make deps"; exit 1; }; \
	 mkdir -p $(BUILD) && cp "$$src" $(BUILD)/boot_app0.bin

# Names the manifests and the release use: <part>-<board>.bin, <app>-<board>.bin and
# <app>-<board>.merged.bin (the whole flash image, for esptool at 0x0).
dist: bootapp0 ## Copy the binaries of BOARD to dist/ with their published names (after make build)
	@mkdir -p dist
	cp $(BUILD)/$(APP).ino.bootloader.bin dist/bootloader-$(BOARD).bin
	cp $(BUILD)/$(APP).ino.partitions.bin dist/partitions-$(BOARD).bin
	cp $(BUILD)/boot_app0.bin dist/boot_app0-$(BOARD).bin
	cp $(BUILD)/$(APP).ino.bin dist/$(APP)-$(BOARD).bin
	cp $(BUILD)/$(APP).ino.merged.bin dist/$(APP)-$(BOARD).merged.bin

dist-all: build-all ## Build every board and fill dist/
	@for b in $(BOARDS); do $(MAKE) --no-print-directory dist BOARD=$$b || exit 1; done

og: ## Render the landing's social card to _site/og.png (no dependencies)
	@mkdir -p _site
	node tools/gen_og.mjs _site/og.png

# The same _site/ the pages job deploys. Binaries never live in the repo.
site: ## Build every board and assemble the web installer in _site/, as CI does (NOBUILD=1 reuses dist/)
	@test "$(NOBUILD)" = 1 || $(MAKE) --no-print-directory dist-all
	rm -rf _site && mkdir -p _site/shared
	sed -e 's|__SITE_URL__|$(SITE_URL)|g' -e 's|__REPO_URL__|$(REPO_URL)|g' \
	    -e 's|__REPO_NAME__|$(REPO_NAME)|g' -e 's|__OWNER__|$(OWNER)|g' docs/index.html > _site/index.html
	cp -R $(filter-out docs/index.html docs/%.md,$(wildcard docs/*)) _site/
	cp shared/*.json _site/shared/
	@for b in $(BOARDS); do \
	  cp dist/bootloader-$$b.bin dist/partitions-$$b.bin dist/boot_app0-$$b.bin dist/$(APP)-$$b.bin _site/ || exit 1; \
	done
	$(SITE_EXTRA)
	node tools/gen_og.mjs _site/og.png
	@echo "Installer assembled in _site/"

# Web Serial only works over HTTPS or on localhost.
serve: $(if $(wildcard _site/index.html),,site) ## Serve _site/ on localhost:8000 (builds it first if missing)
	@echo
	@echo "Open http://localhost:8000 in Chrome or Edge."
	@echo
	python3 -m http.server 8000 -d _site

bump: ## Set the version in config.h and every docs/manifest*.json: make bump VERSION=X.Y.Z
	@test -n "$(VERSION)" || { echo "Missing version: make bump VERSION=0.2.0"; exit 1; }
	@echo "$(VERSION)" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$$' || { echo "Version must be X.Y.Z"; exit 1; }
	@sed -i.bak -E 's/(#define[[:space:]]+FW_VERSION[[:space:]]+")[^"]+(")/\1$(VERSION)\2/' \
		$(SKETCH)/config.h && rm -f $(SKETCH)/config.h.bak
	@for f in docs/manifest*.json; do \
	  sed -i.bak -E 's/("version"[[:space:]]*:[[:space:]]*")[^"]+(")/\1$(VERSION)\2/' $$f && rm -f $$f.bak || exit 1; \
	done
	@$(MAKE) --no-print-directory check
	@echo
	@echo "Version $(VERSION) set in config.h and the manifests. Once tested:"
	@echo "  git commit -am 'Version $(VERSION)' && git tag v$(VERSION) && git push --follow-tags"

# The common core: files that must stay identical in every repo on this base (core.lock.json).
core-check: ## Check the core files against core.lock.json (part of make check)
	@node tools/check_core.mjs check

core-diff: ## Compare the core files with another repo: make core-diff OTHER=../repo
	$(NEED_OTHER)
	@node tools/check_core.mjs diff $(OTHER)

core-sync: ## Copy the core files from another repo, keeping the app blocks: make core-sync OTHER=../repo
	$(NEED_OTHER)
	@node tools/check_core.mjs sync $(OTHER)

core-lock: ## Rewrite core.lock.json from the current core files
	@node tools/check_core.mjs lock

print-build: ## Print the build directory of BOARD
	@echo $(BUILD)

print-boards: ## Print the supported boards
	@echo $(BOARDS)

clean: ## Remove build artifacts, dist/ and the local site
	rm -rf build dist _site $(SKETCH)/build
