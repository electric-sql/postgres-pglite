# PGLite Mobile Cross-Compile (Android/iOS)

This folder contains scripts to cross-compile PostgreSQL and the PGLite glue for Android (NDK) and iOS (Xcode toolchains), producing prebuilt static libraries and headers consumed by the React Native module.

Artifacts layout (per plan):

- dist/mobile/android/<abi>/{include,lib}
  - libpostgres_mobile.a
  - libpglite_glue_mobile.a
- dist/mobile/ios/{arm64, x86_64-sim}/{include,lib}
  - libpostgres_mobile.a
  - libpglite_glue_mobile.a

Notes:

- These scripts are initial scaffolding; you may need to adjust paths (NDK, SDK versions, PG branch) and add additional PG libs to the merge step based on link errors.
- For iOS, consider building an XCFramework for distribution via CocoaPods.

## Nix-based reproducible builds

We provide a Nix flake that sets up the toolchain and host tools (GNU make, android ndk, perl, coreutils, etc.) for reproducible builds.

Prereqs:

- Install Nix (https://nixos.org/download)
- Optional: direnv + use flake (we include .envrc)

Shells:

- Android:
  - nix develop .#android
  - or build-mobile.sh will enter the shell automatically
    - PLATFORM=android ABI=arm64-v8a PG_BRANCH=REL_17_5_WASM ./mobile-build/build-mobile.sh
- iOS (requires Xcode/CLT installed locally):
  - iOS not yet working/tested
  - nix develop .#ios
  - PLATFORM=ios ARCH=arm64 ./build-mobile.sh
