# Build and Run Scripts

Convenience wrappers for building and running the CODA Frame Builder executables.

## Scripts

### build.sh

Builds both `coda-fb` and `evio_event_parser` via meson/ninja, and optionally installs them.

**Basic usage:**
```bash
./build.sh                              # Release build
./build.sh --clean                      # Clean build (removes builddir first)
./build.sh --type debug                 # Debug build
./build.sh --install                    # Build and install
./build.sh --install --prefix /opt      # Build and install to /opt/bin
```

**Options:**

| Option | Default | Description |
|---|---|---|
| `-t, --type TYPE` | `release` | Build type: `debug`, `release`, or `debugoptimized`. Anything else is rejected. |
| `-c, --clean` | off | Remove `builddir` before configuring. |
| `-i, --install` | off | Run `meson install` after a successful build. |
| `-p, --prefix PREFIX` | `/usr/local` | Installation prefix. |
| `-h, --help` | — | Show usage. |

**Where `--install` actually installs.** The default prefix `/usr/local` is a sentinel meaning
"let meson decide", so with no `--prefix` the binaries go to `$CODA/Linux-x86_64/bin` if `$CODA`
is set, otherwise `~/.local/bin` — no sudo needed in either case. Passing any *other* prefix, such
as `--prefix /opt`, installs to `<prefix>/bin` instead, and the script escalates with sudo if that
directory is not writable. The script reads the real destination back out of
`meson introspect`, so the path it prints is the path it used.

### example_run.sh

Runs `coda-fb` in **reassembly-only mode** — it passes no frame-building flags, so the output is a
single concatenated binary file, not EVIO-6 files. Edit the configuration block at the top of the
script before running it.

**Usage:**
```bash
./example_run.sh                        # Run with the configured settings
./example_run.sh --threads 8            # Extra arguments are appended to the command line
```

**Configure these variables:**

| Variable | Default | Passed as |
|---|---|---|
| `EJFAT_URI` | `ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000` | `--uri` |
| `RECEIVER_IP` | `192.168.1.100` | `--ip` |
| `RECEIVER_PORT` | `10000` | `--port` |
| `OUTPUT_DIR` | `/tmp/e2sar_frames` | `--output-dir` (created if missing) |
| `FILE_PREFIX` | `frame` | `--prefix` |
| `FILE_EXTENSION` | `.bin` | `--extension` |
| `NUM_THREADS` | `2` | `--threads` |
| `REPORT_INTERVAL` | `5000` | `--report-interval` |

With the defaults above the output is `/tmp/e2sar_frames/frame.bin`. Any arguments you pass to
`example_run.sh` are forwarded to `coda-fb`, so you can turn on frame building ad hoc:

```bash
./example_run.sh --enable-framebuild=1 --fb-output-dir /tmp/e2sar_frames --expected-streams 3
```

### example_parse.sh

Runs `evio_event_parser` on one EVIO-6 file.

**Usage:**
```bash
./example_parse.sh                          # Parse ./frames_thread0_file0000.evio
./example_parse.sh my_frame.evio            # Parse a specific file
./example_parse.sh my_frame.evio verbose    # Parse with --verbose
```

The second argument enables verbose output when it is `verbose`, `--verbose`, or `true`. The
script wraps one file at a time and does not expose the parser's `--fadc-verbose` flag — call
`evio_event_parser` directly for FADC250 hit decoding, or to parse several files:

```bash
evio_event_parser /data/frames/frames_thread0_file0000.evio --fadc-verbose
for f in /data/frames/*.evio; do evio_event_parser "$f" || echo "FAILED: $f"; done
```

**Exit codes:**
- `0` - File is valid
- `1` - File is invalid, missing, or validation errors were found

## Quick Start

```bash
# Build both executables
./build.sh --clean

# Run the frame builder (edit example_run.sh configuration first)
./example_run.sh --enable-framebuild=1 --fb-output-dir /tmp/e2sar_frames --expected-streams 3

# Validate the output (one file per builder thread)
./example_parse.sh /tmp/e2sar_frames/frames_thread0_file0000.evio
```

Frame building requires a build with the ET library present — see the note in the
[top-level README](../README.md#dependencies). Without it, `coda-fb` runs in reassembly-only mode
and produces `.bin` output that `evio_event_parser` cannot validate.

## Notes

- All scripts look for the executables in `builddir/` first, then on `$PATH`.
- Output is colorized: `[INFO]` green, `[WARN]` yellow, `[ERROR]` red.
- Use `--help` on `build.sh` for its full option list; `example_run.sh` and `example_parse.sh` are
  configured by editing their variables or by passing arguments through.
