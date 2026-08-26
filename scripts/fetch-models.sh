#!/bin/sh
# Verified reference-model installer.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${HOME}/asngn
ROOT_SET=0
INSTALL_TOOLS=0
REPAIR=0

for arg in "$@"; do
  case "$arg" in
    --install-tools) INSTALL_TOOLS=1 ;;
    --repair) REPAIR=1 ;;
    --help)
      echo "usage: scripts/fetch-models.sh [engine-root] [--repair] [--install-tools]"
      exit 0 ;;
    --*) echo "unknown option: $arg" >&2; exit 2 ;;
    *)
      if [ "$ROOT_SET" -eq 1 ]; then
        echo "only one engine-root may be supplied" >&2; exit 2
      fi
      ROOT=$arg
      ROOT_SET=1 ;;
  esac
done

DIR=$ROOT/models
SOURCES=$DIR/.sources
mkdir -p "$DIR" "$SOURCES"

hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

verify() {
  [ -f "$1" ] || return 1
  [ "$(wc -c < "$1" | tr -d ' ')" = "$2" ] || return 1
  [ "$(hash_file "$1")" = "$3" ]
}

while IFS="	" read -r name size sha url post; do
  case "$name" in ''|'#'*) continue ;; esac
  dest=$DIR/$name
  if [ "$post" = "-" ] && verify "$dest" "$size" "$sha"; then
    echo "verified: $dest"
    continue
  fi
  if [ "$post" = "-" ] && [ -e "$dest" ] && [ "$REPAIR" -ne 1 ]; then
    echo "verification failed: $dest (use --repair to replace)" >&2
    exit 1
  fi
  if [ "$post" = "-" ]; then
    rm -f -- "$dest.part"
    echo "downloading verified model: $name"
    curl -L --fail --retry 3 -o "$dest.part" "$url"
    verify "$dest.part" "$size" "$sha" || {
      echo "download verification failed: $name" >&2; exit 1;
    }
    mv -f -- "$dest.part" "$dest"
    echo "installed: $dest"
    continue
  fi

  source=$SOURCES/$name
  if ! verify "$source" "$size" "$sha"; then
    if [ -e "$source" ] && [ "$REPAIR" -ne 1 ]; then
      echo "verification failed: $source (use --repair to replace)" >&2
      exit 1
    fi
    rm -f -- "$source.part"
    echo "downloading verified source: $name"
    curl -L --fail --retry 3 -o "$source.part" "$url"
    verify "$source.part" "$size" "$sha" || {
      echo "download verification failed: $name" >&2; exit 1;
    }
    mv -f -- "$source.part" "$source"
  fi

  cp -- "$source" "$dest.part"
  key=${post%%=*}
  value=${post#*=}
  python3 "$HERE/gguf_add_kv.py" "$dest.part" "$key" "$value"
  mv -f -- "$dest.part" "$dest"
  echo "installed: $dest"
done < "$HERE/models.manifest.tsv"

if [ "$INSTALL_TOOLS" -eq 1 ]; then
  PKG=$HERE/../build/packages
  TOOLS=$ROOT/tools
  [ -d "$PKG" ] || { echo "tool packages not found: $PKG" >&2; exit 1; }
  mkdir -p "$TOOLS"
  for src in "$PKG"/*; do
    [ -d "$src" ] || continue
    dst=$TOOLS/$(basename "$src")
    rm -rf -- "$dst"
    cp -R -- "$src" "$dst"
    echo "tool package: $(basename "$src")"
  done
fi
