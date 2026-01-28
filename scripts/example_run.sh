#!/bin/bash
#
# Example script for running CODA Frame Builder
#

set -e

# Configuration - Modify these values for your environment
EJFAT_URI="ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000"
RECEIVER_IP="192.168.1.100"
RECEIVER_PORT="10000"
OUTPUT_DIR="/tmp/e2sar_frames"
FILE_PREFIX="frame"
FILE_EXTENSION=".bin"
NUM_THREADS="2"
REPORT_INTERVAL="5000"  # milliseconds

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

# Get script directory and find executable
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Look for executable in common locations
RECEIVER_EXE=""
if [[ -x "$PROJECT_DIR/builddir/coda-fb" ]]; then
    RECEIVER_EXE="$PROJECT_DIR/builddir/coda-fb"
elif command -v coda-fb &> /dev/null; then
    RECEIVER_EXE="coda-fb"
else
    echo "Error: coda-fb executable not found"
    echo "Please build the project first: cd $PROJECT_DIR && ./scripts/build.sh"
    exit 1
fi

print_status "Using executable: $RECEIVER_EXE"

# Create output directory
if [[ ! -d "$OUTPUT_DIR" ]]; then
    print_status "Creating output directory: $OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR"
fi

# Check directory permissions
if [[ ! -w "$OUTPUT_DIR" ]]; then
    echo "Error: No write permission for output directory: $OUTPUT_DIR"
    exit 1
fi

print_status "Configuration:"
print_status "  EJFAT URI: $EJFAT_URI"
print_status "  Receiver IP: $RECEIVER_IP"
print_status "  Receiver Port: $RECEIVER_PORT"
print_status "  Output Directory: $OUTPUT_DIR"
print_status "  File Prefix: $FILE_PREFIX"
print_status "  File Extension: $FILE_EXTENSION"
print_status "  Threads: $NUM_THREADS"
print_status "  Report Interval: ${REPORT_INTERVAL}ms"

echo
print_warning "Press Ctrl+C to stop the receiver"
echo

# Run the receiver
exec "$RECEIVER_EXE" \
    --uri "$EJFAT_URI" \
    --ip "$RECEIVER_IP" \
    --port "$RECEIVER_PORT" \
    --output-dir "$OUTPUT_DIR" \
    --prefix "$FILE_PREFIX" \
    --extension "$FILE_EXTENSION" \
    --threads "$NUM_THREADS" \
    --report-interval "$REPORT_INTERVAL" \
    "$@"