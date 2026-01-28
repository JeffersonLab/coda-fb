#!/bin/bash
#
# Build script for CODA Frame Builder
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default values
BUILD_TYPE="release"
CLEAN_BUILD=false
INSTALL=false
PREFIX="/usr/local"

# Function to print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to show usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Build CODA Frame Builder

OPTIONS:
    -h, --help          Show this help message
    -t, --type TYPE     Build type: debug, release, debugoptimized (default: release)
    -c, --clean         Clean build directory before building
    -i, --install       Install after building
    -p, --prefix PREFIX Installation prefix (default: /usr/local)

EXAMPLES:
    $0                              # Basic release build
    $0 --type debug --clean         # Clean debug build
    $0 --install --prefix /opt      # Build and install to /opt
EOF
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            exit 0
            ;;
        -t|--type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -i|--install)
            INSTALL=true
            shift
            ;;
        -p|--prefix)
            PREFIX="$2"
            shift 2
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Validate build type
case $BUILD_TYPE in
    debug|release|debugoptimized)
        ;;
    *)
        print_error "Invalid build type: $BUILD_TYPE"
        print_error "Valid types: debug, release, debugoptimized"
        exit 1
        ;;
esac

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

print_status "Building CODA Frame Builder"
print_status "Build type: $BUILD_TYPE"
print_status "Install: $INSTALL"
if [[ "$INSTALL" == "true" ]]; then
    print_status "Install prefix: $PREFIX"
fi

# Clean build directory if requested
if [[ "$CLEAN_BUILD" == "true" ]]; then
    print_status "Cleaning build directory..."
    rm -rf builddir
fi

# Check for required dependencies
print_status "Checking dependencies..."

# Check for meson
if ! command -v meson &> /dev/null; then
    print_error "Meson build system not found. Please install meson."
    exit 1
fi

# Check for ninja
if ! command -v ninja &> /dev/null; then
    print_warning "Ninja not found, falling back to default backend"
fi

# Check for pkg-config
if ! command -v pkg-config &> /dev/null; then
    print_error "pkg-config not found. Please install pkg-config."
    exit 1
fi

# Check for E2SAR library
if ! pkg-config --exists e2sar; then
    print_error "E2SAR library not found via pkg-config."
    print_error "Please ensure E2SAR is installed and PKG_CONFIG_PATH is set correctly."
    print_error "Example: export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:\$PKG_CONFIG_PATH"
    exit 1
fi

E2SAR_VERSION=$(pkg-config --modversion e2sar)
print_status "Found E2SAR library version: $E2SAR_VERSION"

# Setup build directory
if [[ ! -d "builddir" ]]; then
    print_status "Setting up build directory..."
    meson setup --buildtype="$BUILD_TYPE" --prefix="$PREFIX" builddir
else
    print_status "Using existing build directory..."
    # Reconfigure if needed
    meson configure --buildtype="$BUILD_TYPE" --prefix="$PREFIX" builddir
fi

# Build
print_status "Building..."
meson compile -C builddir

# Test executable
print_status "Testing executable..."
if [[ -x "builddir/coda-fb" ]]; then
    print_status "Build successful!"

    # Show help to verify it works
    echo
    print_status "Testing help output:"
    echo "----------------------------------------"
    ./builddir/coda-fb --help || true
    echo "----------------------------------------"
else
    print_error "Build failed - executable not found"
    exit 1
fi

# Install if requested
if [[ "$INSTALL" == "true" ]]; then
    print_status "Installing..."

    # Extract actual installation directory from meson introspection
    # This respects CODA or ~/.local/bin overrides
    INSTALL_DIR=$(meson introspect builddir --installed | grep -o '"[^"]*coda-fb"' | head -1 | sed 's/"//g')

    if [[ -z "$INSTALL_DIR" ]]; then
        # Fallback: try to determine from environment
        if [[ -n "$CODA" && "$PREFIX" == "/usr/local" ]]; then
            INSTALL_DIR="$CODA/Linux-x86_64/bin"
        elif [[ "$PREFIX" == "/usr/local" ]]; then
            INSTALL_DIR="$HOME/.local/bin"
        else
            INSTALL_DIR="$PREFIX/bin"
        fi
    fi

    # Get directory path (remove filename if present)
    INSTALL_DIR_PATH=$(dirname "$INSTALL_DIR" 2>/dev/null || echo "$INSTALL_DIR")

    # Check if we need sudo based on actual install directory
    if [[ -w "$INSTALL_DIR_PATH" ]] || [[ ! -e "$INSTALL_DIR_PATH" && -w "$(dirname "$INSTALL_DIR_PATH")" ]]; then
        # Directory is writable or parent is writable (for mkdir)
        meson install -C builddir
    else
        print_warning "Installation directory $INSTALL_DIR_PATH requires elevated privileges"
        sudo meson install -C builddir
    fi

    print_status "Installation complete!"
    print_status "Executable installed to: $INSTALL_DIR"
fi

print_status "All done!"