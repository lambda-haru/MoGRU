#!/usr/bin/env bash
# ==============================================================================
# MoGRU Installer for macOS
# ==============================================================================
# Installs MoGRU from a prebuilt release package (zip, pkg, or directory).
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." 2>/dev/null && pwd || echo "$SCRIPT_DIR")"

echo "=== MoGRU Installer for macOS ==="

if [ $# -ge 1 ] && { [ "$1" = "-h" ] || [ "$1" = "--help" ]; }; then
  echo "Usage: $0 [ <path-to-MoGRU-macOS.zip-or-pkg-or-dir> ]"
  echo "       (When run inside extracted MoGRU package, arguments can be omitted)"
  exit 0
fi

# Check OS
if [[ "$(uname)" != "Darwin" ]]; then
  echo "[Error] This script must be run on macOS."
  exit 1
fi

PACKAGE_ARG=""
if [ $# -ge 1 ]; then
  PACKAGE_ARG="$1"
else
  # Auto-detection when run without arguments inside an extracted standalone package
  FOUND_LOCAL_PKG="$(find "$SCRIPT_DIR" -maxdepth 2 -name "*.pkg" -print -quit 2>/dev/null || true)"
  FOUND_LOCAL_APP="$(find "$SCRIPT_DIR" -maxdepth 2 -name "*.app" -type d -print -quit 2>/dev/null || true)"
  FOUND_LOCAL_ZIP="$(find "$SCRIPT_DIR" -maxdepth 2 -name "MoGRU-macOS-*.zip" -print -quit 2>/dev/null || true)"

  if [ -n "$FOUND_LOCAL_PKG" ]; then
    PACKAGE_ARG="$FOUND_LOCAL_PKG"
    echo "[+] Auto-detected package at: $FOUND_LOCAL_PKG"
  elif [ -n "$FOUND_LOCAL_APP" ]; then
    PACKAGE_ARG="$FOUND_LOCAL_APP"
    echo "[+] Auto-detected app bundle at: $FOUND_LOCAL_APP"
  elif [ -n "$FOUND_LOCAL_ZIP" ]; then
    PACKAGE_ARG="$FOUND_LOCAL_ZIP"
    echo "[+] Auto-detected package archive at: $FOUND_LOCAL_ZIP"
  else
    # Check current directory
    FOUND_CURR_PKG="$(find . -maxdepth 2 -name "*.pkg" -print -quit 2>/dev/null || true)"
    if [ -n "$FOUND_CURR_PKG" ]; then
      PACKAGE_ARG="$FOUND_CURR_PKG"
    else
      echo "[!] Error: No MoGRU package or installer found."
      echo "    Please run this script inside an extracted MoGRU-macOS package,"
      echo "    or specify the path to MoGRU-macOS-arm64.zip / MoGRU.pkg:"
      echo "        $0 /path/to/MoGRU-macOS-arm64.zip"
      exit 1
    fi
  fi
fi

TEMP_EXTRACT_DIR=""
cleanup() {
  if [ -n "$TEMP_EXTRACT_DIR" ] && [ -d "$TEMP_EXTRACT_DIR" ]; then
    rm -rf "$TEMP_EXTRACT_DIR"
  fi
}
trap cleanup EXIT

SOURCE_PKG=""
SOURCE_APP=""
PACKAGE_MODELS_DIR=""

if [ -f "$PACKAGE_ARG" ]; then
  ABS_FILE="$(cd "$(dirname "$PACKAGE_ARG")" && pwd)/$(basename "$PACKAGE_ARG")"
  if [[ "$ABS_FILE" == *.zip ]]; then
    TEMP_EXTRACT_DIR="$(mktemp -d)"
    echo "[+] Extracting package archive: $ABS_FILE"
    unzip -q "$ABS_FILE" -d "$TEMP_EXTRACT_DIR"
    SOURCE_PKG="$(find "$TEMP_EXTRACT_DIR" -name "*.pkg" -print -quit 2>/dev/null || true)"
    SOURCE_APP="$(find "$TEMP_EXTRACT_DIR" -name "*.app" -type d -print -quit 2>/dev/null || true)"
    PACKAGE_MODELS_DIR="$(find "$TEMP_EXTRACT_DIR" -type d -name "models" -print -quit 2>/dev/null || true)"
  elif [[ "$ABS_FILE" == *.pkg ]]; then
    SOURCE_PKG="$ABS_FILE"
    DIR_OF_PKG="$(dirname "$ABS_FILE")"
    if [ -d "$DIR_OF_PKG/models" ]; then
      PACKAGE_MODELS_DIR="$DIR_OF_PKG/models"
    fi
  else
    echo "[!] Unexpected file argument: $ABS_FILE (expected .zip or .pkg)"
    exit 1
  fi
elif [ -d "$PACKAGE_ARG" ]; then
  ABS_DIR="$(cd "$PACKAGE_ARG" && pwd)"
  if [[ "$ABS_DIR" == *.app ]]; then
    SOURCE_APP="$ABS_DIR"
  else
    SOURCE_PKG="$(find "$ABS_DIR" -maxdepth 2 -name "*.pkg" -print -quit 2>/dev/null || true)"
    SOURCE_APP="$(find "$ABS_DIR" -maxdepth 2 -name "*.app" -type d -print -quit 2>/dev/null || true)"
    PACKAGE_MODELS_DIR="$(find "$ABS_DIR" -type d -name "models" -print -quit 2>/dev/null || true)"
  fi
fi

if [ -z "$SOURCE_PKG" ] && [ -z "$SOURCE_APP" ]; then
  echo "[!] Error: Neither .pkg nor .app was found in: $PACKAGE_ARG"
  exit 1
fi

[ -n "$SOURCE_PKG" ] && echo "[+] Found installer package: $SOURCE_PKG"
[ -n "$SOURCE_APP" ] && echo "[+] Found application bundle: $SOURCE_APP"

echo "=== Installing MoGRU ==="

# 1. Clean up legacy MoGRU files from Mozc directories and stale duplicate base models
for legacy_dir in \
  "$HOME/Library/Application Support/Mozc" \
  "$HOME/Library/Application Support/Google/JapaneseInput" \
  "$HOME/.config/mozc"; do
  if [ -d "$legacy_dir" ]; then
    rm -f "$legacy_dir/grumodel.bin" "$legacy_dir/chardata.bin" "$legacy_dir/gru_config.ini" "$legacy_dir/gru_config.ini.bak" "$legacy_dir/grumodel_user.bin"
  fi
done

for user_dir in \
  "$HOME/Library/Application Support/MoGRU" \
  "$HOME/.config/mogru"; do
  if [ -d "$user_dir" ]; then
    rm -f "$user_dir/grumodel.bin" "$user_dir/chardata.bin" "$user_dir/grumodel_new.bin" "$user_dir/chardata_new.bin"
  fi
done

# 2. Install binary package or application bundle
# Note: MoGRU.pkg already bundles the base models inside
# /Library/Input Methods/MoGRU.app/Contents/Resources/models/, and
# GruConfigManager initializes ~/Library/Application Support/MoGRU/gru_config.ini
# automatically on first launch.
if [ -n "$SOURCE_PKG" ] && [ -e "$SOURCE_PKG" ]; then
  echo "[+] Installing package: $SOURCE_PKG"
  if command -v sudo &>/dev/null; then
    echo "Running installer (requires administrator password)..."
    sudo installer -pkg "$SOURCE_PKG" -target / || {
      echo "[!] sudo installer failed, opening installer window..."
      open -W "$SOURCE_PKG"
    }
  else
    open -W "$SOURCE_PKG"
  fi
elif [ -n "$SOURCE_APP" ] && [ -d "$SOURCE_APP" ]; then
  TARGET_APP_DIR="$HOME/Library/Input Methods"
  mkdir -p "$TARGET_APP_DIR"
  killall MoGRUConverter 2>/dev/null || true
  killall MoGRURenderer 2>/dev/null || true
  killall MoGRU 2>/dev/null || true
  killall MozcConverter 2>/dev/null || true
  killall MozcRenderer 2>/dev/null || true
  killall Mozc 2>/dev/null || true
  rm -rf "$TARGET_APP_DIR/MoGRU.app" "$TARGET_APP_DIR/Mozc.app"
  cp -R "$SOURCE_APP" "$TARGET_APP_DIR/MoGRU.app"
  if [ ! -f "$TARGET_APP_DIR/MoGRU.app/Contents/Resources/models/grumodel.bin" ] && [ -d "$REPO_ROOT/models" ]; then
    mkdir -p "$TARGET_APP_DIR/MoGRU.app/Contents/Resources/models"
    cp -f "$REPO_ROOT/models/"*.bin "$TARGET_APP_DIR/MoGRU.app/Contents/Resources/models/" 2>/dev/null || true
  fi
  echo "[+] MoGRU.app copied to $TARGET_APP_DIR/"
fi

echo "=== Installation Complete! ==="
echo "Please follow these steps to enable MoGRU:"
echo "1. Open 'System Settings' -> 'Keyboard' -> 'Text Input' -> 'Input Sources'."
echo "2. Click '+' and add 'Japanese (MoGRU)'."
echo "3. If MoGRU does not appear, log out and log back in."
