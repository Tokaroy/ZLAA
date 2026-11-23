#!/bin/sh
# ZLAA Builder: builds ZLAA's game code from your own Link's Awakening dump (needs python3, 3.10 or newer).
#   ./build-game.sh <your dump's folder, the one with exefs and romfs> [--push]
exec python3 "$(dirname "$0")/builder/zlaa_build.py" "$@"
