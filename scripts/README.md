# Build and Run Scripts

Convenience scripts for building and running CODA Frame Builder executables.

## Scripts

### build.sh

Builds both `coda-fb` and `evio_event_parser` executables.

**Basic usage:**
```bash
./build.sh                              # Release build
./build.sh --clean                      # Clean build
./build.sh --type debug                 # Debug build
./build.sh --install                    # Build and install
./build.sh --install --prefix /opt      # Custom install location
```

**Options:**
- `-t, --type` - Build type: `debug`, `release`, `debugoptimized` (default: release)
- `-c, --clean` - Clean build directory before building
- `-i, --install` - Install after building
- `-p, --prefix` - Installation prefix (default: respects $CODA or ~/.local)

### example_run.sh

Example configuration for running `coda-fb` frame builder.

**Usage:**
```bash
# Edit configuration variables first
./example_run.sh
```

**Configure these variables:**
- `EJFAT_URI` - EJFAT control plane URI
- `RECEIVER_IP` - Local IP address for UDP receiver
- `RECEIVER_PORT` - Starting port number
- `OUTPUT_DIR` - Directory for output files
- `NUM_THREADS` - Number of receiver threads

### example_parse.sh

Runs `evio_event_parser` to validate EVIO6 files.

**Usage:**
```bash
./example_parse.sh                          # Parse default sample file
./example_parse.sh my_frame.evio            # Parse specific file
./example_parse.sh my_frame.evio verbose    # Parse with verbose output
```

**Exit codes:**
- `0` - File is valid
- `1` - File is invalid or validation errors found

## Quick Start

```bash
# Build both executables
./build.sh --clean

# Run frame builder (edit configuration first)
./example_run.sh

# Validate output files
./example_parse.sh /tmp/e2sar_frames/frame_*.evio
```

## Notes

- All scripts auto-detect executables in `builddir/` or system PATH
- Scripts provide colored output for errors/warnings/status
- Use `--help` on individual scripts for more options
