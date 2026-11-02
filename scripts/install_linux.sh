#!/usr/bin/env bash
# ==============================================================================
# MoGRU (fcitx5-mogru) Installer for Linux
# ==============================================================================
# Installs MoGRU from a prebuilt release package (zip or directory).
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." 2>/dev/null && pwd || echo "$SCRIPT_DIR")"

echo "=== MoGRU (Fcitx5) Installer for Linux ==="

PACKAGE_ARG=""
if [ $# -ge 1 ]; then
  if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: $0 [ <path-to-MoGRU-linux.zip-or-extracted-dir> ]"
    echo "       (When run inside extracted MoGRU package, arguments can be omitted)"
    exit 0
  fi
  PACKAGE_ARG="$1"
else
  # Auto-detection when run without arguments inside extracted package or alongside zip
  if [ -f "$SCRIPT_DIR/fcitx5-mogru.so" ]; then
    PACKAGE_ARG="$SCRIPT_DIR"
    echo "[+] Auto-detected extracted package at: $SCRIPT_DIR"
  elif [ -f "./fcitx5-mogru.so" ]; then
    PACKAGE_ARG="."
    echo "[+] Auto-detected extracted package in current directory."
  elif [ -f "$SCRIPT_DIR/MoGRU-linux.zip" ]; then
    PACKAGE_ARG="$SCRIPT_DIR/MoGRU-linux.zip"
  elif [ -f "./MoGRU-linux.zip" ]; then
    PACKAGE_ARG="./MoGRU-linux.zip"
  elif [ -f "$REPO_ROOT/MoGRU-linux.zip" ]; then
    PACKAGE_ARG="$REPO_ROOT/MoGRU-linux.zip"
  else
    echo "[!] Error: No MoGRU package found."
    echo "    Please run this script inside an extracted MoGRU-linux package,"
    echo "    or specify the path to MoGRU-linux.zip / package directory:"
    echo "        $0 /path/to/MoGRU-linux.zip"
    exit 1
  fi
fi

TEMP_EXTRACT_DIR=""
cleanup() {
  if [ -n "$TEMP_EXTRACT_DIR" ] && [ -d "$TEMP_EXTRACT_DIR" ]; then
    rm -rf "$TEMP_EXTRACT_DIR"
  fi
}
trap cleanup EXIT

PACKAGE_DIR=""
if [ -f "$PACKAGE_ARG" ]; then
  ABS_FILE="$(cd "$(dirname "$PACKAGE_ARG")" && pwd)/$(basename "$PACKAGE_ARG")"
  if [[ "$ABS_FILE" == *.zip ]]; then
    TEMP_EXTRACT_DIR="$(mktemp -d)"
    echo "[+] Extracting package archive: $ABS_FILE"
    unzip -q "$ABS_FILE" -d "$TEMP_EXTRACT_DIR"
    # Find root of package (may be inside a subfolder like MoGRU-linux)
    if [ -f "$TEMP_EXTRACT_DIR/fcitx5-mogru.so" ]; then
      PACKAGE_DIR="$TEMP_EXTRACT_DIR"
    else
      SUBDIR="$(find "$TEMP_EXTRACT_DIR" -maxdepth 2 -name "fcitx5-mogru.so" -printf '%h\n' | head -n 1)"
      if [ -n "$SUBDIR" ]; then
        PACKAGE_DIR="$SUBDIR"
      else
        PACKAGE_DIR="$TEMP_EXTRACT_DIR"
      fi
    fi
  else
    echo "[!] Unexpected file argument: $ABS_FILE (expected .zip or directory)"
    exit 1
  fi
elif [ -d "$PACKAGE_ARG" ]; then
  PACKAGE_DIR="$(cd "$PACKAGE_ARG" && pwd)"
else
  echo "[!] Specified package path does not exist: $PACKAGE_ARG"
  exit 1
fi

# Locate core components
SOURCE_SO="$(find "$PACKAGE_DIR" -name "fcitx5-mogru.so" -type f -print -quit 2>/dev/null || true)"
if [ -z "$SOURCE_SO" ]; then
  SOURCE_SO="$(find "$PACKAGE_DIR" -name "*.so" -type f -print -quit 2>/dev/null || true)"
fi

if [ -z "$SOURCE_SO" ] || [ ! -f "$SOURCE_SO" ]; then
  echo "[!] Error: fcitx5-mogru.so was not found in package: $PACKAGE_DIR"
  exit 1
fi

SOURCE_TOOL="$(find "$PACKAGE_DIR" -name "mozc_tool" -type f -print -quit 2>/dev/null || true)"
if [ -z "$SOURCE_TOOL" ]; then
  SOURCE_TOOL="$(find "$PACKAGE_DIR" -name "mogru_tool" -type f -print -quit 2>/dev/null || true)"
fi

ICONS_DIR="$(find "$PACKAGE_DIR" -type d -name "icons" -print -quit 2>/dev/null || true)"
MODELS_DIR="$(find "$PACKAGE_DIR" -type d -name "models" -print -quit 2>/dev/null || true)"
if [ -z "$MODELS_DIR" ] && [ -d "$REPO_ROOT/models" ]; then
  MODELS_DIR="$REPO_ROOT/models"
fi

echo "[+] Using binary: $SOURCE_SO"
[ -n "$SOURCE_TOOL" ] && echo "[+] Using GUI tool: $SOURCE_TOOL"
[ -n "$MODELS_DIR" ] && echo "[+] Using models directory: $MODELS_DIR"

# Check Fcitx5 runtime
if ! command -v fcitx5 &>/dev/null; then
  echo "[*] Notice: fcitx5 command not found in PATH."
  echo "    Ensure Fcitx5 is installed on your system (e.g. sudo apt install fcitx5)."
fi

# 1. Create installation directories
echo "=== Installing MoGRU to ~/.local ==="
mkdir -p "$HOME/.local/lib/fcitx5"
mkdir -p "$HOME/.local/lib64/fcitx5"
mkdir -p "$HOME/.local/lib/mogru"
mkdir -p "$HOME/.local/share/fcitx5/addon"
mkdir -p "$HOME/.local/share/fcitx5/inputmethod"
mkdir -p "$HOME/.local/share/icons/hicolor/128x128/apps"
mkdir -p "$HOME/.local/share/icons/hicolor/48x48/apps"
mkdir -p "$HOME/.local/share/icons/hicolor/32x32/apps"
mkdir -p "$HOME/.local/share/icons/mogru"
mkdir -p "$HOME/.local/share/metainfo"
mkdir -p "$HOME/.local/share/applications"
mkdir -p "$HOME/.config/mogru"

# 2. Install shared library
rm -f "$HOME/.local/lib/fcitx5/fcitx5-mogru.so"
rm -f "$HOME/.local/lib64/fcitx5/fcitx5-mogru.so"
cp -f "$SOURCE_SO" "$HOME/.local/lib/fcitx5/fcitx5-mogru.so"
cp -f "$SOURCE_SO" "$HOME/.local/lib64/fcitx5/fcitx5-mogru.so"
chmod 755 "$HOME/.local/lib/fcitx5/fcitx5-mogru.so" "$HOME/.local/lib64/fcitx5/fcitx5-mogru.so"
echo "[+] Installed fcitx5-mogru.so"

# 3. Install GUI configuration tool
if [ -n "$SOURCE_TOOL" ] && [ -f "$SOURCE_TOOL" ]; then
  # Clean up existing tools and legacy mozc_tool to ensure mogru_tool is a clean standalone binary
  rm -f "$HOME/.local/lib/mogru/mogru_tool" "$HOME/.local/lib/mogru/mozc_tool"
  cp -f "$SOURCE_TOOL" "$HOME/.local/lib/mogru/mogru_tool"
  chmod 755 "$HOME/.local/lib/mogru/mogru_tool"
  echo "[+] Installed GUI tool: $HOME/.local/lib/mogru/mogru_tool"
fi

# 4. Install Icons
echo "=== Installing MoGRU Icons ==="

# Clean up any previously overwritten Mozc icons in user directories.
# This ensures standard Mozc uses system-installed icons and is not affected by MoGRU.
echo "[*] Cleaning up overwritten Mozc icons in ~/.local/share/icons..."
for mozc_icon in \
    "$HOME/.local/share/icons/hicolor/128x128/apps/fcitx_mozc.png" \
    "$HOME/.local/share/icons/hicolor/128x128/apps/fcitx-mozc.png" \
    "$HOME/.local/share/icons/hicolor/32x32/apps/fcitx_mozc.png" \
    "$HOME/.local/share/icons/hicolor/32x32/apps/fcitx-mozc.png" \
    "$HOME/.local/share/icons/hicolor/32x32/apps/org.fcitx.Fcitx5.fcitx_mozc.png" \
    "$HOME/.local/share/icons/hicolor/48x48/apps/fcitx_mozc_tool.png" \
    "$HOME/.local/share/icons/hicolor/48x48/apps/fcitx_mozc_properties.png" \
    "$HOME/.local/share/icons/hicolor/48x48/apps/fcitx_mozc_dictionary.png" \
    "$HOME/.local/share/icons/hicolor/48x48/apps/fcitx_mozc_"*.png; do
  if [ -e "$mozc_icon" ]; then
    rm -f "$mozc_icon"
  fi
done
rm -rf "$HOME/.local/share/icons/mozc"

if [ -n "$ICONS_DIR" ] && [ -d "$ICONS_DIR" ]; then
  if [ -d "$ICONS_DIR/hicolor" ]; then
    cp -rf "$ICONS_DIR/hicolor/"* "$HOME/.local/share/icons/hicolor/" 2>/dev/null || true
  fi
  if [ -d "$ICONS_DIR/mogru" ]; then
    cp -rf "$ICONS_DIR/mogru/"* "$HOME/.local/share/icons/mogru/" 2>/dev/null || true
  fi
fi

if [ -d "$REPO_ROOT/src/data/images" ]; then
  # Deploy exclusive MoGRU icon assets
  [ -f "$REPO_ROOT/src/data/images/product_icon_32bpp-128.png" ] && {
    cp -f "$REPO_ROOT/src/data/images/product_icon_32bpp-128.png" "$HOME/.local/share/icons/hicolor/128x128/apps/fcitx_mogru.png"
    cp -f "$REPO_ROOT/src/data/images/product_icon_32bpp-128.png" "$HOME/.local/share/icons/hicolor/128x128/apps/mogru.png"
    cp -f "$REPO_ROOT/src/data/images/product_icon_32bpp-128.png" "$HOME/.local/share/icons/mogru/product_icon_32bpp-128.png"
  }
  [ -f "$REPO_ROOT/src/data/images/unix/ime_product_icon_opensource-32.png" ] && {
    cp -f "$REPO_ROOT/src/data/images/unix/ime_product_icon_opensource-32.png" "$HOME/.local/share/icons/hicolor/32x32/apps/fcitx_mogru.png"
    cp -f "$REPO_ROOT/src/data/images/unix/ime_product_icon_opensource-32.png" "$HOME/.local/share/icons/hicolor/32x32/apps/mogru.png"
    cp -f "$REPO_ROOT/src/data/images/unix/ime_product_icon_opensource-32.png" "$HOME/.local/share/icons/hicolor/32x32/apps/org.fcitx.Fcitx5.Addon.MoGRU.png"
    cp -f "$REPO_ROOT/src/data/images/unix/ime_product_icon_opensource-32.png" "$HOME/.local/share/icons/mogru/ime_product_icon_opensource-32.png"
  }
  for mode in direct hiragana katakana_full katakana_half alpha_full alpha_half; do
    if [ -f "$REPO_ROOT/src/data/images/unix/48x48/${mode}.png" ]; then
      cp -f "$REPO_ROOT/src/data/images/unix/48x48/${mode}.png" "$HOME/.local/share/icons/hicolor/48x48/apps/fcitx_mogru_${mode}.png"
    fi
  done
  [ -f "$REPO_ROOT/src/data/images/unix/ui-tool.png" ] && \
    cp -f "$REPO_ROOT/src/data/images/unix/ui-tool.png" "$HOME/.local/share/icons/hicolor/48x48/apps/fcitx_mogru_tool.png"
  [ -f "$REPO_ROOT/src/data/images/unix/ui-properties.png" ] && \
    cp -f "$REPO_ROOT/src/data/images/unix/ui-properties.png" "$HOME/.local/share/icons/hicolor/48x48/apps/fcitx_mogru_properties.png"
  [ -f "$REPO_ROOT/src/data/images/unix/ui-dictionary.png" ] && \
    cp -f "$REPO_ROOT/src/data/images/unix/ui-dictionary.png" "$HOME/.local/share/icons/hicolor/48x48/apps/fcitx_mogru_dictionary.png"
  cp -f "$REPO_ROOT/src/data/images/unix/ui-"*.png "$HOME/.local/share/icons/mogru/" 2>/dev/null || true
fi

if command -v gtk-update-icon-cache &>/dev/null; then
  gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
fi

# 5. Deploy AppStream metadata & Desktop Entries
cat << 'EOF' > "$HOME/.local/share/metainfo/org.fcitx.Fcitx5.Addon.MoGRU.metainfo.xml"
<?xml version="1.0" encoding="utf-8"?>
<component type="addon">
  <id>org.fcitx.Fcitx5.Addon.MoGRU</id>
  <extends>org.fcitx.Fcitx5</extends>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>BSD-3-Clause</project_license>
  <name>MoGRU for Fcitx 5</name>
  <summary>Neural Language Model (GRU) Integrated Mozc Japanese Input Method</summary>
  <url type="homepage">https://github.com/lambda-haru/MoGRU</url>
  <project_group>Fcitx</project_group>
</component>
EOF

cat << EOF > "$HOME/.local/share/applications/mogru-tool.desktop"
[Desktop Entry]
Name=MoGRU Settings
Name[ja]=MoGRU 設定
Comment=MoGRU Configuration Dialog
Comment[ja]=MoGRU (ニューラル言語モデル日本語入力) の設定ツール
Exec=$HOME/.local/lib/mogru/mogru_tool --mode=config_dialog
Icon=fcitx_mogru
Terminal=false
Type=Application
Categories=Settings;Utility;
EOF

cat << EOF > "$HOME/.local/share/applications/mogru-dict.desktop"
[Desktop Entry]
Name=MoGRU Dictionary Tool
Name[ja]=MoGRU 辞書ツール
Comment=MoGRU Dictionary Management Tool
Comment[ja]=MoGRU ユーザー辞書の管理
Exec=$HOME/.local/lib/mogru/mogru_tool --mode=dictionary_tool
Icon=fcitx_mogru
Terminal=false
Type=Application
Categories=Settings;Utility;
EOF

# 6. Deploy Addon and Input Method definitions
cat << 'EOF' > "$HOME/.local/share/fcitx5/addon/mogru.conf"
[Addon]
Name=MoGRU
Category=InputMethod
Library=fcitx5-mogru
Type=SharedLibrary
OnDemand=True
Configurable=True

[Addon/OptionalDependencies]
0=clipboard
EOF

cat << 'EOF' > "$HOME/.local/share/fcitx5/inputmethod/mogru.conf"
[InputMethod]
Name=MoGRU
Icon=fcitx_mogru
Label=あ(M)
LangCode=ja
Addon=mogru
Configurable=True
EOF

# Clean up older mozc-gru references to prevent duplicates
rm -f "$HOME/.local/lib/fcitx5/fcitx5-mozc-gru.so"
rm -f "$HOME/.local/lib64/fcitx5/fcitx5-mozc-gru.so"
rm -f "$HOME/.local/share/fcitx5/addon/mozc-gru.conf"
rm -f "$HOME/.local/share/fcitx5/inputmethod/mozc-gru.conf"

# 7. Migrate old MoGRU configuration and learned model from ~/.config/mozc if present
if [ -d "$HOME/.config/mozc" ] || [ -d "$HOME/.mozc" ]; then
  for legacy_dir in "$HOME/.config/mozc" "$HOME/.mozc"; do
    if [ -d "$legacy_dir" ]; then
      if [ -f "$legacy_dir/gru_config.ini" ] && [ ! -f "$HOME/.config/mogru/gru_config.ini" ]; then
        echo "[+] Migrating existing gru_config.ini from $legacy_dir to ~/.config/mogru/ ..."
        cp -p "$legacy_dir/gru_config.ini" "$HOME/.config/mogru/gru_config.ini"
      fi
      if [ -f "$legacy_dir/grumodel_user.bin" ] && [ ! -f "$HOME/.config/mogru/grumodel_user.bin" ]; then
        echo "[+] Migrating existing grumodel_user.bin from $legacy_dir to ~/.config/mogru/ ..."
        cp -p "$legacy_dir/grumodel_user.bin" "$HOME/.config/mogru/grumodel_user.bin"
      fi
      rm -f "$legacy_dir/grumodel.bin" "$legacy_dir/chardata.bin" "$legacy_dir/gru_config.ini" "$legacy_dir/gru_config.ini.bak" "$legacy_dir/grumodel_user.bin"
    fi
  done
fi

# 8. Install models from models/ directory
# Base model files are installed to ~/.local/share/mogru/models (read-only data dir),
# while user configuration (gru_config.ini) and learned user model (grumodel_user.bin)
# reside in ~/.config/mogru.
TARGET_MODELS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/mogru/models"
TARGET_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/mogru"
mkdir -p "$TARGET_MODELS_DIR"
mkdir -p "$TARGET_CONFIG_DIR"

# Collect unique candidate models directories (package models takes top priority, then repo models)
CANDIDATE_MODEL_DIRS=()
[ -n "$MODELS_DIR" ] && [ -d "$MODELS_DIR" ] && CANDIDATE_MODEL_DIRS+=("$MODELS_DIR")
[ -d "$SCRIPT_DIR/models" ] && CANDIDATE_MODEL_DIRS+=("$SCRIPT_DIR/models")
[ -d "$REPO_ROOT/models" ] && CANDIDATE_MODEL_DIRS+=("$REPO_ROOT/models")
[ -d "./models" ] && CANDIDATE_MODEL_DIRS+=("./models")

UNIQUE_MODEL_DIRS=()
for d in "${CANDIDATE_MODEL_DIRS[@]}"; do
  if [ -d "$d" ]; then
    abs_d="$(cd "$d" 2>/dev/null && pwd || echo "$d")"
    already=0
    for u in "${UNIQUE_MODEL_DIRS[@]}"; do
      if [ "$u" = "$abs_d" ]; then
        already=1
        break
      fi
    done
    if [ "$already" -eq 0 ]; then
      UNIQUE_MODEL_DIRS+=("$abs_d")
    fi
  fi
done

echo "=== Installing MoGRU Models ==="
COPIED_MODELS=0
for d in "${UNIQUE_MODEL_DIRS[@]}"; do
  for file in "$d"/*; do
    if [ -f "$file" ]; then
      filename="$(basename "$file")"
      if [ "$filename" != "gru_config.ini" ]; then
        echo "[+] Copying model file: $filename (from $d) -> $TARGET_MODELS_DIR/"
        cp -f "$file" "$TARGET_MODELS_DIR/"
        COPIED_MODELS=$((COPIED_MODELS + 1))
      fi
    fi
  done
done

if [ "$COPIED_MODELS" -eq 0 ]; then
  echo "[!] Warning: No model files found in: ${UNIQUE_MODEL_DIRS[*]}"
else
  # Clean up duplicate base models previously copied into ~/.config/mogru/
  rm -f "$TARGET_CONFIG_DIR/grumodel.bin" "$TARGET_CONFIG_DIR/chardata.bin" \
        "$TARGET_CONFIG_DIR/grumodel_new.bin" "$TARGET_CONFIG_DIR/chardata_new.bin"
fi

# Deploy or upgrade gru_config.ini
TARGET_INI="$TARGET_CONFIG_DIR/gru_config.ini"
CONFIG_SRC=""
for d in "${UNIQUE_MODEL_DIRS[@]}"; do
  if [ -f "$d/gru_config.ini" ]; then
    CONFIG_SRC="$d/gru_config.ini"
    break
  fi
done

if [ -n "$CONFIG_SRC" ]; then
  if [ ! -f "$TARGET_INI" ]; then
    echo "[+] Initializing $TARGET_INI from default template ..."
    cp -f "$CONFIG_SRC" "$TARGET_INI"
  elif grep -q "katakana_penalty" "$TARGET_INI" || ! grep -q "enable_live_conversion" "$TARGET_INI"; then
    echo "[+] Upgrading $TARGET_INI to unified MoGRU config format (backup saved to $TARGET_INI.bak) ..."
    cp -f "$TARGET_INI" "$TARGET_INI.bak"
    cp -f "$CONFIG_SRC" "$TARGET_INI"
  else
    echo "[*] Preserving existing configuration: $TARGET_INI"
  fi
fi

# 9. Configure Fcitx5 directory and restart
mkdir -p "$HOME/.config/fcitx5/conf"

echo "=== Restarting Fcitx5 ==="
if command -v fcitx5 &>/dev/null; then
  FCITX_ADDON_DIRS="$HOME/.local/lib/fcitx5:$HOME/.local/lib64/fcitx5:/usr/lib64/fcitx5:/usr/lib/x86_64-linux-gnu/fcitx5:/usr/lib/fcitx5" fcitx5 -r -d >/dev/null 2>&1 </dev/null || true
  echo "[+] Fcitx5 restarted successfully."
else
  echo "[!] fcitx5 command not found in PATH. Please restart Fcitx5 manually."
fi

echo "=== Installation Complete! ==="
echo "You can now add 'MoGRU' in Fcitx5 Configuration (fcitx5-config-qt)."
echo "MoGRU settings can be customized via 'Configuration Tool' (mogru_tool) or ~/.config/mogru/gru_config.ini."
