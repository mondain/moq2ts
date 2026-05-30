#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s <install-prefix>\n' "$0" >&2
  exit 2
fi

INSTALL_PREFIX="$1"
BINARY="$INSTALL_PREFIX/bin/moq2ts-publisher"
LIB_DIR="$INSTALL_PREFIX/lib"
PLUGIN_DIR="$INSTALL_PREFIX/plugins"

if [[ ! -x "$BINARY" ]]; then
  printf 'missing executable: %s\n' "$BINARY" >&2
  exit 1
fi

rm -rf "$LIB_DIR" "$PLUGIN_DIR"
mkdir -p "$LIB_DIR"

copy_dependencies() {
  local target="$1"
  ldd "$target" | awk '
    /=> \// { print $3 }
    /^[[:space:]]*\// { print $1 }
  ' | sort -u | while read -r library; do
    case "$(basename "$library")" in
      ld-linux-*|libc.so.*|libdl.so.*|libm.so.*|libpthread.so.*|librt.so.*)
        continue
        ;;
    esac
    cp -L "$library" "$LIB_DIR/"
  done
}

copy_dependencies "$BINARY"

qt_plugin_root=""
for candidate in /usr/lib/x86_64-linux-gnu/qt5/plugins /usr/lib/qt5/plugins; do
  if [[ -d "$candidate/platforms" ]]; then
    qt_plugin_root="$candidate"
    break
  fi
done

copy_qt_plugin() {
  local relative_path="$1"
  local source="$qt_plugin_root/$relative_path"
  local target="$PLUGIN_DIR/$relative_path"

  if [[ ! -f "$source" ]]; then
    printf 'missing Qt plugin: %s\n' "$source" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$target")"
  cp -a "$source" "$target"
}

if [[ -n "$qt_plugin_root" ]]; then
  qt_platform_plugins="${MOQ2TS_QT_PLATFORM_PLUGINS:-xcb}"
  for plugin in $qt_platform_plugins; do
    copy_qt_plugin "platforms/libq${plugin}.so"
  done

  if [[ -f "$qt_plugin_root/platforminputcontexts/libcomposeplatforminputcontextplugin.so" ]]; then
    copy_qt_plugin "platforminputcontexts/libcomposeplatforminputcontextplugin.so"
  fi
fi

while IFS= read -r plugin; do
  copy_dependencies "$plugin"
done < <(find "$PLUGIN_DIR" -type f -name '*.so' | sort)

previous_count=-1
current_count="$(find "$LIB_DIR" -type f | wc -l)"
while [[ "$current_count" != "$previous_count" ]]; do
  previous_count="$current_count"
  while IFS= read -r library; do
    copy_dependencies "$library"
  done < <(find "$LIB_DIR" -type f | sort)
  current_count="$(find "$LIB_DIR" -type f | wc -l)"
done

if command -v patchelf >/dev/null 2>&1; then
  patchelf --set-rpath '$ORIGIN/../lib' "$BINARY"
  while IFS= read -r library; do
    if patchelf --print-rpath "$library" >/dev/null 2>&1; then
      patchelf --set-rpath '$ORIGIN' "$library"
    fi
  done < <(find "$LIB_DIR" -type f | sort)
  while IFS= read -r plugin; do
    patchelf --set-rpath '$ORIGIN/../../lib' "$plugin"
  done < <(find "$PLUGIN_DIR" -type f -name '*.so' | sort)
fi

cat > "$INSTALL_PREFIX/bin/qt.conf" <<'QTCONF'
[Paths]
Plugins = ../plugins
QTCONF

cat > "$INSTALL_PREFIX/moq2ts-publisher-bookworm" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if mkdir -p "$APP_DIR/cache" 2>/dev/null; then
  export XDG_CACHE_HOME="$APP_DIR/cache"
else
  export XDG_CACHE_HOME="${TMPDIR:-/tmp}/moq2ts-publisher-cache-${UID:-user}"
  mkdir -p "$XDG_CACHE_HOME"
fi
export LD_LIBRARY_PATH="$APP_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$APP_DIR/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export QT_QPA_PLATFORM_PLUGIN_PATH="$APP_DIR/plugins/platforms"
exec "$APP_DIR/bin/moq2ts-publisher" "$@"
WRAPPER

chmod +x "$INSTALL_PREFIX/moq2ts-publisher-bookworm"
