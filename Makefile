# Dragon Radar: development shortcuts. Plain `make` lists the targets.
# The shared targets (deps, gen, check, build, flash, dist, site...) live in core.mk, shared by
# every project on the common base; this file only sets the app and adds its own targets.

APP        := dragonradar
TITLE      := Dragon Radar
BOARDS     := amoled
BOARD      ?= amoled
GENERATORS := tools/gen_radar.mjs
# The landing's demo draws with the same renderer as the preview and the web panel: make site
# copies it next to docs/index.html (which loads it as renderer.js) instead of a second copy.
SITE_EXTRA := cp tools/preview/renderer.js _site/renderer.js

# Sibling repo on the same base, for make core-diff / core-sync.
CORE_PEER  := ../esp-led-matrix

include core.mk

.PHONY: mcp-install

mcp-install: ## Install the MCP server dependencies (mcp/node_modules)
	cd mcp && npm install
