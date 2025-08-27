#!/usr/bin/env bash
set -euo pipefail

# Unified mobile build for PGLite native prebuilt libs
# Platforms: ANDROID (arm64-v8a, x86_64) and iOS (arm64, x86_64-sim)
#
# Usage examples:
#   PLATFORM=android ABI=arm64-v8a ANDROID_NDK=$HOME/Android/Sdk/ndk/27.1.12297006 \
#     bash postgres-pglite/mobile-build/build-mobile.sh
#
#   PLATFORM=ios ARCH=arm64 \
#     bash postgres-pglite/mobile-build/build-mobile.sh
#
# Outputs:
#   dist/mobile/android/<abi>/{include,lib,runtime/share/postgresql}
#   dist/mobile/ios/<arch>/{include,lib,runtime/share/postgresql}

PLATFORM=${PLATFORM:-android}  # android|ios
ABI=${ABI:-arm64-v8a}          # android ABI (arm64-v8a|x86_64)
ARCH=${ARCH:-arm64}            # ios arch (arm64|x86_64-sim)
API=${API:-24}                 # android min API for toolchain wrapper
PG_BRANCH=${PG_BRANCH:-REL_17_5_WASM}

# Resolve paths
SCRIPT_ABS=$(cd "$(dirname "$0")" && pwd)/"$(basename "$0")"
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)            # postgres-pglite
FLAKE_ROOT=$(cd "$REPO_ROOT/.." && pwd)                  # repo root (has flake.nix)
PGSRC=${PGSRC:-"$REPO_ROOT/postgresql-${PG_BRANCH}"}
BUILD_BASE="$REPO_ROOT/out/mobile"
DIST_BASE="$REPO_ROOT/dist/mobile"

# If nix is available and we are not already inside a nix devShell, re-enter via nix
maybe_enter_nix() {
  if [ "${NO_NIX:-0}" != "1" ] && command -v nix >/dev/null 2>&1; then
    if [ -z "${IN_NIX_SHELL:-}" ]; then
      case "$PLATFORM" in
        android) ATTR="android" ;;
        ios)     ATTR="ios" ;;
        *) echo "Unknown PLATFORM '$PLATFORM' (expected android|ios)" >&2; exit 2 ;;
      esac
      echo "Re-executing inside Nix devShell: $ATTR"
      exec nix develop "$FLAKE_ROOT#${ATTR}" --command bash -lc \
        "PLATFORM='$PLATFORM' ABI='$ABI' ARCH='$ARCH' API='$API' PG_BRANCH='$PG_BRANCH' NO_NIX=1 '$SCRIPT_ABS'"
    fi
  fi
}

maybe_enter_nix

# Ensure Perl (needed by PostgreSQL build) and GNU Make
require_tools() {
  if ! command -v perl >/dev/null 2>&1; then
    echo "Perl is required. On macOS: brew install perl" >&2
    exit 2
  fi
  # Prefer GNU make under common names: gmake, gnumake, then make if GNU
  if command -v gmake >/dev/null 2>&1; then
    MAKE_BIN=$(command -v gmake)
  elif command -v gnumake >/dev/null 2>&1; then
    MAKE_BIN=$(command -v gnumake)
  else
    MAKE_BIN=$(command -v make)
    if ! "$MAKE_BIN" --version 2>&1 | head -n1 | grep -qi "GNU Make"; then
      echo "GNU make is required. On macOS: brew install make (use gmake) or rely on Nix devShell" >&2
      exit 2
    fi
  fi
  export PERL=$(command -v perl)
  export MAKE="$MAKE_BIN"
}

# Android toolchain setup (NDK)
setup_android() {
  : "${ANDROID_NDK:?ANDROID_NDK must be set}"
  case "$ABI" in
    arm64-v8a) TRIPLE=aarch64-linux-android ;;
    x86_64)    TRIPLE=x86_64-linux-android ;;
    *) echo "Unsupported ABI: $ABI" >&2; exit 2;;
  esac
  # Locate a valid NDK LLVM toolchain directory. Nix androidenv layouts can differ.
  TOOL=""
  if [ -d "$ANDROID_NDK/toolchains/llvm/prebuilt" ]; then
    # Preferred: prebuilt host-tagged dirs
    FIRST_PREBUILT=""
    for d in "$ANDROID_NDK"/toolchains/llvm/prebuilt/*; do
      [ -d "$d" ] || continue
      [ -z "$FIRST_PREBUILT" ] && FIRST_PREBUILT="$d"
      if [ -x "$d/bin/${TRIPLE}${API}-clang" ] && [ -x "$d/bin/${TRIPLE}${API}-clang++" ]; then
        TOOL="$d"
        break
      fi
    done
    [ -z "$TOOL" ] && [ -n "$FIRST_PREBUILT" ] && TOOL="$FIRST_PREBUILT"
  fi
  if [ -z "$TOOL" ] && [ -d "$ANDROID_NDK/toolchains/llvm/bin" ]; then
    # Some Nix NDKs expose llvm/bin directly without prebuilt/
    TOOL="$ANDROID_NDK/toolchains/llvm"
  fi
  if [ -z "$TOOL" ]; then
    # Last resort: find a llvm/* dir with bin/clang
    for d in "$ANDROID_NDK"/toolchains/llvm/*; do
      [ -d "$d" ] || continue
      if [ -x "$d/bin/clang" ]; then TOOL="$d"; break; fi
    done
  fi
  if [ -z "$TOOL" ]; then
    echo "No valid NDK LLVM toolchain found under $ANDROID_NDK/toolchains/llvm" >&2
    exit 2
  fi
  echo "Using NDK toolchain at: $TOOL"

  WRAP_CC="$TOOL/bin/${TRIPLE}${API}-clang"
  WRAP_CXX="$TOOL/bin/${TRIPLE}${API}-clang++"
  if [ -x "$WRAP_CC" ] && [ -x "$WRAP_CXX" ]; then
    export CC="$WRAP_CC"
    export CXX="$WRAP_CXX"
  else
    export CC="$TOOL/bin/clang --target=${TRIPLE}${API} --sysroot=$TOOL/sysroot"
    export CXX="$TOOL/bin/clang++ --target=${TRIPLE}${API} --sysroot=$TOOL/sysroot"
  fi
  export AR="$TOOL/bin/llvm-ar"
  export RANLIB="$TOOL/bin/llvm-ranlib"
  export STRIP="$TOOL/bin/llvm-strip"
  export LD="$TOOL/bin/ld.lld"
  export CFLAGS="-fPIC -O2 -DHAVE_SPINLOCKS"
  export LDFLAGS="-fPIC"

  # Keep flags minimal; DSM selection is handled via config.site-android and configure
  export CPPFLAGS="${CPPFLAGS:-} -DHAVE_SPINLOCKS -D__ANDROID__ -DPGL_MOBILE"

  BUILD_DIR="$BUILD_BASE/android/$ABI"
  DIST_DIR="$DIST_BASE/android/$ABI"
  CONFIG_SITE_FILE="$REPO_ROOT/mobile-build/config.site-android"
}

# iOS toolchain setup (Xcode)
setup_ios() {
  if [[ "$ARCH" == "arm64" ]]; then
    SDK=iphoneos
    CC_BIN=$(xcrun --sdk iphoneos -f clang)
    CXX_BIN=$(xcrun --sdk iphoneos -f clang++)
    TRIPLE=arm-apple-darwin
    ARCH_FLAG="-arch arm64"
    MIN_FLAG="-miphoneos-version-min=13.0"
    OUT_ARCH_DIR="arm64"
  else
    SDK=iphonesimulator
    CC_BIN=$(xcrun --sdk iphonesimulator -f clang)
    CXX_BIN=$(xcrun --sdk iphonesimulator -f clang++)
    TRIPLE=x86_64-apple-darwin
    ARCH_FLAG="-arch x86_64"
    MIN_FLAG="-mios-simulator-version-min=13.0"
    OUT_ARCH_DIR="x86_64-sim"
  fi
  SDKROOT=$(xcrun --sdk "$SDK" --show-sdk-path)
  export CC="$CC_BIN $ARCH_FLAG -isysroot $SDKROOT $MIN_FLAG"
  export CXX="$CXX_BIN $ARCH_FLAG -isysroot $SDKROOT $MIN_FLAG"
  export AR=$(xcrun -f ar)
  export RANLIB=$(xcrun -f ranlib)
  export STRIP=$(xcrun -f strip)
  export CFLAGS="-fPIC -O2 -DHAVE_SPINLOCKS"
  export LDFLAGS="-fPIC"
  export CPPFLAGS="${CPPFLAGS:-} -DHAVE_SPINLOCKS -DPGL_MOBILE"

  BUILD_DIR="$BUILD_BASE/ios/$OUT_ARCH_DIR"
  DIST_DIR="$DIST_BASE/ios/$OUT_ARCH_DIR"
  CONFIG_SITE_FILE="$REPO_ROOT/mobile-build/config.site-ios"
}

# Common configure flags (aligned with wasm build spirit)
configure_flags() {
  CNF_COMMON=(
    "--disable-largefile" "--without-llvm"
    "--without-pam" "--with-openssl=no" "--without-readline"
    "--without-icu" "--without-libxml"
  )
}

# Build PostgreSQL and package artifacts
build_pg() {
  mkdir -p "$BUILD_DIR" "$DIST_DIR/lib" "$DIST_DIR/include"

  require_tools
  configure_flags

  echo "Configuring PostgreSQL (in-tree) in $PGSRC, install prefix=$BUILD_DIR/install ..."
  pushd "$PGSRC" >/dev/null
  CONFIG_SITE="$CONFIG_SITE_FILE" ./configure --host=${TRIPLE} \
    --prefix="$BUILD_DIR/install" "${CNF_COMMON[@]}"

  # Detect GNU make
  if command -v gmake >/dev/null 2>&1; then MAKE_BIN=$(command -v gmake); else MAKE_BIN=$(command -v make); fi
  NCPU=$( (sysctl -n hw.ncpu 2>/dev/null) || (nproc 2>/dev/null) || echo 1 )

  # Clean any previous object files to avoid mixing different CFLAGS
  "$MAKE_BIN" clean || true

  # Ensure a clean rebuild to avoid mixing objects compiled with different CFLAGS
  "$MAKE_BIN" clean || true
  find src -name '*.o' -type f -delete || true

  # Build full tree (lets PostgreSQL pick correct backend units), then install headers and data
  "$MAKE_BIN" -j"$NCPU"
  "$MAKE_BIN" install

  # Sanity-check: print HAVE_SPINLOCKS from the installed pg_config.h
  echo "HAVE_SPINLOCKS in installed headers:" && grep -n "HAVE_SPINLOCKS" "$BUILD_DIR/install/include/pg_config.h" || true

  popd >/dev/null

  # Headers and runtime
  cp -R "$BUILD_DIR/install/include/"* "$DIST_DIR/include/"

  # Ensure the catalog assets (share/postgresql) are present and complete
  # Prefer installed artifacts; handle cases where postgres.bki is under share/ (not share/postgresql)
  INSTALL_SHARE_POSTGRESQL="$BUILD_DIR/install/share/postgresql"
  INSTALL_SHARE="$BUILD_DIR/install/share"
  if [ ! -d "$INSTALL_SHARE_POSTGRESQL" ]; then
    mkdir -p "$INSTALL_SHARE_POSTGRESQL"
  fi
  # Ensure postgres.bki lands under share/postgresql
  if [ ! -f "$INSTALL_SHARE_POSTGRESQL/postgres.bki" ]; then
    if [ -f "$INSTALL_SHARE/postgres.bki" ]; then
      cp -f "$INSTALL_SHARE/postgres.bki" "$INSTALL_SHARE_POSTGRESQL/"
    elif [ -f "$PGSRC/src/backend/catalog/postgres.bki" ]; then
      cp -f "$PGSRC/src/backend/catalog/postgres.bki" "$INSTALL_SHARE_POSTGRESQL/"
    else
      echo "Warning: postgres.bki not found in install or source; initdb will fail" >&2
    fi
  fi
  # Copy the known SQL/text inputs; prefer installed share dir if present
  for f in \
    system_views.sql system_functions.sql system_constraints.sql \
    information_schema.sql sql_features.txt snowball_create.sql \
    postgresql.conf.sample pg_hba.conf.sample pg_ident.conf.sample
  do
    if [ ! -f "$INSTALL_SHARE_POSTGRESQL/$f" ]; then
      if [ -f "$INSTALL_SHARE_POSTGRESQL/$f" ]; then
        : # already there
      elif [ -f "$INSTALL_SHARE/$f" ]; then
        cp -f "$INSTALL_SHARE/$f" "$INSTALL_SHARE_POSTGRESQL/"
      elif [ -f "$PGSRC/src/backend/catalog/$f" ]; then
        cp -f "$PGSRC/src/backend/catalog/$f" "$INSTALL_SHARE_POSTGRESQL/" || true
      elif [ -f "$PGSRC/src/backend/snowball/$f" ]; then
        cp -f "$PGSRC/src/backend/snowball/$f" "$INSTALL_SHARE_POSTGRESQL/" || true
      elif [ -f "$PGSRC/src/backend/utils/misc/$f" ]; then
        cp -f "$PGSRC/src/backend/utils/misc/$f" "$INSTALL_SHARE_POSTGRESQL/" || true
      fi
    fi
  done

  if [ -d "$INSTALL_SHARE_POSTGRESQL" ]; then
    mkdir -p "$DIST_DIR/runtime/share"
    cp -R "$INSTALL_SHARE_POSTGRESQL" "$DIST_DIR/runtime/share/"
  fi

  # Also copy timezone data (share/timezone), abbreviation sets (share/timezonesets),
  # text search dictionaries (share/tsearch_data), and extensions (share/extension)
  local INSTALL_SHARE_TZ="$BUILD_DIR/install/share/timezone"
  local INSTALL_SHARE_TZSETS="$BUILD_DIR/install/share/timezonesets"
  local INSTALL_SHARE_TSEARCH="$BUILD_DIR/install/share/tsearch_data"
  local INSTALL_SHARE_EXT="$BUILD_DIR/install/share/extension"
  if [ -d "$INSTALL_SHARE_TZ" ]; then
    mkdir -p "$DIST_DIR/runtime/share"
    cp -R "$INSTALL_SHARE_TZ" "$DIST_DIR/runtime/share/"
  else
    echo "Warning: timezone directory not found at $INSTALL_SHARE_TZ"
  fi
  if [ -d "$INSTALL_SHARE_TZSETS" ]; then
    mkdir -p "$DIST_DIR/runtime/share"
    cp -R "$INSTALL_SHARE_TZSETS" "$DIST_DIR/runtime/share/"
  else
    echo "Warning: timezonesets directory not found at $INSTALL_SHARE_TZSETS"
  fi
  if [ -d "$INSTALL_SHARE_TSEARCH" ]; then
    mkdir -p "$DIST_DIR/runtime/share"
    cp -R "$INSTALL_SHARE_TSEARCH" "$DIST_DIR/runtime/share/"
  else
    echo "Warning: tsearch_data directory not found at $INSTALL_SHARE_TSEARCH"
    # Fallback: copy snowball and tsearch_data from source directories if present
    if [ -d "$PGSRC/src/backend/snowball" ]; then
      mkdir -p "$DIST_DIR/runtime/share/tsearch_data"
      cp -f "$PGSRC"/src/backend/snowball/stopwords/*.stop "$DIST_DIR/runtime/share/tsearch_data/" 2>/dev/null || true
      cp -f "$PGSRC"/src/backend/snowball/snowball_create.sql "$DIST_DIR/runtime/share/postgresql/" 2>/dev/null || true
      echo "Added snowball stopwords and snowball_create.sql from source"
    fi
    if [ -d "$PGSRC/src/backend/tsearch" ]; then
      mkdir -p "$DIST_DIR/runtime/share/tsearch_data"
      cp -f "$PGSRC"/src/backend/tsearch/*.dict "$DIST_DIR/runtime/share/tsearch_data/" 2>/dev/null || true
      cp -f "$PGSRC"/src/backend/tsearch/*.affix "$DIST_DIR/runtime/share/tsearch_data/" 2>/dev/null || true
      echo "Added tsearch dictionaries/affixes from source"
    fi
  fi
  if [ -d "$INSTALL_SHARE_EXT" ]; then
    mkdir -p "$DIST_DIR/runtime/share"
    cp -R "$INSTALL_SHARE_EXT" "$DIST_DIR/runtime/share/"
  else
    echo "Note: extension directory not found at $INSTALL_SHARE_EXT (ok if not building contrib)"
    # Fallback: copy core plpgsql extension files from source tree if present
    if [ -f "$PGSRC/src/pl/plpgsql/src/plpgsql.control" ]; then
      mkdir -p "$DIST_DIR/runtime/share/extension"
      cp -f "$PGSRC/src/pl/plpgsql/src/plpgsql.control" "$DIST_DIR/runtime/share/extension/"
      # Copy all versioned SQLs for plpgsql
      for sql in "$PGSRC"/src/pl/plpgsql/src/plpgsql--*.sql; do
        [ -f "$sql" ] && cp -f "$sql" "$DIST_DIR/runtime/share/extension/"
      done
      echo "Added plpgsql extension files from source tree"
    fi
  fi
}


# Build glue and merge libs
build_glue_and_merge() {
  pushd "$BUILD_DIR" >/dev/null

  # Glue: mirror WASM by compiling pg_main.c (which includes interactive_one.c, pgl_mains.c, etc.)
  ${CC%% *} \
    -I"$BUILD_DIR/install/include" -I"$PGSRC/src/include" -I"$PGSRC/src" \
    -I"$PGSRC/src/interfaces/libpq" \
    -I"$REPO_ROOT/mobile-build" \
    -include "$REPO_ROOT/mobile-build/wasm_common_mobile.h" \
    -include "$REPO_ROOT/mobile-build/pgl_mobile_compat.h" \
    -DPGL_LIB_ONLY -DPGL_MOBILE -UHAVE_SHM_OPEN \
    -fPIC -c "$REPO_ROOT/pglite-wasm/pg_main.c" -o pg_main.o
  # Glue extras: provide mobile alias and minimal helpers
  ${CC%% *} \
    -I"$BUILD_DIR/install/include" -I"$PGSRC/src/include" -I"$PGSRC/src" \
    -I"$REPO_ROOT/mobile-build" -DPGL_MOBILE -fPIC -c "$REPO_ROOT/mobile-build/pgl_mobile_shims.c" -o pgl_mobile_shims.o
  # Backend weak stubs to satisfy backend-only globals when not linking postmaster
  ${CC%% *} \
    -I"$BUILD_DIR/install/include" -I"$PGSRC/src/include" -I"$PGSRC/src" \
    -I"$REPO_ROOT/mobile-build" -DPGL_MOBILE -fPIC -c "$REPO_ROOT/mobile-build/pgl_backend_stubs.c" -o pgl_backend_stubs.o
  # Mobile CMA shim providing get_buffer_addr/get_buffer_size
  ${CC%% *} \
    -I"$BUILD_DIR/install/include" -I"$PGSRC/src/include" -I"$PGSRC/src" \
    -I"$REPO_ROOT/mobile-build" -DPGL_MOBILE -fPIC -c "$REPO_ROOT/mobile-build/sdk_port-mobile.c" -o sdk_port-mobile.o
  # Mobile comm methods to route libpq I/O to CMA
  ${CC%% *} \
    -I"$BUILD_DIR/install/include" -I"$PGSRC/src/include" -I"$PGSRC/src" \
    -I"$REPO_ROOT/mobile-build" -DPGL_MOBILE -fPIC -c "$REPO_ROOT/mobile-build/pgl_mobile_comm.c" -o pgl_mobile_comm.o

  # Glue archive; includes pg_main and mobile shims/stubs
  "$AR" rcs libpglite_glue_mobile.a pg_main.o pgl_mobile_shims.o pgl_backend_stubs.o sdk_port-mobile.o pgl_mobile_comm.o

  # Merge core PG libs (reuse the server build outputs like WASM)
  # Prefer consuming installed archives if present; otherwise fall back to server variants in source tree
  pushd "$BUILD_DIR" >/dev/null

  # Collect backend object files into MRI ADDMOD lines
  BACKEND_MRI="$BUILD_DIR/backend_objs.mri"
  : > "$BACKEND_MRI"
  while IFS= read -r obj; do
    echo "ADDMOD $obj" >> "$BACKEND_MRI"
  done < <(find "$PGSRC/src/backend" -name '*.o' -type f | sort)

  # Also collect timezone objects (needed by timestamp/varlena etc.)
  TZ_MRI="$BUILD_DIR/tz_objs.mri"
  : > "$TZ_MRI"
  while IFS= read -r obj; do
    echo "ADDMOD $obj" >> "$TZ_MRI"
  done < <(find "$PGSRC/src/timezone" -name '*.o' -type f | sort)

  # Create a single MRI script that creates and fills the archive (always use server variants)
  cat > merge_all.mri <<EOF
CREATE libpgcore_mobile.a
ADDLIB $PGSRC/src/common/libpgcommon_srv.a
ADDLIB $PGSRC/src/port/libpgport_srv.a
$(cat "$BACKEND_MRI")
$(cat "$TZ_MRI")
SAVE
END
EOF
  "$AR" -M < merge_all.mri
  popd >/dev/null

  # Additional mobile-only weak stubs remain allowed if specific postmaster-only globals are unresolved

  mv -f libpglite_glue_mobile.a "$DIST_DIR/lib/" || true
  mv -f libpgcore_mobile.a "$DIST_DIR/lib/"

  popd >/dev/null
}

# Copy artifacts into the React Native module for Android
copy_into_rn_android() {
  local RN_DIR="$FLAKE_ROOT/packages/pglite-react-native/android"
  local ABI_DIR
  case "$ABI" in
    arm64-v8a) ABI_DIR="arm64-v8a" ;;
    x86_64)    ABI_DIR="x86_64" ;;
    *) echo "Unsupported ABI: $ABI" >&2; return 1;;
  esac

  echo "Installing artifacts into RN module at $RN_DIR"
  mkdir -p "$RN_DIR/src/main/jni/$ABI_DIR" || true
  cp -f "$DIST_DIR/lib/libpgcore_mobile.a" "$RN_DIR/src/main/jni/$ABI_DIR/"
  cp -f "$DIST_DIR/lib/libpglite_glue_mobile.a" "$RN_DIR/src/main/jni/$ABI_DIR/"

  # Copy runtime catalogs (share/postgresql) and timezone assets into RN assets
  local ASSET_ROOT="$RN_DIR/src/main/assets/pglite"
  local ASSET_SHARE="$ASSET_ROOT/share"
  # postgresql catalogs
  if [ -d "$DIST_DIR/runtime/share/postgresql" ]; then
    local ASSET_PG_DST="$ASSET_SHARE/postgresql"
    mkdir -p "$ASSET_PG_DST"
    rsync -a --delete "$DIST_DIR/runtime/share/postgresql/" "$ASSET_PG_DST/" 2>/dev/null || {
      rm -rf "$ASSET_PG_DST"/*
      cp -R "$DIST_DIR/runtime/share/postgresql/." "$ASSET_PG_DST/"
    }
    echo "Copied runtime catalogs to $ASSET_PG_DST"
  else
    echo "Warning: runtime catalogs not found at $DIST_DIR/runtime/share/postgresql"
  fi
  # timezone directory
  if [ -d "$DIST_DIR/runtime/share/timezone" ]; then
    local ASSET_TZ_DST="$ASSET_SHARE/timezone"
    mkdir -p "$ASSET_TZ_DST"
    rsync -a --delete "$DIST_DIR/runtime/share/timezone/" "$ASSET_TZ_DST/" 2>/dev/null || {
      rm -rf "$ASSET_TZ_DST"/*
      cp -R "$DIST_DIR/runtime/share/timezone/." "$ASSET_TZ_DST/"
    }
    echo "Copied timezone data to $ASSET_TZ_DST"
  else
    echo "Warning: timezone data directory not found at $DIST_DIR/runtime/share/timezone"
  fi
  # timezonesets directory
  if [ -d "$DIST_DIR/runtime/share/timezonesets" ]; then
    local ASSET_TZSETS_DST="$ASSET_SHARE/timezonesets"
    mkdir -p "$ASSET_TZSETS_DST"
    rsync -a --delete "$DIST_DIR/runtime/share/timezonesets/" "$ASSET_TZSETS_DST/" 2>/dev/null || {
      rm -rf "$ASSET_TZSETS_DST"/*
      cp -R "$DIST_DIR/runtime/share/timezonesets/." "$ASSET_TZSETS_DST/"
    }
    echo "Copied timezone abbreviation sets to $ASSET_TZSETS_DST"
  else
    echo "Warning: timezone abbreviation sets not found at $DIST_DIR/runtime/share/timezonesets"
  fi
  # tsearch_data directory
  if [ -d "$DIST_DIR/runtime/share/tsearch_data" ]; then
    local ASSET_TSEARCH_DST="$ASSET_SHARE/tsearch_data"
    mkdir -p "$ASSET_TSEARCH_DST"
    rsync -a --delete "$DIST_DIR/runtime/share/tsearch_data/" "$ASSET_TSEARCH_DST/" 2>/dev/null || {
      rm -rf "$ASSET_TSEARCH_DST"/*
      cp -R "$DIST_DIR/runtime/share/tsearch_data/." "$ASSET_TSEARCH_DST/"
    }
    echo "Copied tsearch_data to $ASSET_TSEARCH_DST"
  else
    echo "Note: tsearch_data not found at $DIST_DIR/runtime/share/tsearch_data"
  fi
  # extension directory
  if [ -d "$DIST_DIR/runtime/share/extension" ]; then
    local ASSET_EXT_DST="$ASSET_SHARE/extension"
    mkdir -p "$ASSET_EXT_DST"
    rsync -a --delete "$DIST_DIR/runtime/share/extension/" "$ASSET_EXT_DST/" 2>/dev/null || {
      rm -rf "$ASSET_EXT_DST"/*
      cp -R "$DIST_DIR/runtime/share/extension/." "$ASSET_EXT_DST/"
    }
    echo "Copied extensions to $ASSET_EXT_DST"
  else
    echo "Note: extensions directory not found at $DIST_DIR/runtime/share/extension"
  fi

  # Copy base cluster snapshot tar into assets if available
  local ASSET_ROOT="$RN_DIR/src/main/assets/pglite"
  mkdir -p "$ASSET_ROOT"
  local CANDIDATES=(
    "${PGLITE_DATADIR_TAR:-}"
    "$DIST_DIR/PGLiteDataDir.tar"
    "$DIST_BASE/PGLiteDataDir.tar"
    "$FLAKE_ROOT/packages/pglite/release/PGLiteDataDir.tar"
  )
  local FOUND_TAR=""
  for f in "${CANDIDATES[@]}"; do
    if [ -n "$f" ] && [ -f "$f" ]; then FOUND_TAR="$f"; break; fi
  done
  if [ -n "$FOUND_TAR" ]; then
    cp -f "$FOUND_TAR" "$ASSET_ROOT/PGLiteDataDir.tar"
    echo "Copied base cluster snapshot to $ASSET_ROOT/PGLiteDataDir.tar (from $FOUND_TAR)"
  else
    echo "Note: base cluster snapshot (PGLiteDataDir.tar) not found; skipping copy. Set PGLITE_DATADIR_TAR to override."
  fi
}


main() {
  case "$PLATFORM" in
    android)
      setup_android
      ;;
    ios)
      setup_ios
      ;;
    *)
      echo "Unknown PLATFORM '$PLATFORM' (expected android|ios)" >&2
      exit 2
      ;;
  esac

  echo "\n=== Building PGLite mobile core: PLATFORM=$PLATFORM ABI=$ABI ARCH=$ARCH ===\n"
  echo "Using PGSRC=$PGSRC"

  build_pg
  build_glue_and_merge
  if [ "$PLATFORM" = "android" ]; then
    copy_into_rn_android
  fi

  echo "\nArtifacts at: $DIST_DIR\n  - include/*\n  - lib/libpgcore_mobile.a\n  - lib/libpglite_glue_mobile.a\n  - runtime/share/postgresql (if present)\nAlso installed into RN module: packages/pglite-react-native/android/src/main/{jni,assets}\n"
}

main "$@"

