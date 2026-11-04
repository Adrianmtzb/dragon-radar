# Dragon Radar: development shortcuts. Plain `make` lists the targets.
# The shared targets (deps, gen, check, build, flash, dist, site...) live in core.mk, shared by
# every project on the common base; this file only sets the app and adds its own targets.

APP        := dragonradar
TITLE      := Dragon Radar
BOARDS     := amoled
BOARD      ?= amoled
GENERATORS := tools/gen_radar.mjs

# Sibling repo on the same base, for make core-diff / core-sync.
CORE_PEER  := ../esp-led-matrix

include core.mk
