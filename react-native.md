# PGLite React Native Module Implementation

## Overview

This document describes the architecture and implementation of PGLite for React Native, which provides a native PostgreSQL database engine for iOS and Android applications. The implementation maintains exact API compatibility with the web/WASM version of PGLite while using native compilation for better performance and platform integration.

## Architecture

### Core Principles

1. **API Parity**: Maintain exactly the same JavaScript/TypeScript API as the web/WASM version
2. **Native Performance**: Compile PostgreSQL natively for ARM64 (iOS/Android) and x86_64 (Android)
3. **Single-User Mode**: Run PostgreSQL in single-user mode without client-server architecture
4. **Wire Protocol**: Use PostgreSQL's wire protocol for all communication between JavaScript and the native backend
5. **Minimal Native Bridge**: Keep the native bridge thin - only handle byte buffer marshalling and filesystem operations

### Major Components

- **PostgreSQL Core**: Native compilation of PostgreSQL 17 for mobile platforms
- **PGLite Glue**: Adaptation layer (pg_main.c, interactive_one.c, pgl_mains.c) that provides single-user mode operation
- **Nitro Module (autolinked)**: React Native bridge using Nitro Modules (JSI) with autolinking; no manually written/native bridges are required
- **TypeScript Adapter**: JavaScript layer that implements the PGLite API on top of the wire protocol

## Execution Model

### Startup Phase

1. **Environment Setup**

   - Set PGDATA to app sandbox directory (e.g., `/data/user/0/com.app/files/pglite/pgdata`)
   - Set PGSYSCONFDIR to runtime resources directory
   - Extract bundled PostgreSQL runtime files (share/postgresql/\*) if first run

2. **Database Initialization**

   - Call `pgl_initdb()` to check if database exists or needs creation
   - If no database exists, run initdb to create the initial database cluster
   - Execute bootstrap SQL to create system catalogs (via `bootstrap_template1()`)
   - Run single-user replay of initialization scripts

3. **Backend Setup**
   - Call `pgl_backend()` once to initialize the backend
   - Set up critical memory contexts (MessageContext, row_description_context)
   - Install mobile communication methods via `pgl_install_mobile_comm()`
   - Transition to normal backend operation mode

### Query Processing

1. **Request Flow**

   - JavaScript calls query/exec/transaction methods
   - TypeScript adapter converts to PostgreSQL wire protocol messages
   - Messages passed to native via `execProtocolRaw()`
   - Native writes message to CMA (Contiguous Memory Area) buffer
   - Calls `interactive_one()` to process the message

2. **Backend Processing**

   - `interactive_one()` reads from the CMA buffer
   - Processes via the backend wire protocol machinery (libpq/PQcomm)
   - Executes SQL via `exec_simple_query()` or extended protocol
   - Results written back to the CMA buffer via mobile PQcomm methods

3. **Response Flow**
   - Native reads response from CMA buffer
   - Returns raw bytes to JavaScript
   - TypeScript adapter parses protocol messages
   - Converts to PGLite Results format

### Communication Pattern

The mobile implementation uses a CMA buffer for efficient communication:

```
JavaScript -> Wire Protocol -> CMA Buffer -> PostgreSQL Backend
                                   ^              |
                                   |              v
                                   +-- Response --+
```

- Request written to buffer offset 1
- Response written to buffer offset (request_size + 2)

### Memory Management

- Single shared buffer for request/response via CMA (size controlled by build-time CMA_MB macro; current default is 12MB)
- Conservative PostgreSQL memory settings via single-user flags (-B 16, -S 512)
- Memory contexts properly initialized and persisted across calls
- No memory leaks between successive queries

## Status

### What's Working

- ✅ Database initialization (pgl_initdb) completes successfully
- ✅ Backend startup (pgl_backend) initializes properly
- ✅ Memory contexts and mobile communication methods are installed
- ✅ CMA buffer communication is set up and is the only path on mobile
- ✅ Nitro Modules autolinking loads the native module; execProtocolRaw round-trips

### Historical issue (resolved)

Earlier, PostgreSQL and React Native accessed different buffer instances (function-based access to `get_buffer_addr()` introduced duplicate static buffers). This caused invalid message errors.

Resolution: Use external variables set by the mobile glue and consumed by PostgreSQL (e.g., `pgl_mobile_cma_buffer_addr`, `pgl_mobile_cma_buffer_size`). pqcomm.c (PGL_MOBILE) reads these in `pq_startmsgread()`, matching the WASM pattern of direct pointer usage.

Notes:

- `interactive_one.c` is the primary loop. Mobile uses its PGL_MOBILE CMA path exclusively; web/WASM continues to use the non-mobile paths.
- Verbose logging is intentionally kept to aid diagnostics.

## Data Flow

### Initialization Data Flow

```
Application Start
    |
    v
RuntimeResources::ensureResourcesExtracted()
    |
    v
Set PGDATA/PGSYSCONFDIR environment variables
    |
    v
pgl_initdb() - Check/create database
    |
    v
pgl_backend() - Initialize backend
    |
    v
Ready for queries
```

### Query Execution Data Flow

```
JavaScript query()
    |
    v
Build wire protocol message
    |
    v
execProtocolRaw() [Native]
    |
    v
Write to CMA buffer
    |
    v
interactive_one()
    |
    v
PostgreSQL processes query
    |
    v
Write response to CMA
    |
    v
Read response buffer
    |
    v
Parse protocol messages [JavaScript]
    |
    v
Return Results object
```

## Major Functions

### Native Functions (C/C++)

- `pgl_initdb()` - Initialize database cluster if needed
- `pgl_backend()` - Start PostgreSQL backend in single-user mode
- `interactive_one()` - Process one wire protocol message
- `pgl_install_mobile_comm()` - Install mobile-specific communication methods
- `get_buffer_addr()/get_buffer_size()` - CMA buffer management
- `interactive_write()/interactive_read()` - Set message sizes in buffer

### JavaScript/TypeScript Functions

- `PGLite.create()` - Create and initialize a database instance
- `query()` - Execute SQL query with parameters
- `exec()` - Execute SQL statements
- `transaction()` - Run queries in a transaction
- `execProtocolRaw()` - Send raw wire protocol messages
- `listen()/unlisten()` - LISTEN/NOTIFY support

### Implementation Notes

**Why We Don't Extend BasePGlite**

The React Native adapter implements its own `query()` method instead of extending `BasePGlite` from `@electric-sql/pglite` due to web-specific dependencies that don't exist in React Native:

- `File` and `Blob` types in abstract method signatures (`_handleBlob`, `_getWrittenBlob`)
- Filesystem imports (`interface.ts` imports from `fs/base.js`)
- WASM-specific utilities and memory management
- Browser-specific APIs and polyfills

Additionally, we miss out on BasePGlite's sophisticated parameter serialization system:

- **OID-based type detection**: Uses PostgreSQL `describe` statements to determine parameter types
- **Comprehensive type serializers**: Proper handling of arrays, JSON, bytea, dates, etc.
- **PostgreSQL-specific formats**: Boolean serialization as 't'/'f', array escaping, etc.
- **Type validation and error handling**: Catches invalid inputs before sending to PostgreSQL

Our current React Native implementation uses a simplified serialization approach that handles basic types but may not cover all edge cases that the web version handles.

In the future, we may separate the base class and serialization logic into platform-agnostic packages without web-specific dependencies to reduce code duplication and ensure consistent type handling across implementations.

## Build & Initialization

The build uses a two-stage approach on mobile:

1. **PostgreSQL Compilation** (mobile-build/build-mobile.sh)

   - Cross-compile PostgreSQL using Autotools for each platform/ABI
   - Builds static libraries: `libpgcore_mobile.a`, `libpglite_glue_mobile.a`
   - Copies artifacts into the RN module under platform-specific locations

2. **Module Linking**
   - RN uses Nitro Modules with autolinking; the native module is discovered/linked automatically

### Android initialization

- On app process start, a ContentProvider (InitProvider) ensures the native library is loaded and environment variables are applied via NativeEnv
- Runtime resources (share/postgresql/\*) are copied into app storage if needed
- Environment variables set include (examples):
  - PGDATA: `<app files>/pglite/pgdata`
  - PGSYSCONFDIR: `<app files>/pglite/runtime`
  - PGROOT/PREFIX: runtime root
- On first run, `pgl_initdb()` initializes the cluster, then `pgl_backend()` prepares the backend

### iOS initialization

- On app launch, Swift (NativeEnv) sets environment variables and prepares Application Support paths:
  - PGDATA: `<App Support>/PGLite/pgdata`
  - PGSYSCONFDIR + PGROOT + PREFIX: `<App Support>/PGLite/runtime`
- Runtime resources are bundled in the Pod and copied on first run
- The Nitro module is autolinked; no manual bridge code is required
- Then `pgl_initdb()` and `pgl_backend()` run similarly to Android

## How to build and test

### Integrated Build Script (Recommended)

The `rebuild-mobile.sh` script in `packages/pglite-react-native/example/` provides an integrated workflow that automates the entire React Native mobile development process:

**Purpose**: This script orchestrates the complete build chain from native PostgreSQL compilation to app deployment, eliminating the need to run multiple commands manually.

**Functionality**:

- Builds native PostgreSQL static libraries for the target platform
- Compiles the React Native module TypeScript code
- Generates platform-specific native projects (Android/iOS)
- Builds and deploys the app to devices/simulators
- Provides comprehensive error checking and progress reporting

**Usage Examples**:

```bash
cd packages/pglite-react-native/example

# Build Android with defaults (arm64-v8a, API 27)
./rebuild-mobile.sh

# Build iOS simulator for Apple Silicon
./rebuild-mobile.sh -p ios

# Build iOS for physical device
./rebuild-mobile.sh -p ios --arch arm64

# Skip native build, just rebuild app
./rebuild-mobile.sh --skip-build

# Build specific Android ABI
./rebuild-mobile.sh -p android -a arm64-v8a
```

**Configuration Options**:

- `--platform`: Target platform (android, ios)
- `--abi`: Android ABI (arm64-v8a, armeabi-v7a)
- `--arch`: iOS architecture (arm64-sim, arm64, x86_64)
- `--pg-branch`: PostgreSQL branch to build
- `--skip-build`: Skip native library compilation
- `--skip-install`: Skip npm/pnpm install steps

### Manual Build Process

For advanced users or debugging, you can run the individual steps manually:

- in `postgres-pglite` run `PLATFORM=android ABI=arm64-v8a PG_BRANCH=REL_17_5_WASM ./mobile-build/build-mobile.sh` to build pglite static libs for android. You need nix installed.
- build-mobile.sh copies static libs into pglite-react-native project
- in `packages/pglite-react-native` do `pnpm install`
- in `packages/pglite-react-native/example`
  - `npm install` to install deps (first run only)
  - `npx expo run:android` to start android sim (first run only)
  - `rm -rf android` to clean generated android project (if needed)
  - `npx expo prebuild -p android --clean` to generate native android project
  - `cd android`
  - `./gradlew assembleDebug` to build the apk
  - `adb install app/build/outputs/apk/debug/app-debug.apk` to install on device
  - `cd ..`
  - `npx expo start -c` to start the react native dev server
  - Use `adb logcat` to view android system logs for more detail.
  - Open app on the emulator to test.
  - To view more logs
    - `adb root` to get root adb
    - `adb shell` to open a shell on device
    - `run-as com.evelant.example` to run as app user
    - `cat files/pglite/runtime/initdb.stderr.log` to see backend logs
    - You may need to `rm -rf files/pglite/pgdata` to clear data for another run after a crash

## Production Readiness Status

**⚠️ NOT PRODUCTION READY** - This React Native implementation is currently in active development and requires significant work before it can be considered production-ready.

### Current Status (August 2025)

**✅ What's Working:**

- Basic CRUD operations (CREATE TABLE, INSERT, SELECT, UPDATE, DELETE)
- Rudimentary serialization with correct PostgreSQL OIDs
- PostgreSQL extended wire protocol communication via CMA buffers
- Android/ios build compilation and basic query execution
- Memory context initialization for mobile environment

### Remaining Work

- ~~**iOS Implementation** - Port Android work to iOS with proper build system integration~~
- **Extract Base Package** - Create shared `@electric-sql/pglite-base` without web dependencies so serialization and other logic can be shared with react native
- **Replace Custom Serialization** - Use extracted base serialization methods for consistency
- **Review PGL_MOBILE vs **EMSCRIPTEN\*\*\*\* - Find places where mobile is missing conditional compilation it might need
- **Cleanup** - Remove debug logging, temporary code
- **Add Extensions Support** - Load and use PostgreSQL extensions

### TODOS

- pgl_install_mobile_comm is getting run on every request, should only be necessary on the first one?
-
