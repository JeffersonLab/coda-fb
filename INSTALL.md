# Installation Instructions - E2SAR Standalone Receiver

## Quick Start

```bash
cd /Users/gurjyan/Documents/Devel/e2sar_receiver
./scripts/build.sh --install
```

## Prerequisites

### 1. E2SAR Library Installation

The E2SAR library must be installed on your system before building this package. 

**Verify E2SAR is installed:**
```bash
pkg-config --exists e2sar && echo "E2SAR found" || echo "E2SAR not found"
pkg-config --modversion e2sar  # Should show version like "0.2.2"
```

**If E2SAR is not found, install it first:**
```bash
# From E2SAR source directory
cd /Users/gurjyan/Documents/Devel/E2SAR
meson setup builddir
meson compile -C builddir
sudo meson install -C builddir
```

### 2. System Dependencies

**macOS:**
```bash
brew install boost grpc protobuf glib pkg-config meson ninja
```

**Ubuntu/Debian:**
```bash
sudo apt install libboost-all-dev libgrpc++-dev libprotobuf-dev libglib2.0-dev
sudo apt install pkg-config meson ninja-build build-essential
```

**RHEL/CentOS/Fedora:**
```bash
sudo dnf install boost-devel grpc-devel protobuf-devel glib2-devel
sudo dnf install pkg-config meson ninja-build gcc-c++
```

## Build Methods

### Method 1: Using Build Script (Recommended)

```bash
cd /Users/gurjyan/Documents/Devel/e2sar_receiver

# Basic build
./scripts/build.sh

# Debug build with clean
./scripts/build.sh --type debug --clean

# Build and install to custom location
./scripts/build.sh --install --prefix /opt/e2sar_receiver
```

### Method 2: Manual Meson Commands

```bash
cd /Users/gurjyan/Documents/Devel/e2sar_receiver

# Setup build directory
meson setup builddir

# Compile
meson compile -C builddir

# Install (optional)
meson install -C builddir
```

### Method 3: Custom Configuration

```bash
cd /Users/gurjyan/Documents/Devel/e2sar_receiver

# Configure with custom options
meson setup builddir \
  --buildtype=release \
  --prefix=/usr/local \
  -Dnuma_support=enabled \
  -Daffinity_support=enabled

meson compile -C builddir
meson install -C builddir
```

## Verification

### 1. Test Build

```bash
# Check executable exists and works
ls -la builddir/e2sar_receiver
./builddir/e2sar_receiver --help
```

### 2. Test Installation

```bash
# If installed to default location
e2sar_receiver --help

# If installed to custom location
/opt/e2sar_receiver/bin/e2sar_receiver --help
```

### 3. Run Example

```bash
# Create test directory
mkdir -p /tmp/test_frames

# Run example (modify configuration in script first)
./scripts/example_run.sh
```

## Troubleshooting

### E2SAR Library Not Found

**Error:** `dependency 'e2sar' not found`

**Solutions:**
1. Install E2SAR library system-wide
2. Set PKG_CONFIG_PATH to include E2SAR's .pc file:
   ```bash
   export PKG_CONFIG_PATH=/path/to/e2sar/lib/pkgconfig:$PKG_CONFIG_PATH
   ```

### Boost Library Issues

**Error:** `dependency 'boost' not found`

**Solutions:**
1. Install boost development packages
2. Set BOOST_ROOT environment variable:
   ```bash
   export BOOST_ROOT=/path/to/boost
   ```

### gRPC Version Mismatch

**Error:** `dependency 'grpc++' version '>=1.51.1' not found`

**Solutions:**
1. Update gRPC to a compatible version
2. Check installed version: `pkg-config --modversion grpc++`
3. Install gRPC from source if package version is too old

### Compiler Issues

**Error:** `C++ compiler does not support C++17`

**Solutions:**
1. Update compiler: GCC 8+, Clang 6+, or newer
2. Set specific compiler:
   ```bash
   export CXX=g++-9  # or clang++-9, etc.
   ```

### Permission Issues During Install

**Error:** `Permission denied` during `meson install`

**Solutions:**
1. Use sudo: `sudo meson install -C builddir`
2. Install to user directory:
   ```bash
   meson setup --prefix=$HOME/.local builddir
   meson install -C builddir
   ```

## Advanced Configuration

### NUMA Support

To enable/disable NUMA support:
```bash
meson setup -Dnuma_support=enabled builddir    # Force enable
meson setup -Dnuma_support=disabled builddir   # Force disable
meson setup -Dnuma_support=auto builddir       # Auto-detect (default)
```

### CPU Affinity Support

To control CPU affinity features:
```bash
meson setup -Daffinity_support=enabled builddir   # Force enable
meson setup -Daffinity_support=disabled builddir  # Force disable
meson setup -Daffinity_support=auto builddir      # Auto-detect (default)
```

### Cross-Compilation

For cross-compilation, create a cross-file and use:
```bash
meson setup --cross-file=cross_file.txt builddir
```

## Package Information

- **Package Name:** e2sar_receiver  
- **Version:** 1.0.0
- **Dependencies:** E2SAR library, Boost ≥1.83.0, gRPC++ ≥1.51.1, Protocol Buffers, GLib 2.0
- **Build System:** Meson ≥0.56
- **Language:** C++17
- **License:** MIT (see LICENSE file)

## Support

For build or installation issues:

1. **Check Prerequisites:** Ensure all dependencies are installed
2. **Environment Variables:** Verify PKG_CONFIG_PATH, BOOST_ROOT are set correctly  
3. **Clean Build:** Try `./scripts/build.sh --clean`
4. **Verbose Output:** Use `meson setup -v builddir` for detailed configuration info