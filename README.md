# CODA Frame Builder

Multi-threaded frame aggregator for Jefferson Lab CODA DAQ. Receives UDP packets from EJFAT load
balancers, reassembles frames, aggregates matching frames from multiple data streams, and outputs
EVIO-6 format to ET systems or files.

```
UDP from EJFAT LB
  → E2SAR Reassembler   (N receiver threads on ports P .. P+N-1)
  → reception loop      (validate EVIO payload, extract timestamp / ROC id)
  → FrameBuilder        (M builder threads, per-stream FIFOs, alignment)
  → EVIO-6 records      → ET system and/or .evio files (2 GB rollover)
```

## Dependencies

**Required:**
- C++17 compiler (GCC 8+, Clang 6+)
- Meson ≥0.55, Ninja
- [E2SAR](https://github.com/JeffersonLab/E2SAR) library (with pkg-config)
- Boost ≥1.83.0, ≤1.86.0 (system, program_options, chrono, thread, filesystem, url) — located via CMake
- gRPC++ ≥1.51.1
- Protocol Buffers (searched in `/usr/local/lib64`, `/usr/local/lib`, `/usr/lib64`, `/usr/lib`)
- Abseil (`absl_synchronization`, `absl_time`) — same search paths
- GLib 2.0

**Required for frame building:**
- ET library — see the note below.

**Optional:**
- libnuma — enables `--numa`. Without it, passing `--numa N` (N ≥ 0) is a fatal error.

> **The ET library gates *all* frame building, not just ET output.**
> `src/e2sar_reassembler_framebuilder.cpp` is only added to the build when ET is found, and that is
> what defines `ENABLE_FRAME_BUILDER`. If ET is missing, `coda-fb` prints
> `Frame builder: NOT COMPILED (reassembly-only mode)`, `--enable-framebuild` has no effect, and
> `--output-dir` becomes mandatory — **even if you only wanted `--fb-output-dir` file output**.
> Meson reports the outcome in its `Dependencies` summary (`ET Library: Found / Not Found`).

**Install system dependencies (Ubuntu/Debian):**
```bash
sudo apt install libboost-all-dev libgrpc++-dev libprotobuf-dev libglib2.0-dev \
                 libnuma-dev pkg-config meson ninja-build
```
E2SAR and ET are not packaged; build and install them separately. If ET does not ship a
`pkg-config` file, the build falls back to `find_library('et')` plus a check for `et.h`, so set
`LIBRARY_PATH` and `CPATH` to point at your ET installation.

## Build

```bash
# Setup
meson setup builddir --buildtype=release

# Compile
meson compile -C builddir

# Install
meson install -C builddir
```

**Outputs:** `coda-fb` and `evio_event_parser`.

**Install directory** is chosen in this order:
1. A prefix you set explicitly (anything other than meson's default `/usr/local`) → `<prefix>/bin`
2. `$CODA` set → `$CODA/Linux-x86_64/bin`
3. Otherwise → `~/.local/bin`

Because `/usr/local` is treated as "no prefix given", passing `--prefix=/usr/local` does *not*
install to `/usr/local/bin`; it falls through to `$CODA` or `~/.local`.

See [`scripts/README.md`](scripts/README.md) for the convenience build/run wrappers.

### Docker

A self-contained image is available for deployment to remote hosts without
installing E2SAR, ET or the Boost/gRPC stack there:

```bash
./docker/build.sh --tag coda-fb:v1.0.0

docker run -d --network host -v /data/frames:/data \
  -e EJFAT_URI='ejfat://token@cp-host:18347/lb/1?data=10.0.0.5:10000' \
  -e RECEIVER_IP=10.0.0.5 -e THREADS=4 -e EXPECTED_STREAMS=3 \
  coda-fb:v1.0.0
```

Host networking is required, and `docker stop -t 90` is needed so the load
balancer deregistration completes. See [`docker/README.md`](docker/README.md)
for the full parameter list, ET options, and registry/tarball deployment.

## Usage

### coda-fb (Frame Builder)

`coda-fb` has two modes. **Reassembly-only is the default**; frame building must be turned on
explicitly with `--enable-framebuild=1` (it takes a value — a bare `--enable-framebuild` will
consume the next argument).

**Reassembly-only (default): raw frames concatenated into one file**
```bash
coda-fb --uri 'ejfat://token@host:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 --port 10000 \
  --threads 4 --output-dir /data/raw --prefix events --extension .bin
```
Writes a single file `/data/raw/events.bin` (truncated at startup).

**Frame building to file**
```bash
coda-fb --uri 'ejfat://...' --ip 192.168.1.100 --port 10000 \
  --enable-framebuild=1 --fb-output-dir /data/frames \
  --fb-threads 8 --threads 4 --expected-streams 8
```
Writes `/data/frames/frames_thread<N>_file<MMMM>.evio`, one file series per builder thread.

**Frame building to ET**
```bash
coda-fb --uri 'ejfat://...' --ip 192.168.1.100 --port 10000 \
  --enable-framebuild=1 --et-file /tmp/et_sys --et-host localhost \
  --fb-threads 8 --threads 4
```

**Dual output (ET + file backup)**
```bash
coda-fb --uri 'ejfat://...' --ip 192.168.1.100 --port 10000 \
  --enable-framebuild=1 --et-file /tmp/et_sys \
  --fb-output-dir /data/backup --fb-output-prefix backup
```

**Current test run for a 3-ROC configuration**
```bash
./coda-fb -u "$EJFAT_URI" --novalidate --ip 129.57.109.231 --threads 4 \
  --enable-framebuild=1 --expected-streams=3 --fb-threads 1 --fb-output-dir "$CODA_DATA"
```

Stop with `Ctrl+C`; the handler deregisters from the EJFAT load balancer, drains the builder
threads, and prints final statistics.

#### Options

**Required**

| Option | Default | Description |
|---|---|---|
| `--uri`, `-u` | — | EJFAT control-plane URI. The only strictly required option. |

Exactly one of `--ip` or `--autoip` must also be given.

**Network**

| Option | Default | Description |
|---|---|---|
| `--ip` | — | Local IP to receive UDP on. Conflicts with `--autoip`. |
| `--autoip` | off | Auto-detect the local host IP. Conflicts with `--ip`. |
| `--port`, `-p` | `10000` | **Starting** UDP port. Receiver thread *i* listens on `port + i`, so the range is `port .. port + threads - 1`. |
| `--ipv6`, `-6` | off | Prefer IPv6 for control-plane connections. |
| `--novalidate`, `-v` | off | Do **not** validate TLS certificates. (`-v` is *not* a verbosity flag.) |
| `--withcp`, `-c` | on | Enable control-plane interaction. Currently always on — see [Known quirks](#known-quirks). |

**Mode**

| Option | Default | Description |
|---|---|---|
| `--enable-framebuild` | `false` | Enable aggregation. Takes a value: `--enable-framebuild=1`. Requires an ET-enabled build. |

**Reassembly-only output** (required when frame building is off)

| Option | Default | Description |
|---|---|---|
| `--output-dir`, `-o` | — | Output directory. Must already exist and be writable. |
| `--prefix` | `events` | Output file name stem. |
| `--extension`, `-e` | `.bin` | Output file extension; a leading `.` is added if missing. |

**Frame-builder output** (at least one of ET or file output is required when frame building is on)

| Option | Default | Description |
|---|---|---|
| `--et-file` | *(empty)* | ET system file, e.g. `/tmp/et_sys_pagg`. Empty disables ET output. |
| `--et-host` | *(empty)* | ET host. Empty = local/broadcast; otherwise a hostname or IP. |
| `--et-port` | `0` | ET server port; `0` uses the ET default. |
| `--et-event-size` | `2097152` | Maximum ET event size in bytes (2 MB). |
| `--fb-output-dir` | *(empty)* | Frame-builder file output directory. Empty disables file output. |
| `--fb-output-prefix` | `frames` | File name prefix → `<prefix>_thread<N>_file<MMMM>.evio`. |
| `--fb-threads` | `1` | Parallel builder threads. Must be 1–32. |

**Aggregation tuning**

| Option | Default | Description |
|---|---|---|
| `--expected-streams` | `1` | Data streams expected per frame number. A frame is emitted as soon as all of them arrive, or after `--frame-timeout` if incomplete. |
| `--frame-timeout` | `1000` | Milliseconds to wait for the missing streams before building a partial frame. |
| `--framenumber-slop` | `0` | Maximum allowed difference between corrected event numbers within one frame. Data-quality validation only; it does not change alignment. |
| `--verbose-frames` | off | Print every frame plus builder alignment messages. |
| `--verbose-reassemble` | off | Print reassembler event numbers for all streams. Intended for reassembly-only mode. |

**Performance and placement**

| Option | Default | Description |
|---|---|---|
| `--threads`, `-t` | `1` | Parallel UDP receiver threads. Ignored if `--cores` is given. |
| `--cores` | *(none)* | Space-separated CPU core list to pin receiver threads to, e.g. `--cores 4 5 6 7`. **Overrides `--threads`** — the thread count becomes the length of this list. |
| `--numa` | `-1` | Bind memory allocation to this NUMA node. Requires a libnuma-enabled build. |
| `--bufsize`, `-b` | `3145728` | UDP socket receive buffer size in bytes (3 MB). |
| `--timeout` | `500` | E2SAR event reassembly timeout in milliseconds. |
| `--report-interval` | `5000` | Statistics reporting interval in milliseconds. |
| `--help`, `-h` | — | Show options and built-in examples. |

### evio_event_parser (Validator)

```
evio_event_parser <evio_file> [--verbose] [--fadc-verbose]
evio_event_parser --help
```

| Option | Description |
|---|---|
| `--verbose` | Full EVIO-6 structure: file and record headers with all fields, bank/segment tags, frame numbers, timestamps, ROC IDs, payload sizes. |
| `--fadc-verbose` | Decode FADC250 detector hits — crate (ROC ID), slot (0–20), channel (0–15), integrated charge (13-bit ADC), and absolute hit time in ns. |
| `-h`, `--help` | Usage and examples. |

Exactly one input file is accepted; a second positional argument is an error.

```bash
evio_event_parser frames_thread0_file0000.evio
evio_event_parser frames_thread0_file0000.evio --verbose
evio_event_parser frames_thread0_file0000.evio --fadc-verbose
evio_event_parser frames_thread0_file0000.evio --verbose --fadc-verbose
```

**Exit codes:** `0` = valid, `1` = validation errors or the file could not be opened.

**Validates:**
- EVIO-6 file header: file type ID `0x4556494F` (`"EVIO"`) and magic number `0xC0DA0100`
- Record headers and their magic numbers
- Streaming physics event structure: aggregated frame bank `0xFF60`, stream info bank `0xFF31`,
  time-slice segment `0x32`, aggregation info segment `0x42`
- Length consistency across headers, banks, and segments

## How frame building works

Reception (`src/coda-fb.cpp`):

1. `Reassembler::recvEvent()` returns one reassembled payload per stream, with an event number and
   a data ID.
2. The payload is parsed and validated — magic number, endianness, timestamp, and ROC ID. Invalid
   payloads are counted and dropped; wrong-endian payloads are byte-swapped and counted.
3. The slice is handed to the frame builder, which takes ownership of the buffer.

Aggregation (`src/e2sar_reassembler_framebuilder.cpp`):

4. Slices are routed to a builder thread by `frameNumber % fb-threads`, so all slices of one frame
   always land on the same thread and threads never contend for a frame.
5. Each builder thread keeps a **per-stream FIFO** keyed by data ID.
6. Streams rarely start at the same event number. At startup the builder computes a **constant
   per-stream correction offset** (`minEventNum - streamEventNum`) and keeps it fixed for the whole
   run.
7. A frame is built only when every non-empty FIFO head carries the same *corrected* event number.
   If a stream lags, only the lagging streams are advanced until alignment is restored.
   `--frame-timeout` forces a partial build when a stream never arrives.
8. The thread emits an EVIO-6 record: a 14-word record header, then the `0xFF60` aggregated frame
   bank containing the `0xFF31` stream info bank, the `0x32` time-slice segment, and the `0x42`
   aggregation info segment.
9. File output rolls over at 2 GB per file, with an independent file series per builder thread.

Note that aggregation keys on the **reassembler's event number**, not on the payload timestamp,
even though parts of the API still call this value a timestamp. The payload timestamp is carried
through into the EVIO-6 output.

## Known quirks

These are current code behaviors, documented here so the flags are not misread:

- **`--withcp` cannot be turned off.** It is declared as a boolean switch whose default is already
  `true`, so passing it changes nothing and there is no CLI way to disable the control plane.
- **`-v` means `--novalidate`,** not verbose. Use `--verbose-frames` or `--verbose-reassemble` for
  verbosity.
- **`coda-fb --help` examples still use the old binary name** `e2sar_receiver`. The options shown
  are correct; the program name is not.
- **`--cores` silently overrides `--threads`.**

## License

MIT License - Copyright (c) 2024 Jefferson Science Associates
