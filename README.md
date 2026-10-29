# CODA Frame Builder

Multi-threaded frame aggregator for Jefferson Lab CODA DAQ. Receives UDP packets from an EJFAT load
balancer, reassembles frames, aggregates matching frames from multiple data streams, and outputs
EVIO-6 format to ET systems, files, and/or a second EJFAT load balancer.

```
UDP from EJFAT LB #1            (--recv-ejfat-uri: coda-fb is a worker here)
  → E2SAR Reassembler   (N receiver threads on ports P .. P+N-1)
  → reception loop      (validate EVIO payload, extract timestamp / ROC id)
  → FrameBuilder        (M builder threads, per-stream FIFOs, alignment)
  → EVIO-6 records      → FrameSender → E2SAR Segmenter → UDP to EJFAT LB #2
                          (--send-ejfat-uri: coda-fb is a sender there)
                        → ET system and/or .evio files (2 GB rollover)
```

## Dependencies

**Required:**
- C++17 compiler (GCC 8+, Clang 6+)
- Meson ≥0.55, Ninja
- [E2SAR](https://github.com/JeffersonLab/E2SAR) ≥0.4.0rc1 (with pkg-config)
- Boost 1.89.0 exactly (url, program_options, chrono, thread, filesystem, log) — located via CMake
- gRPC++ ≥1.74.1
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

# Test (optional; builds and runs the tests, see Testing)
meson test -C builddir

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

### Docker: build, ship and run on a remote node

The image is self-contained: the Docker build compiles ET, E2SAR (v0.4.0rc1) and coda-fb inside
the container, so no separate compile step is needed and the remote node only needs Docker — not
E2SAR, ET or the Boost/gRPC stack.

**1. Build the image** on an **x86_64 Linux** machine. The image is `linux/amd64` because
E2SAR's dependency bundle exists only for amd64; building on Apple Silicon falls back to emulation
and is impractically slow (and can run out of memory compiling E2SAR).

```bash
git clone https://github.com/JeffersonLab/coda-fb.git && cd coda-fb
./docker/build.sh --tag coda-fb:v1.1.0
```

Useful `build.sh` options: `--save FILE` (also write a gzipped tarball), `--push REGISTRY` (tag
into a registry and push), `--no-cache`, `--buildtype` (default `release`), and `--e2sar-ref`,
`--e2sar-deps-ver`, `--et-ref` to pin versions (defaults v0.4.0rc1 and v16.6.0). The build fails
early if the frame builder was not compiled in or the binary cannot run. The build context is the
working tree, so uncommitted changes are included.

**2. Ship it to the remote node**, as a tarball or through a registry:

```bash
# tarball
./docker/build.sh --tag coda-fb:v1.1.0 --save coda-fb-v1.1.0.tar.gz
scp coda-fb-v1.1.0.tar.gz remote:/tmp/
ssh remote 'gunzip -c /tmp/coda-fb-v1.1.0.tar.gz | docker load'

# or a registry
./docker/build.sh --tag coda-fb:v1.1.0 --push <dockerhub-user>
ssh remote docker pull <dockerhub-user>/coda-fb:v1.1.0
```

**3. Run it on the remote node.** `--network host` is required: coda-fb registers its IP with
the load balancer, which sends UDP straight to it. Stop it with `docker stop -t 90` (or longer)
so the LB deregistration and the drain of forwarded frames complete; the container is given
`SHUTDOWN_GRACE` (default 60 s) before being killed.

Frame building to EVIO files (`ENABLE_FRAMEBUILD=1` and `FB_OUTPUT_DIR=/data` are the container
defaults, so mount a host directory on `/data`):

```bash
docker run -d --name coda-fb --network host -v /data/frames:/data \
  -e RECV_EJFAT_URI='ejfats://<token>@<cp-host>:18347/lb/<id>?data=10.0.0.1&sync=10.0.0.1:19010' \
  -e RECEIVER_IP=10.0.0.5 -e THREADS=4 -e EXPECTED_STREAMS=3 \
  coda-fb:v1.1.0
```

Also forwarding to a second load balancer, with Prometheus metrics:

```bash
docker run -d --name coda-fb --network host -v /data/frames:/data \
  -e RECV_EJFAT_URI="$LB1_URI" -e SEND_EJFAT_URI="$LB2_URI" \
  -e RECEIVER_IP=10.0.0.5 -e THREADS=4 -e EXPECTED_STREAMS=3 \
  -e METRICS_PORT=9465 \
  coda-fb:v1.1.0
```

Raw flags instead of environment variables: if `--recv-ejfat-uri` (or the deprecated `--uri`) is
on the command line, the environment is not consulted:

```bash
docker run -d --name coda-fb --network host -v /data/frames:/data coda-fb:v1.1.0 \
  --recv-ejfat-uri "$LB1_URI" --ip 10.0.0.5 --threads 4 \
  --enable-framebuild=1 --expected-streams 3 --fb-output-dir /data
```

Managing it:

```bash
docker logs -f coda-fb                    # statistics report every 5 s
docker stop -t 90 coda-fb                 # clean shutdown (SIGTERM is turned into SIGINT)
docker run --rm coda-fb:v1.1.0 --help     # all options
docker run --rm -v /data/frames:/data coda-fb:v1.1.0 \
  evio_event_parser /data/frames_thread0_file0000.evio    # validate the output
```

With Docker Compose instead (host networking, stop grace period and log rotation are preset):

```bash
cp docker/coda-fb.env.example coda-fb.env && $EDITOR coda-fb.env
docker compose -f docker/docker-compose.yml up -d
```

Notes:

- Set `FB_OUTPUT_DIR=` (empty) for an ET-only or forward-only run.
- ET: `ET_FILE` with `ET_HOST`/`ET_PORT` for an external ET system, or `RUN_ET_START=1` with
  `--ipc=host` to start one inside the container.
- When forwarding, keep `SHUTDOWN_GRACE` (and `docker stop -t`) comfortably above
  `SEND_DRAIN_TIMEOUT` (default 10 s).
- The URIs carry instance tokens: keep them in an env file with mode `600`, not in shell history.
  coda-fb and the entrypoint never print them.
- To watch a live dashboard of a remote coda-fb, see
  [`doc/monitoring.md`](doc/monitoring.md) section 6.5.

[`docker/README.md`](docker/README.md) has the full list of environment variables, ET details,
Perlmutter (podman-hpc/Shifter) instructions and troubleshooting.

## Usage

### coda-fb (Frame Builder)

`coda-fb` has two modes. **Reassembly-only is the default**; frame building must be turned on
explicitly with `--enable-framebuild=1` (it takes a value — a bare `--enable-framebuild` will
consume the next argument).

`--recv-ejfat-uri` is the **instance URI** of the load balancer to receive from (the one returned
when it was reserved). It replaces `--uri`/`-u`; see [Migrating from `--uri`](#migrating-from---uri).

#### Output options

Where the data goes depends on the mode.

**Reassembly-only mode** (default, `--enable-framebuild=0`) has a single output: every event
reassembled from the receive LB is appended, as raw bytes, to one file
`<--output-dir>/<--prefix><--extension>` (default `events.bin`). No frames are built, and ET and
LB forwarding are not available in this mode.

**Frame-building mode** (`--enable-framebuild=1`) has three outputs for the built EVIO-6 frames.
Any combination can be enabled, and at least one is required:

| Output | Enabled by | What it produces |
|---|---|---|
| **Second EJFAT load balancer** | `--send-ejfat-uri` | Each frame sent as one E2SAR event, fragmented into UDP packets for LB #2. See [Forwarding built frames](#forwarding-built-frames-to-a-second-load-balancer). |
| **ET system** | `--et-file` (with `--et-host`, `--et-port`) | Each frame put into one ET event at the GRAND_CENTRAL station. |
| **EVIO file** | `--fb-output-dir` | `.evio` files: a 56-byte EVIO-6 file header, then the frames back to back; one file series per builder thread, `<prefix>_thread<N>_file<MMMM>.evio`, rolling over at 2 GB. |

All three receive exactly the same frame bytes; they share one buffer, so nothing is copied. For
each frame the order is: forward to the LB first (an asynchronous hand-off, so the frame leaves as
soon as it is built), then ET, then file. A failure in one output does not stop the others; it is
counted per output.

The outputs differ in a few ways:

- **Partial frames** (a stream still missing after `--frame-timeout`) always go to ET and file;
  they go to the LB only with `--send-partial-frames=1` (the default).
- **At shutdown**, a frame that has just been built is still forwarded to the LB, while ET and
  file output skip it once shutdown has begun.
- **Backpressure** applies only to the LB output: an in-flight limit (`--send-queue-frames`,
  `--send-queue-bytes`) with a `block` or `drop` policy (`--send-overflow`). ET and file writes
  happen directly in the builder thread.

> **ET gates all frame building.** If the ET library is not found at build time, frame-building
> mode is not compiled in at all, so the EVIO file output and LB forwarding are unavailable too;
> only reassembly-only mode remains. The Docker image always includes ET. See
> [Dependencies](#dependencies).

Not data outputs: the periodic **statistics report** on stdout (`--report-interval`) and the
**Prometheus `/metrics` endpoint** (`--metrics-port`, see [Statistics](#statistics)) carry
counters only, never frame data.

#### Examples

**Reassembly-only (default): raw frames concatenated into one file**
```bash
coda-fb --recv-ejfat-uri 'ejfats://token@host:18347/lb/1?data=10.0.0.1&sync=10.0.0.1:19010' \
  --ip 192.168.1.100 --port 10000 \
  --threads 4 --output-dir /data/raw --prefix events --extension .bin
```
Writes a single file `/data/raw/events.bin` (truncated at startup).

**Frame building to file**
```bash
coda-fb --recv-ejfat-uri 'ejfats://...' --ip 192.168.1.100 --port 10000 \
  --enable-framebuild=1 --fb-output-dir /data/frames \
  --fb-threads 8 --threads 4 --expected-streams 8
```
Writes `/data/frames/frames_thread<N>_file<MMMM>.evio`, one file series per builder thread.

**Frame building to ET**
```bash
coda-fb --recv-ejfat-uri 'ejfats://...' --ip 192.168.1.100 --port 10000 \
  --enable-framebuild=1 --et-file /tmp/et_sys --et-host localhost \
  --fb-threads 8 --threads 4
```

**Dual output (ET + file backup)**
```bash
coda-fb --recv-ejfat-uri 'ejfats://...' --ip 192.168.1.100 --port 10000 \
  --enable-framebuild=1 --et-file /tmp/et_sys \
  --fb-output-dir /data/backup --fb-output-prefix backup
```

**Current test run for a 3-ROC configuration**
```bash
./coda-fb --recv-ejfat-uri "$EJFAT_URI" --novalidate --ip 129.57.109.231 --threads 4 \
  --enable-framebuild=1 --expected-streams=3 --fb-threads 1 --fb-output-dir "$CODA_DATA"
```

**Frame building with forwarding to a second load balancer** (plus a file copy)
```bash
coda-fb --recv-ejfat-uri 'ejfats://token1@cp1:18347/lb/1?data=10.0.0.1&sync=10.0.0.1:19010' \
  --send-ejfat-uri 'ejfats://token2@cp2:18347/lb/7?data=10.0.1.1&sync=10.0.1.1:19010' \
  --ip 192.168.1.100 --port 10000 --threads 4 \
  --enable-framebuild=1 --expected-streams 3 --fb-threads 1 \
  --fb-output-dir /data/frames \
  --send-data-id 1 --send-queue-frames 256 --send-overflow block
```
Each built frame is sent to LB #2 as one E2SAR event as soon as it is built. `--fb-output-dir` /
`--et-file` are optional when forwarding. See
[Forwarding built frames](#forwarding-built-frames-to-a-second-load-balancer).

Stop with `Ctrl+C` (SIGINT); shutdown deregisters from LB #1 first, drains the builder threads and
the frames being forwarded, removes the sender registration from LB #2, and prints final
statistics.

#### Migrating from `--uri`

`--uri`/`-u` is **retained as a deprecated alias** of `--recv-ejfat-uri`: it means exactly the same
thing, prints a deprecation warning, and cannot be combined with `--recv-ejfat-uri`. Existing
command lines keep working unchanged. To migrate, rename `--uri`/`-u` to `--recv-ejfat-uri`. In
the container, `EJFAT_URI` is likewise kept as an alias of `RECV_EJFAT_URI`. There is no implicit
fallback in the other direction: forwarding is enabled only by an explicit `--send-ejfat-uri`.

#### Options

**Required**

| Option | Default | Description |
|---|---|---|
| `--recv-ejfat-uri` | — | Instance URI of the load balancer to receive from (LB #1). The only strictly required option. |
| `--uri`, `-u` | — | Deprecated alias of `--recv-ejfat-uri`; mutually exclusive with it. |

Exactly one of `--ip` or `--autoip` must also be given.

**Network**

| Option | Default | Description |
|---|---|---|
| `--ip` | — | Local IP to receive UDP on. Conflicts with `--autoip`. |
| `--autoip` | off | Auto-detect the local host IP. Conflicts with `--ip`. |
| `--port`, `-p` | `10000` | **Starting** UDP port. Receiver thread *i* listens on `port + i`, so the range is `port .. port + threads - 1`. |
| `--ipv6`, `-6` | off | Prefer IPv6 for control-plane connections. |
| `--novalidate`, `-v` | off | Do **not** validate TLS certificates. (`-v` is *not* a verbosity flag.) |
| `--withcp`, `-c` | on | Control-plane interaction with LB #1. `--withcp=0` disables it — debugging only: no worker registration, and packets must carry LB headers. A bare `--withcp` still means on. |

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

**Forwarding to a second load balancer** (requires `--enable-framebuild=1`)

| Option | Default | Description |
|---|---|---|
| `--send-ejfat-uri` | *(empty)* | Instance URI of LB #2. Empty disables forwarding. Must have `data=`; with the control plane also `sync=` (IPv4), a token and an LB id. Must not name the same LB instance as `--recv-ejfat-uri`. |
| `--send-data-id` | `1` | E2SAR data id carried by every forwarded frame. Must differ between coda-fb instances whose frames meet in one downstream reassembler. |
| `--send-src-id` | `0` | E2SAR event source id in sync packets, unique per sender. `0` derives it (FNV-1a) from the host name; the value is printed at startup. |
| `--send-sender-ip` | auto | Address(es) registered as sender with LB #2. Default: the local address that routes to its data address. |
| `--send-event-offset` | `1` | Outbound event number = offset + built frame number. Must be ≥ 1 (E2SAR reserves 0). |
| `--send-partial-frames` | `1` | Also forward partial frames (fewer streams than expected). ET/file always get them. |
| `--send-mtu` | `1500` | MTU incl. IP/UDP/LB/RE headers; `0` = outgoing interface MTU. |
| `--send-sockets` | `4` | Send sockets/threads (spreads packets over LB ports). |
| `--send-bufsize` | `3145728` | Send socket buffer (bounded by `net.core.wmem_max`). |
| `--send-rate` | `-1` | Rate limit in Gbps; negative = unlimited. |
| `--send-ipv6` | off | Send to the IPv6 data address of LB #2. |
| `--send-lbhdr-version` | `2` | LB header version (2 or 3). |
| `--send-warmup` | `1000` | ms of sync packets before the first frame. |
| `--send-nocp` | off | Debugging only: no sender registration, no sync packets. |
| `--send-queue-frames` | `256` | Max frames in flight (handed to E2SAR, not yet released). 1–65534. |
| `--send-queue-bytes` | 512 MiB | Max bytes in flight. A single larger frame is sent on its own. |
| `--send-overflow` | `block` | At the limit: `block` the builder threads, or `drop` the frame after `--send-block-timeout`. |
| `--send-block-timeout` | `1000` | ms to wait for space before dropping (`drop` policy only). |
| `--send-drain-timeout` | `10000` | ms allowed at shutdown for completed frames to be sent. |

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
| `--metrics-port` | `0` | Serve Prometheus metrics at `http://<metrics-bind>:<port>/metrics`; `0` disables. See [Statistics](#statistics). |
| `--metrics-bind` | `127.0.0.1` | Address the metrics endpoint listens on. The default is reachable from this host only; use `0.0.0.0` or the host address for a remote Prometheus. |
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

## Forwarding built frames to a second load balancer

With `--send-ejfat-uri`, each built frame is forwarded to a second EJFAT load balancer, on the
fly, by an E2SAR `Segmenter`:

```
LB #1 → Reassembler → reception loop → FrameBuilder ──► FrameSender ──► Segmenter ──► LB #2
                                                     └─► ET / file (unchanged)
```

**What is sent.** Exactly the serialized EVIO-6 record that goes to ET (and to the file, after
its 56-byte file header): one record = one E2SAR event, fragmented by E2SAR into MTU-sized
packets. Incoming packets and individual stream slices are never forwarded. A downstream E2SAR
reassembler reconstructs the record byte for byte. The buffer is shared between the sender and
ET/file output, so no copy is made; it is freed when its last user (normally E2SAR's send
callback) releases it.

**Which frames.** A frame is forwarded once, when its builder thread serializes it, before ET and
file output. That includes partial frames (timeout or stream misalignment) unless
`--send-partial-frames=0`, in which case they are counted as *partial not forwarded*. Frames that
never complete — still collecting slices at shutdown, or failing EVIO validation — are never
sent; they are counted as *discarded at shutdown* or *build failed*.

**Identifiers.**

| E2SAR field | Value |
|---|---|
| RE header event number | `--send-event-offset` (default 1) + the builder's *corrected* event number, i.e. the frame number written in the record's time-slice segment. E2SAR's `addToSendQueue()` treats 0 as "next sequential number", which is why the offset must be ≥ 1. |
| RE header data id | `--send-data-id` (default 1), the same for every frame from this instance. |
| Sync header source id | `--send-src-id`, or a hash of the host name. |
| LB header tick / entropy | Set by E2SAR (microsecond clock, random per-event entropy). |

Corrected event numbers are unique per run with `--fb-threads 1` (the default) or when all streams
start aligned. With several builder threads *and* misaligned streams, the per-thread correction
factors can make two threads produce the same number; use a different `--send-data-id` per
instance and keep `--fb-threads 1` if that matters downstream.

**Startup** runs against the data flow, so that nothing arrives before it can be handled and a
failure leaves nothing registered: (1) register as a sender on LB #2 (`LBManager::addSenders`,
instance token) and start the `Segmenter` (sync warm-up, sockets, threads); (2) start the frame
builder; (3) register as a worker on LB #1 and start receiving. If any step fails, the steps
already done are undone in reverse — including removing the sender registration — and coda-fb
exits non-zero.

**Backpressure.** A frame is *in flight* from the moment the sender admits it until E2SAR's send
callback releases its buffer. In-flight frames and bytes are bounded by `--send-queue-frames` and
`--send-queue-bytes`. (E2SAR's own queue does not bound memory: its send thread moves events into
an unbounded thread-pool queue.) At the limit:

- `--send-overflow=block` (default): the builder thread waits. Pressure propagates to the builder
  FIFOs and then to E2SAR's reassembly queue on LB #1, where losses show up as *Lost on enqueue*.
  Time spent waiting is reported as *Blocked*.
- `--send-overflow=drop`: the builder waits up to `--send-block-timeout` ms, then drops the frame,
  counts it as *Dropped: overflow* and logs it (rate-limited). ET/file output still get the frame.

The only retry is when E2SAR's queue reports full, which means the frame was *not* enqueued, so it
cannot create a duplicate; retries are bounded by the same deadline and counted.

**Send failures.** E2SAR 0.4.0rc1 calls the send callback whether or not `sendmsg()` succeeded and
discards the per-event result, so failures are visible only per packet: *Packet send errors* in
the report (E2SAR's send `errCnt`, with the last `errno`). UDP gives no delivery confirmation;
*Released by E2SAR* means E2SAR finished sending, **not** that LB #2 or a downstream worker
received the frame.

**Shutdown** (SIGINT, or SIGTERM in the container): deregister from LB #1 and stop receiving;
stop the builder threads, each finishing and forwarding its current frame; wait until E2SAR has
released every in-flight frame or `--send-drain-timeout` expires; stop the `Segmenter`; remove the
sender registration from LB #2. A frame still waiting for queue space at the deadline is counted
as *Dropped: shutdown*; frames still in flight are reported as *Abandoned at shutdown* (they may
or may not have been sent), the `Segmenter` is then left running until process exit because it
still uses their buffers, and the sender registration is removed anyway.

**Operational notes.** The E2SAR send thread polls its queue without sleeping, so expect one core
busy while forwarding. Raise `net.core.wmem_max` if you raise `--send-bufsize`.

## Statistics

coda-fb prints a report every `--report-interval` ms and a final one at exit. Rates are deltas over
the last interval, divided by the measured interval (the final report averages over the run).
Values marked `~` are estimated from histogram buckets; everything else is an exact count. All
values are cumulative since process start: coda-fb has no run transition inside one process, so a
new run (a new process) starts from zero.

| Section | Source | Contents |
|---|---|---|
| Health | coda-fb | Readiness of receiver, builder and sender (`READY` once initialized, `NOT READY` during shutdown); age of the last received event, built frame, submitted frame and released frame. |
| Receive: E2SAR reassembler | E2SAR `Reassembler::getStats()` | UDP packets and payload bytes received; events reassembled (includes events then lost on enqueue); events lost in reassembly (segments missing at `--timeout`); lost on enqueue (E2SAR queue full); bad headers; data socket errors; gRPC errors. |
| Receive: reception loop | coda-fb | Events and bytes taken from E2SAR; `recvEvent` errors; payload validation errors (dropped); wrong-endian payloads (byte-swapped, kept). |
| Frame building | coda-fb | Slices in; frames complete / partial / build failed / discarded at shutdown; serialized frames/s and bytes/s; slices and frames pending now; frame size, serialization time and assembly latency (first slice arrival → frame finalized) as ~p50/~p99; frames delivered to every enabled output; ET/file output errors. |
| Forwarding | coda-fb | Offered = submitted + dropped (overflow, shutdown) + rejected; released by E2SAR; partial frames not forwarded; queue-full retries; in-flight frames and bytes vs capacity; submits that blocked and total blocked time; queue wait (offered → submitted) and E2SAR release latency (submitted → released) as ~p50/~p99; abandoned at shutdown (final report). |
| Forwarding: E2SAR segmenter | E2SAR `Segmenter::getSendStats()/getSyncStats()` | Packets (with coda-fb's estimate `Σ ceil(frame bytes / payload per packet)` for comparison); packet send errors and last errno; sync messages and errors. Bytes sent on the wire are not exposed by E2SAR. |

All counters are lock-free atomics (relaxed); a report reads them one at a time, so related values
can be one in-flight update apart. Definitions and measurement boundaries are documented on each
field in [`src/pipeline_stats.hpp`](src/pipeline_stats.hpp).

### Prometheus

With `--metrics-port N`, coda-fb serves the same statistics in Prometheus text format at
`http://<metrics-bind>:N/metrics` (embedded Boost.Beast server on its own thread; a scrape only
reads atomics and E2SAR's counters, it never takes a lock the data path uses). The endpoint is
started before anything else, so a busy port fails startup before any LB registration, and it
keeps serving through shutdown so readiness can be seen dropping to 0; it stops before the E2SAR
objects are deleted.

```bash
coda-fb --recv-ejfat-uri "$LB1_URI" ... --metrics-port 9465 --metrics-bind 0.0.0.0
curl -s http://daq-fb1:9465/metrics | grep coda_fb_send_frames_total
```

Metric names, types and labels, PromQL examples, a scrape configuration, alert rules and a
Grafana dashboard are in [`doc/monitoring.md`](doc/monitoring.md) and
[`doc/monitoring/`](doc/monitoring/). Families for a disabled stage (no frame building, no
forwarding) are not exported, and E2SAR's own families appear once startup has created the
reassembler and segmenter.

### Live dashboard of a coda-fb on a remote host

Run Prometheus and Grafana on the remote host next to coda-fb, keep every port bound to
localhost there, and view Grafana from your own machine through an SSH tunnel. Nothing is exposed
on the network; your machine needs only `ssh` and a browser.

```
your laptop                            remote host
browser ──ssh -L 3000──────────────►   Grafana     127.0.0.1:3000
                                         └─► Prometheus  127.0.0.1:9090
                                               └─► coda-fb /metrics 127.0.0.1:9465
```

1. **Build and ship the image** as in
   [Docker: build, ship and run on a remote node](#docker-build-ship-and-run-on-a-remote-node).

2. **Copy the monitoring files**: the whole `doc/monitoring/` directory, since the stack loads the
   alert rules and the dashboard from its parent directory:

   ```bash
   scp -r doc/monitoring remote:~/coda-fb-monitoring
   ```

   If the remote host cannot pull from Docker Hub, ship those images too:
   `docker save prom/prometheus:v2.53.0 grafana/grafana:11.1.0 | gzip > monitoring-images.tar.gz`.

3. **Run coda-fb with metrics on**, on the remote host. The endpoint binds to localhost by
   default, which is what this setup needs:

   ```bash
   docker run -d --name coda-fb --network host -v /data/frames:/data \
     -e RECV_EJFAT_URI="$LB1_URI" -e SEND_EJFAT_URI="$LB2_URI" \
     -e RECEIVER_IP=10.0.0.5 -e THREADS=4 -e EXPECTED_STREAMS=3 \
     -e METRICS_PORT=9465 \
     coda-fb:v1.1.0
   curl -s localhost:9465/metrics | grep coda_fb_ready    # quick check on the host
   ```

   With `docker/docker-compose.yml`, put `METRICS_PORT=9465` in `coda-fb.env` instead.

4. **Start Prometheus and Grafana** on the remote host, listening on localhost only:

   ```bash
   cd ~/coda-fb-monitoring/stack          # prometheus.yml already targets localhost:9465
   PROMETHEUS_LISTEN=127.0.0.1:9090 GRAFANA_LISTEN_ADDR=127.0.0.1 \
   GRAFANA_ADMIN_PASSWORD='choose-one' docker compose up -d
   ```

5. **Open the tunnel** from your machine and browse:

   ```bash
   ssh -N -L 3000:localhost:3000 -L 9090:localhost:9090 user@remote
   # through a gateway: ssh -N -J user@gateway -L 3000:localhost:3000 -L 9090:localhost:9090 user@remote
   ```

   Open <http://localhost:3000> and log in as `admin` with the password from step 4. The home page
   is the **coda-fb pipeline** dashboard, refreshing every 10 s. <http://localhost:9090> is
   Prometheus (targets, alerts, queries). If a port is busy on your laptop, tunnel another one,
   e.g. `-L 13000:localhost:3000`.

The stack keeps recording while you are disconnected (30 days of history). For several coda-fb
hosts, run the stack once on a monitoring host instead, start each coda-fb with `METRICS_BIND`
set to an address that host can reach (firewalled to it), and list them in
`doc/monitoring/stack/prometheus.yml`. Details, the Docker Desktop variant, a loopback demo
without EJFAT and a screenshot of the dashboard are in [`doc/monitoring.md`](doc/monitoring.md),
sections 6.1–6.5.

## Testing

```bash
meson setup builddir && meson test -C builddir --print-errorlogs
```

Tests are not built by `meson compile`; `meson test` builds them. See [`test/README.md`](test/README.md)
for what each test covers and for the procedure that needs live EJFAT load balancers.

## Known quirks

These are current code behaviors, documented here so the flags are not misread:

- **`-v` means `--novalidate`,** not verbose. Use `--verbose-frames` or `--verbose-reassemble` for
  verbosity.
- **`--withcp=0` and `--send-nocp` are for debugging.** Without the control plane nothing is
  registered with the load balancers.
- **`--cores` silently overrides `--threads`.**

## Behavior changes in the forwarding release

Besides the new forwarding, these existing behaviors changed; each was needed for correct
statistics or a safe shutdown:

- `--uri`/`-u` is now a deprecated alias of `--recv-ejfat-uri`; `--withcp=0` now works.
- Startup order is sender → frame builder → receiver (was receiver first), and every startup
  failure undoes what was set up. A failed `openAndStart()` after a successful worker
  registration now deregisters the worker.
- With ET output enabled, *Frames Built* used to be counted twice per frame, and the reported
  slice count was doubled. Both are fixed. As a consequence the EVIO-6 record number (word 1) is
  now 1, 2, 3, … per builder thread in every output mode; with ET enabled it used to advance by 2.
- Builder threads are now joined at shutdown (previously they were always detached after 1 s).
  A built frame is still forwarded when shutdown begins; ET/file output keep their previous
  shutdown checks.
- Timeout bookkeeping for frames finalized from lagging streams is now released (it used to leak
  one entry per such frame), and the startup entries are re-keyed to corrected event numbers.
- The final statistics used to say "Reassembly-Only" in frame-building mode and could be one
  report interval stale; rates were averages since process start labelled as current rates, and
  MiB were labelled MB.
- Error messages and the container log no longer print EJFAT tokens.

## License

MIT License - Copyright (c) 2024 Jefferson Science Associates
