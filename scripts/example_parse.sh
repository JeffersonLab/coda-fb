#!/bin/bash
#
# Example script for running EVIO Event Parser
#

set -e

# Configuration - Modify these values for your environment
EVIO_FILE="${1:-frames_thread0_file0000.evio}"
VERBOSE="${2:-false}"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Get script directory and find executable
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Look for executable in common locations
PARSER_EXE=""
if [[ -x "$PROJECT_DIR/builddir/evio_event_parser" ]]; then
    PARSER_EXE="$PROJECT_DIR/builddir/evio_event_parser"
elif command -v evio_event_parser &> /dev/null; then
    PARSER_EXE="evio_event_parser"
else
    print_error "evio_event_parser executable not found"
    print_error "Please build the project first: cd $PROJECT_DIR && ./scripts/build.sh"
    exit 1
fi

print_status "Using executable: $PARSER_EXE"

# Check if file exists
if [[ ! -f "$EVIO_FILE" ]]; then
    print_error "EVIO file not found: $EVIO_FILE"
    echo
    echo "Usage: $0 [EVIO_FILE] [verbose]"
    echo
    echo "Examples:"
    echo "  $0                                    # Parse default sample file"
    echo "  $0 my_frame.evio                      # Parse specific file"
    echo "  $0 my_frame.evio verbose              # Parse with verbose output"
    echo "  $0 /data/frames/*.evio                # Parse multiple files"
    exit 1
fi

print_status "Parsing EVIO file: $EVIO_FILE"

# Build command
CMD=("$PARSER_EXE" "$EVIO_FILE")
if [[ "$VERBOSE" == "verbose" ]] || [[ "$VERBOSE" == "--verbose" ]] || [[ "$VERBOSE" == "true" ]]; then
    CMD+=("--verbose")
    print_status "Verbose mode: enabled"
fi

echo
print_warning "Running parser..."
echo

# Run the parser and capture exit code
if "${CMD[@]}"; then
    EXIT_CODE=$?
    echo
    print_status "Validation: PASSED (exit code: $EXIT_CODE)"
    exit 0
else
    EXIT_CODE=$?
    echo
    print_error "Validation: FAILED (exit code: $EXIT_CODE)"
    exit $EXIT_CODE
fi
