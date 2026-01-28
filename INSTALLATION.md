# E2SAR Receiver Installation Guide

## Installation Directory

The e2sar_receiver build system automatically determines the best installation location based on your environment.

### Installation Directory Priority

The build system uses this priority order to determine where to install:

1. **Custom Prefix** (highest priority)
2. **CODA Directory** (if `$CODA` is set)
3. **User Home Directory** (default fallback)

### 1. Custom Installation Directory

You can explicitly specify where to install by providing a custom prefix:

```bash
meson setup builddir --prefix=/custom/path
meson install -C builddir
# Installs to: /custom/path/bin/e2sar_receiver
```

**Use cases:**
- System-wide installation to `/usr/local`
- Project-specific installation to `/opt/e2sar`
- Development installation to `/tmp/test`

### 2. Automatic CODA Detection

If the `CODA` environment variable is set and no custom prefix is specified, the receiver automatically installs to the CODA directory:

```
$CODA/Linux-x86_64/bin/e2sar_receiver
```

**Example:**
```bash
export CODA=/usr/coda/3.10
meson setup builddir
meson install -C builddir
# Installs to: /usr/coda/3.10/Linux-x86_64/bin/e2sar_receiver
```

**Benefits:**
- Seamless integration with Jefferson Lab CODA installations
- Follows CODA directory structure conventions
- Automatically available to CODA tools

### 3. Default User Installation (No CODA, No Prefix)

If `CODA` is not set and no custom prefix is specified, the receiver installs to your home directory:

```
~/.local/bin/e2sar_receiver
```

**Example:**
```bash
# No CODA environment variable set
meson setup builddir
meson install -C builddir
# Installs to: ~/.local/bin/e2sar_receiver
```

**Benefits:**
- No root/sudo access required
- User-specific installation
- Follows XDG Base Directory specification
- Safe for development and testing

## Installation Methods

### Method 1: Using Build Script (Recommended)

The build script handles everything automatically:

```bash
# Build and install (uses CODA if set, otherwise ~/.local)
./scripts/build.sh --install

# Build and install to custom location
./scripts/build.sh --install --prefix /opt
```

### Method 2: Manual Meson Commands

```bash
# Setup build directory
meson setup builddir

# Compile
meson compile -C builddir

# Install (no sudo needed for ~/.local or $CODA)
meson install -C builddir
```

**Note:** sudo is only required if you specify a system directory like `--prefix=/usr/local`

### Method 3: Clean Build and Install

```bash
# Clean previous build
rm -rf builddir

# Reconfigure and install
meson setup builddir
meson compile -C builddir
meson install -C builddir
```

## Verifying Installation

After installation, verify the executable is accessible:

### With CODA
```bash
which e2sar_receiver
# Expected: $CODA/Linux-x86_64/bin/e2sar_receiver

e2sar_receiver --help
```

### Without CODA (User Installation)
```bash
which e2sar_receiver
# Expected: ~/.local/bin/e2sar_receiver

e2sar_receiver --help
```

**Note:** If `which` doesn't find the executable, you may need to add the installation directory to your PATH (see below).

## Installation Summary

During the build setup, you'll see a message indicating where the executable will be installed:

```
Configuration
  E2SAR Receiver Version : 1.0.0
  Build Type             : release
  Install Directory      : /usr/coda/3.10/Linux-x86_64/bin
  ...
```

## Permissions

### No Permissions Required (Default)

The default installation directories do NOT require root access:
- `~/.local/bin` - User home directory (no sudo needed)
- `$CODA/Linux-x86_64/bin` - Typically user-writable (no sudo needed)

```bash
# Standard installation (no sudo)
meson setup builddir
meson install -C builddir
```

### System Directories (Requires Root)

Only if you explicitly specify a system directory (like `/usr/local`) will you need sudo:

```bash
# System-wide installation (requires sudo)
meson setup builddir --prefix=/usr/local
sudo meson install -C builddir
```

## PATH Configuration

### User Installation (~/.local/bin)

Most modern Linux distributions automatically include `~/.local/bin` in PATH. If not, add it:

```bash
# Add to ~/.bashrc or ~/.bash_profile
export PATH=$HOME/.local/bin:$PATH

# Reload configuration
source ~/.bashrc
```

### CODA Installation

If using CODA, ensure the bin directory is in your PATH:

```bash
# Add to ~/.bashrc or ~/.bash_profile
export CODA=/usr/coda/3.10
export PATH=$CODA/Linux-x86_64/bin:$PATH

# Reload configuration
source ~/.bashrc
```

### Custom Installation

For custom installation directories:

```bash
# Add to ~/.bashrc or ~/.bash_profile
export PATH=/custom/path/bin:$PATH

# Reload configuration
source ~/.bashrc
```

## Uninstallation

To uninstall the receiver:

```bash
# From the build directory
cd /path/to/e2sar-receiver
ninja -C builddir uninstall

# Or manually remove based on installation location
# CODA installation:
rm -f $CODA/Linux-x86_64/bin/e2sar_receiver

# User installation:
rm -f ~/.local/bin/e2sar_receiver

# System installation:
sudo rm -f /usr/local/bin/e2sar_receiver
```

## Troubleshooting

### "Permission denied" during installation

**Cause:** Trying to install to a system directory without sudo

**Solution:** The default installation to `~/.local/bin` doesn't require sudo. Only system directories need elevated privileges:

```bash
# Default installation (no sudo needed)
meson setup builddir
meson install -C builddir

# System installation (requires sudo)
meson setup builddir --prefix=/usr/local
sudo meson install -C builddir
```

### "CODA environment variable detected" but installs to wrong location

**Issue:** You specified a custom prefix that overrides CODA detection

**Solution:** Don't specify `--prefix` if you want automatic CODA detection

```bash
# Wrong (overrides CODA)
meson setup builddir --prefix=/usr/local

# Correct (uses CODA)
meson setup builddir
```

### Executable not in PATH after installation

**Solution:** Add installation directory to PATH based on where it was installed

```bash
# For ~/.local/bin (most systems include this by default)
export PATH=$HOME/.local/bin:$PATH

# For CODA
export PATH=$CODA/Linux-x86_64/bin:$PATH

# For custom location
export PATH=/custom/path/bin:$PATH

# Make permanent by adding to ~/.bashrc
echo 'export PATH=$HOME/.local/bin:$PATH' >> ~/.bashrc
```

## Complete Examples

### Example 1: User Installation (No CODA)

```bash
# Build and install to ~/.local/bin
cd /path/to/e2sar-receiver
./scripts/build.sh --clean --install

# Verify
which e2sar_receiver
# Output: ~/.local/bin/e2sar_receiver

# If not in PATH, add it
echo 'export PATH=$HOME/.local/bin:$PATH' >> ~/.bashrc
source ~/.bashrc

e2sar_receiver --help
```

### Example 2: CODA Installation

```bash
# Setup environment
export CODA=/usr/coda/3.10
export PATH=$CODA/Linux-x86_64/bin:$PATH

# Build and install
cd /path/to/e2sar-receiver
./scripts/build.sh --clean --install

# Verify
which e2sar_receiver
# Output: /usr/coda/3.10/Linux-x86_64/bin/e2sar_receiver

e2sar_receiver --help
```

### Example 3: System-Wide Installation

```bash
# Build and install to /usr/local
cd /path/to/e2sar-receiver
./scripts/build.sh --clean --install --prefix=/usr/local

# Verify (requires sudo for installation)
which e2sar_receiver
# Output: /usr/local/bin/e2sar_receiver

e2sar_receiver --help
```

## Build System Details

The installation directory is determined in [meson.build](meson.build) using this priority logic:

1. **Custom prefix** (highest priority): If user specifies `--prefix=/path`
   - Install to `/path/bin`
2. **CODA environment**: If `$CODA` is set and no custom prefix
   - Install to `$CODA/Linux-x86_64/bin`
3. **User directory** (default): Otherwise
   - Install to `~/.local/bin`

This ensures:
- Seamless integration with CODA installations
- No root access required for default builds
- Flexibility for custom installations
- Follows XDG Base Directory specification
