# E2SAR Receiver Compilation Fixes Summary

## Overview

This document summarizes all the fixes applied to make the e2sar_receiver compile successfully on RHEL9 with E2SAR library v0.1.5.

## Issues Fixed

### 1. E2SAR API Changes (v0.1.5)

**Files Modified:** `src/e2sar_receiver.cpp`

#### Issue 1.1: getStats() Return Type Change
- **Error:** `has no member named 'reassemblyLoss'`
- **Root Cause:** `getStats()` now returns `boost::tuples::tuple` instead of a struct
- **Fix:** Changed from `stats.fieldName` to `stats.get<N>()` syntax
- **Details:** See [E2SAR_API_CHANGES.md](E2SAR_API_CHANGES.md)

#### Issue 1.2: Reassembler Constructor Signature Change
- **Error:** `no matching function for call to 'e2sar::Reassembler::Reassembler'`
- **Root Cause:** Constructor now requires explicit `boost::asio::ip::address` parameter
- **Fix:** Extract IP from URI using `get_dataAddrv4()` or `get_dataAddrv6()` and pass `.first` element
- **Code:**
  ```cpp
  auto data_addr = uri.get_dataAddrv4();
  data_ip = data_addr.value().first;  // Extract IP from pair
  reasPtr = new Reassembler(uri, data_ip, recvStartPort, numThreads, rflags);
  ```

#### Issue 1.3: get_dataIP() and get_recvPorts() Removed
- **Error:** `has no member named 'get_dataIP'`
- **Root Cause:** Methods no longer available in new API
- **Fix:** Store IP and calculate ports manually from local variables

#### Issue 1.4: get_FDStats() Removed
- **Error:** `has no member named 'get_FDStats'`
- **Root Cause:** Per-port statistics no longer available
- **Fix:** Removed per-port statistics code

### 2. Protobuf Library Conflict

**Files Modified:** `meson.build`

- **Error:** `libprotobuf.so.32 may conflict with libprotobuf.so.25`
- **Root Cause:** System has two protobuf versions; E2SAR built with v32, build was finding v25
- **Fix:** Explicitly search for protobuf in `/usr/local` first
- **Code:**
  ```meson
  protobuf_lib = compiler.find_library('protobuf',
      dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'],
      required: true)
  ```
- **Details:** See [BUILD_FIXES.md](BUILD_FIXES.md)

### 3. Abseil Library Conflict

**Files Modified:** `meson.build`

- **Error:** `undefined reference to symbol '_ZN4absl12lts_202301255MutexD1Ev'`
- **Root Cause:** E2SAR built with Abseil from `/usr/local`, build was mixing versions
- **Fix:** Explicitly find and link Abseil libraries from `/usr/local`
- **Code:**
  ```meson
  absl_sync_lib = compiler.find_library('absl_synchronization',
      dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'],
      required: true)
  absl_time_lib = compiler.find_library('absl_time',
      dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'],
      required: true)

  protobuf_dep = declare_dependency(
      include_directories: include_directories('/usr/local/include'),
      dependencies: [protobuf_lib, absl_sync_lib, absl_time_lib]
  )
  ```
- **Details:** See [BUILD_FIXES.md](BUILD_FIXES.md)

### 4. Optional ET Library Support

**Files Modified:** `src/e2sar_receiver.cpp`, `meson.build`

- **Error:** `undefined reference to 'e2sar::FrameBuilder::...'`
- **Root Cause:** FrameBuilder code compiled conditionally (only with ET), but used unconditionally
- **Fix:**
  - Wrapped FrameBuilder usage in `#ifdef ENABLE_FRAME_BUILDER` guards
  - Improved ET library detection in meson.build
- **Changes:**
  - Added forward declaration when ET not available
  - Wrapped frame builder operations in conditional compilation
  - Added error message when frame builder requested but not compiled
  - Enhanced ET detection to search via LIBRARY_PATH/CPATH environment variables
  - Added diagnostic messages during ET detection

### 5. FrameBuilder Class Linking Issues

**Files Modified:** `src/e2sar_reassembler_framebuilder.hpp`, `src/e2sar_reassembler_framebuilder.cpp`

- **Error:** `undefined reference to 'e2sar::FrameBuilder::addTimeSlice'` and other methods
- **Root Cause:** FrameBuilder class was defined entirely inline within the .cpp file (duplicate definition)
- **Fix:**
  - Updated header to include missing member variables
  - Refactored .cpp to use proper method implementations instead of inline class definition
  - Added `#include "e2sar_reassembler_framebuilder.hpp"` at top of .cpp
  - Converted all methods to `FrameBuilder::methodName()` format
- **Changes:**
  - Header: Added `enableET`, `enableFileOutput`, `fileOutputDir`, `fileOutputPrefix`, `filesCreated`, `bytesWritten`
  - Implementation: Removed inline class definition, implemented methods separately

### 6. Installation Directory Configuration

**Files Modified:** `meson.build`

- **Change:** Updated default installation directory to use CODA environment variable or user home directory
- **Behavior (priority order):**
  1. If user specifies custom `--prefix`: installs to `<prefix>/bin`
  2. If `$CODA` is set: installs to `$CODA/Linux-x86_64/bin`
  3. Otherwise: installs to `~/.local/bin`
- **Benefits:**
  - Automatically integrates with Jefferson Lab CODA installation structure
  - No root access required for default installation
  - Follows XDG Base Directory specification

## Files Changed

1. **src/e2sar_receiver.cpp** - E2SAR API compatibility and ET conditional compilation
2. **src/e2sar_reassembler_framebuilder.hpp** - Added missing member variables for proper class declaration
3. **src/e2sar_reassembler_framebuilder.cpp** - Refactored from inline class to proper method implementations
4. **meson.build** - Library dependency resolution, ET detection, and CODA installation directory
5. **E2SAR_API_CHANGES.md** (NEW) - Complete E2SAR API migration guide
6. **BUILD_FIXES.md** (NEW) - Build system fixes documentation
7. **ET_LIBRARY_SETUP.md** (NEW) - ET library setup and troubleshooting guide
8. **INSTALLATION.md** (NEW) - Installation directory configuration and guide
9. **COMPILATION_FIXES_SUMMARY.md** (NEW) - This file

## Build Instructions

After all fixes, rebuild with:

```bash
cd /path/to/e2sar-receiver
rm -rf builddir          # Clean old build
meson setup builddir     # Reconfigure
meson compile -C builddir
```

Or use the build script:
```bash
./scripts/build.sh --clean --install
```

### Installation Directory

The build system automatically detects the installation directory (in priority order):

1. **Custom prefix**: If you specify `--prefix=/custom/path`, installs to `/custom/path/bin`
2. **With CODA**: If `$CODA` environment variable is set, installs to `$CODA/Linux-x86_64/bin`
3. **Default**: Installs to `~/.local/bin` (no root access required)

Examples:
```bash
# Install to CODA (if $CODA is set)
meson setup builddir
meson install -C builddir

# Install to custom location
meson setup builddir --prefix=/opt/e2sar
meson install -C builddir

# Install to user directory (when CODA not set)
meson setup builddir
meson install -C builddir  # Installs to ~/.local/bin
```

## Build Configurations

### With ET Library (Full Features)
```bash
# Requires ET library installed
meson setup builddir
meson compile -C builddir
```
**Features:** Full receiver + frame builder support

### Without ET Library (Basic Receiver)
```bash
# ET library not required
meson setup builddir
meson compile -C builddir
```
**Features:** Basic receiver only (no frame builder)

The build system auto-detects ET availability and enables/disables frame builder accordingly.

## Verification

### Check Build Configuration
Look for this in the build output:
```
Dependencies
  E2SAR Library          : Found
  Boost                  : 1.85.0
  gRPC++                 : 1.54.1
  Protobuf               : Found (/usr/local)
  ET Library             : Found (frame builder enabled)
                           OR
                           Not Found (frame builder disabled)
```

### Test Basic Receiver
```bash
./builddir/e2sar_receiver \
  --uri 'ejfat://token@host:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --output-dir /tmp/test
```

### Test Frame Builder (if ET available)
```bash
./builddir/e2sar_receiver \
  --uri 'ejfat://token@host:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --use-framebuilder \
  --fb-output-dir /tmp/test
```

## Platform Notes

### RHEL9 Specific
- System protobuf: 3.14 (libprotobuf.so.25)
- System abseil: Various versions in `/usr/lib64`
- E2SAR requires: protobuf 3.21+ and abseil 2301+
- Solution: Use `/usr/local` versions explicitly

### Other Platforms
Similar fixes may be needed on:
- Ubuntu 20.04/22.04 with older system packages
- CentOS 7/8
- Debian 10/11

**Key principle:** Match library versions used to build E2SAR

## Common Errors

### "Frame builder requested but not available"
- **Cause:** `--use-framebuilder` specified but ET library not found during build
- **Solution:** Install ET library and rebuild, or remove `--use-framebuilder` flag

### "libprotobuf.so.32 may conflict with libprotobuf.so.25"
- **Cause:** Build finding wrong protobuf version
- **Solution:** Already fixed in meson.build; do clean rebuild

### "undefined reference to absl::..."
- **Cause:** Missing or wrong Abseil library version
- **Solution:** Already fixed in meson.build; do clean rebuild

## Success Criteria

Build succeeds when:
1. ✅ Compilation completes without errors
2. ✅ Linker finds all symbols
3. ✅ Executable runs and shows help: `./builddir/e2sar_receiver --help`
4. ✅ Can connect and receive data

## Support

For issues:
1. Check relevant documentation files (E2SAR_API_CHANGES.md, BUILD_FIXES.md)
2. Verify library versions match E2SAR build
3. Try clean rebuild: `rm -rf builddir && meson setup builddir && meson compile -C builddir`
4. Check E2SAR library version: built against v0.1.5
