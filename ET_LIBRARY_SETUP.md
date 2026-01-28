# ET Library Setup for E2SAR Receiver

## Overview

The ET (Event Transfer) library is required for the frame builder functionality. If ET is not found during the build, the receiver will compile without frame builder support (basic receiver only).

## Build System Detection

The build system searches for ET in this order:

1. **pkg-config** - Looks for `et.pc` file
2. **Manual search** - Uses `compiler.find_library('et')` which checks:
   - Standard system library paths (`/usr/lib`, `/usr/lib64`, `/usr/local/lib`, etc.)
   - Paths in `LIBRARY_PATH` environment variable
3. **Header check** - Verifies `et.h` is available via:
   - Standard include paths
   - Paths in `CPATH` environment variable

## Environment Variables

If ET is installed in a non-standard location, set these environment variables **before** running meson:

```bash
# Example: ET installed in /opt/et
export CPATH=/opt/et/include:$CPATH
export LIBRARY_PATH=/opt/et/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=/opt/et/lib:$LD_LIBRARY_PATH
```

Or for a custom location:

```bash
export ET_PREFIX=/path/to/et/installation
export CPATH=$ET_PREFIX/include:$CPATH
export LIBRARY_PATH=$ET_PREFIX/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=$ET_PREFIX/lib:$LD_LIBRARY_PATH
```

## Verification

### Check ET is Available

Before building, verify ET can be found:

```bash
# Check if library is in LIBRARY_PATH
echo $LIBRARY_PATH | tr ':' '\n'

# Try to find the library
find /usr/lib /usr/lib64 /usr/local/lib /opt -name "libet.so*" 2>/dev/null

# Check if header is in CPATH
echo $CPATH | tr ':' '\n'

# Try to find the header
find /usr/include /usr/local/include /opt -name "et.h" 2>/dev/null
```

### Build with Diagnostics

Configure the build and watch for ET detection messages:

```bash
cd /path/to/e2sar-receiver
rm -rf builddir
meson setup builddir
```

Look for these messages:
```
ET not found via pkg-config, trying manual search...
Found ET library
Found ET header et.h
```

Or if not found:
```
ET library not found via LIBRARY_PATH
Set LIBRARY_PATH to include ET library directory if needed
```

Check the build summary:
```
Dependencies
  ...
  ET Library             : Found (frame builder enabled)
```

Or:
```
  ET Library             : Not Found (frame builder disabled)
```

## Common Installation Locations

### Jefferson Lab Standard
```bash
export CODA=/usr/coda/3.10  # or appropriate version
export CPATH=$CODA/Linux-x86_64/include:$CPATH
export LIBRARY_PATH=$CODA/Linux-x86_64/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=$CODA/Linux-x86_64/lib:$LD_LIBRARY_PATH
```

### System Installation
```bash
# ET in /usr/local
export CPATH=/usr/local/include:$CPATH
export LIBRARY_PATH=/usr/local/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

### Custom Build
```bash
# ET built from source
export ET_HOME=/home/username/et-16.3
export CPATH=$ET_HOME/include:$CPATH
export LIBRARY_PATH=$ET_HOME/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=$ET_HOME/lib:$LD_LIBRARY_PATH
```

## Troubleshooting

### Issue: "ET Library: Not Found"

**Diagnosis:**
```bash
# Check if libet.so exists
find /usr -name "libet.so*" 2>/dev/null
find /opt -name "libet.so*" 2>/dev/null

# Check if et.h exists
find /usr -name "et.h" 2>/dev/null
find /opt -name "et.h" 2>/dev/null
```

**Solution:**
1. Locate your ET installation
2. Set environment variables pointing to it
3. Reconfigure: `rm -rf builddir && meson setup builddir`

### Issue: "Found ET library but et.h header not found"

**Cause:** Library found but header not in CPATH

**Solution:**
```bash
# Find where et.h is located
find /usr /opt -name "et.h" 2>/dev/null

# Add to CPATH
export CPATH=/path/to/et/include:$CPATH

# Reconfigure
rm -rf builddir
meson setup builddir
```

### Issue: "undefined reference to et_..." at link time

**Cause:** Found during configure but not at runtime

**Solution:**
```bash
# Ensure LD_LIBRARY_PATH is set
export LD_LIBRARY_PATH=/path/to/et/lib:$LD_LIBRARY_PATH

# Rebuild
rm -rf builddir
meson setup builddir
meson compile -C builddir
```

## Persistent Setup

### For Your User Account

Add to `~/.bashrc` or `~/.bash_profile`:

```bash
# ET Library Setup
export ET_PREFIX=/path/to/et
export CPATH=$ET_PREFIX/include:$CPATH
export LIBRARY_PATH=$ET_PREFIX/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=$ET_PREFIX/lib:$LD_LIBRARY_PATH
```

Then reload:
```bash
source ~/.bashrc
```

### For System-Wide

Create `/etc/profile.d/et.sh`:

```bash
#!/bin/bash
export ET_PREFIX=/usr/local/et
export CPATH=$ET_PREFIX/include:$CPATH
export LIBRARY_PATH=$ET_PREFIX/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=$ET_PREFIX/lib:$LD_LIBRARY_PATH
```

Make it executable:
```bash
sudo chmod +x /etc/profile.d/et.sh
```

### Using ldconfig (Alternative)

If ET is in a standard location, add to ldconfig:

```bash
# Create config file
echo "/path/to/et/lib" | sudo tee /etc/ld.so.conf.d/et.conf

# Update ldconfig
sudo ldconfig

# Verify
ldconfig -p | grep libet
```

## Building Without ET

If you don't need frame builder functionality, you can build without ET:

```bash
# Simply configure and build
meson setup builddir
meson compile -C builddir
```

The build will succeed with:
```
ET Library             : Not Found (frame builder disabled)
```

**Features available without ET:**
- ✅ UDP packet reception
- ✅ Frame reassembly
- ✅ File output (single file mode)
- ✅ Statistics reporting
- ❌ Frame builder (multi-stream aggregation)
- ❌ EVIO-6 output
- ❌ ET system integration
- ❌ 2GB auto-rollover file output

## Installing ET

If ET is not installed, you can obtain it from:

- **Jefferson Lab**: https://coda.jlab.org/drupal/
- **Source code**: Part of CODA distribution
- **Documentation**: https://coda.jlab.org/drupal/content/event-transfer

### Quick Install from Source

```bash
# Download ET (example version)
wget https://coda.jlab.org/downloads/et-16.3.tgz
tar xzf et-16.3.tgz
cd et-16.3

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Install
sudo make install

# Or install to custom location
cmake -DCMAKE_INSTALL_PREFIX=/opt/et ..
make -j$(nproc)
make install

# Set environment variables
export ET_PREFIX=/opt/et
export CPATH=$ET_PREFIX/include:$CPATH
export LIBRARY_PATH=$ET_PREFIX/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=$ET_PREFIX/lib:$LD_LIBRARY_PATH
```

## Verification After Build

### Check Compilation

```bash
# Verify frame builder was compiled
nm builddir/e2sar_receiver | grep FrameBuilder

# Should show symbols like:
# addTimeSlice
# FrameBuilder constructor
# etc.
```

### Test Frame Builder

```bash
# Try using frame builder
./builddir/e2sar_receiver --help | grep -A5 "frame"

# Should show frame builder options:
# --use-framebuilder
# --et-file
# --et-station
# etc.
```

### Runtime Check

```bash
# Verify ET library is found at runtime
ldd builddir/e2sar_receiver | grep libet

# Should show:
# libet.so => /path/to/libet.so (0x...)
```

## Summary Checklist

Before building with ET support:

- [ ] ET library (`libet.so`) is installed
- [ ] ET header (`et.h`) is installed
- [ ] `CPATH` includes ET header directory
- [ ] `LIBRARY_PATH` includes ET library directory
- [ ] `LD_LIBRARY_PATH` includes ET library directory
- [ ] Run `meson setup builddir` and check for "ET Library: Found"
- [ ] Build with `meson compile -C builddir`
- [ ] Verify with `ldd builddir/e2sar_receiver | grep libet`

If all checks pass, frame builder will be available!
