# CODA Frame Builder (coda-fb)

A high-performance, multi-threaded frame aggregator for Jefferson Lab's CODA Data Acquisition system. Receives UDP packets from EJFAT load balancers, reassembles fragmented frames, aggregates multi-stream data by timestamp, and outputs EVIO-6 formatted events to ET systems or files.

**Key Features:**
- Multi-threaded UDP reception with E2SAR reassembly
- Timestamp-synchronized frame aggregation across multiple data streams
- EVIO-6 compliant output (compatible with EMU PAGG)
- Flexible output: ET system, file with 2GB auto-rollover, or dual mode
- Lock-free parallel builder threads for high throughput
- NUMA-aware with CPU affinity support

---

## Table of Contents

- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Usage](#usage)
  - [Command-Line Options](#command-line-options)
  - [Output Modes](#output-modes)
- [Configuration](#configuration)
  - [ET Connection Modes](#et-connection-modes)
  - [ET System Setup](#et-system-setup)
  - [Threading and Performance](#threading-and-performance)
- [Troubleshooting](#troubleshooting)
- [Architecture & Notes](#architecture--notes)
- [License](#license)

---

## Prerequisites

### System Requirements

- **OS**: Linux (RHEL/CentOS/Ubuntu/Debian) or macOS
- **Compiler**: C++17 compatible (GCC 8+, Clang 6+)
- **Build System**: Meson ≥0.55, Ninja

### Required Dependencies

- **E2SAR library** - EJFAT reassembly library (must be installed with pkg-config support)
- **Boost** ≥1.83.0, ≤1.86.0 (system, program_options, chrono, thread, filesystem, url)
- **gRPC++** ≥1.51.1
- **Protocol Buffers**
- **GLib 2.0**

### Optional Dependencies

- **ET library** - For ET system output (frame builder mode)
- **NUMA library** - For NUMA-aware memory allocation
- **CPU affinity support** - For binding threads to specific cores

### Install Dependencies

**macOS (Homebrew):**
```bash
brew install boost grpc protobuf glib pkg-config meson ninja
```

**Ubuntu/Debian:**
```bash
sudo apt install libboost-all-dev libgrpc++-dev libprotobuf-dev libglib2.0-dev
sudo apt install pkg-config meson ninja-build
```

**RHEL/CentOS/Fedora:**
```bash
sudo dnf install boost-devel grpc-devel protobuf-devel glib2-devel
sudo dnf install pkg-config meson ninja-build
```

### E2SAR Library Installation

Ensure E2SAR is installed and pkg-config can find it:
```bash
pkg-config --modversion e2sar
# If not found, set PKG_CONFIG_PATH:
export PKG_CONFIG_PATH=/path/to/e2sar/lib/pkgconfig:$PKG_CONFIG_PATH
```

### ET Library Installation (Optional)

For frame builder ET output support:
```bash
# Clone and build ET library from Jefferson Lab
git clone https://github.com/JeffersonLab/et
cd et && mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make && sudo make install
```

---

## Installation

### Build from Source

```bash
# Clone repository
cd /path/to/coda-fb

# Build (release mode)
./scripts/build.sh

# Build with options
./scripts/build.sh --type debug --clean

# Build and install
./scripts/build.sh --install --prefix /opt
```

**Manual build:**
```bash
meson setup builddir --buildtype=release
meson compile -C builddir
meson install -C builddir  # optional
```

### Installation Directories

The build system automatically detects installation location:

1. **CODA environment** (if `$CODA` set): `$CODA/Linux-x86_64/bin`
2. **Custom prefix** (if specified): `<prefix>/bin`
3. **User local** (default): `~/.local/bin`

**Override installation:**
```bash
meson setup --prefix=/custom/path builddir
```

### Verify Build

```bash
# Test executable
./builddir/coda-fb --help

# Check version
./builddir/coda-fb --version
```

---

## Quick Start

### Basic File Output

Simplest mode - writes raw reassembled frames to a single file:

```bash
coda-fb \
  --uri 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --output-dir /data/output \
  --prefix run1234
```

**Output:** `/data/output/run1234.dat` (single file, raw frames)

### Frame Builder with ET Output

Aggregate frames and send to ET system for real-time processing:

```bash
# Start ET system first
et_start -f /tmp/et_sys_coda -n 1000 -s 2000000

# Run frame builder
coda-fb \
  --uri 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --et-file /tmp/et_sys_coda \
  --et-station CODA_PAGG \
  --fb-threads 4
```

**Output:** EVIO-6 frames sent to ET station `CODA_PAGG`

### Frame Builder with File Output

Aggregate frames and write to files with 2GB auto-rollover:

```bash
coda-fb \
  --uri 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --fb-output-dir /data/frames \
  --fb-output-prefix clas12_run1234 \
  --fb-threads 4
```

**Output:** Multiple EVIO-6 files:
```
/data/frames/clas12_run1234_thread0_file0000.evio
/data/frames/clas12_run1234_thread0_file0001.evio  (after 2GB)
/data/frames/clas12_run1234_thread1_file0000.evio
...
```

### Dual Output (ET + File)

Real-time processing via ET, permanent archival to files:

```bash
coda-fb \
  --uri 'ejfat://...' \
  --ip 192.168.1.100 \
  --et-file /tmp/et_sys_coda \
  --et-station CODA_PAGG \
  --fb-output-dir /mnt/archive/run1234 \
  --fb-output-prefix run1234 \
  --fb-threads 8
```

**Output:** Both ET system (real-time) + archive files (permanent)

---

## Usage

### Command-Line Options

#### Basic Options

```
--uri, -u <uri>              EJFAT URI for control plane connection (required)
--ip <address>               IP address for receiving UDP packets
--port, -p <port>            Starting UDP port (default: 10000)
--autoip                     Auto-detect IP from system interfaces
--version                    Show version and exit
--help                       Show help message
```

#### Basic File Output (Legacy Mode)

```
--output-dir, -o <dir>       Output directory for raw frames
--prefix <name>              Filename prefix (default: events)
--extension, -e <ext>        File extension (default: .bin)
```

**Note:** Basic mode writes one raw frame per line to a single file. For production use, prefer frame builder mode.

#### Frame Builder Options

```
ET Output:
  --et-file <file>           ET system file name (e.g., /tmp/et_sys_coda)
  --et-host <host>           ET host (empty for local, hostname, or IP)
  --et-port <port>           ET port (0 for default, typically 11111)
  --et-station <name>        ET station name (default: GRAND_CENTRAL)
  --et-event-size <bytes>    Max ET event size (default: 2MB)

File Output:
  --fb-output-dir <dir>      Frame builder output directory
  --fb-output-prefix <name>  File prefix (default: frames)

Frame Building:
  --fb-threads <count>       Builder threads (default: 4, range: 1-32)
  --timestamp-slop <ticks>   Max timestamp difference (default: 100)
  --frame-timeout <ms>       Incomplete frame timeout (default: 1000)
  --expected-streams <n>     Expected data streams for aggregation
```

**Important:** Frame builder requires at least ONE output mode (ET or file). Both can be enabled for dual output.

#### Performance Options

```
--threads, -t <count>        Receiver threads (default: 1)
--bufsize, -b <bytes>        Socket buffer size (default: 3MB)
--timeout <ms>               Reassembly timeout (default: 500ms)
--cores <list>               CPU cores for thread binding (e.g., 0 1 2 3)
--numa <node>                NUMA node for memory allocation
--report-interval <ms>       Statistics interval (default: 5000ms)
```

#### Control Plane Options

```
--withcp, -c                 Enable control plane (default: true)
--ipv6, -6                   Prefer IPv6 for control plane
--novalidate, -v             Skip TLS certificate validation
```

### Output Modes

| Mode | Flags | Output | Format | Rollover | Use Case |
|------|-------|--------|--------|----------|----------|
| **Basic** | `--output-dir` | Single file | Raw frames | No | Simple recording |
| **FB File** | `--fb-output-dir` | Multiple files | EVIO-6 | 2GB auto | Production archival |
| **FB ET** | `--et-file` + `--et-station` | ET system | EVIO-6 | N/A | Real-time DAQ |
| **FB Dual** | Both above | ET + files | EVIO-6 | 2GB auto | Production (with backup) |

### Examples

**High-throughput configuration:**
```bash
coda-fb \
  --uri 'ejfat://...' \
  --ip 192.168.1.100 \
  --threads 4 \
  --fb-output-dir /data/frames \
  --fb-threads 8 \
  --bufsize 16777216 \
  --cores 0 1 2 3 4 5 6 7 8 9 10 11 \
  --numa 0
```

**Remote ET connection:**
```bash
coda-fb \
  --uri 'ejfat://...' \
  --ip 192.168.1.100 \
  --et-file /tmp/et_sys_coda \
  --et-host et-server.jlab.org \
  --et-port 11111 \
  --et-station CODA_PAGG
```

**Auto IP detection:**
```bash
coda-fb \
  --uri 'ejfat://...' \
  --autoip \
  --fb-output-dir /data/frames
```

---

## Configuration

### ET Connection Modes

The frame builder supports three ET connection modes:

#### 1. Local Broadcast (Default)

```bash
--et-file /tmp/et_sys_coda
# Empty host → uses UDP broadcast to find ET on local network
```

Automatically discovers ET system via broadcast. Suitable for same machine or local subnet.

#### 2. Direct Connection by Hostname

```bash
--et-file /tmp/et_sys_coda \
--et-host et-server.jlab.org \
--et-port 11111
```

Connects directly to specified host. Required for remote ET systems with DNS.

#### 3. Direct Connection by IP

```bash
--et-file /tmp/et_sys_coda \
--et-host 192.168.100.50 \
--et-port 11111
```

Connects directly to IP address. Useful when DNS unavailable or for explicit control.

**ET Station Configuration:**

The ET station must be configured for **PARALLEL** mode to support multiple builder threads. Each builder thread creates its own attachment for lock-free operation.

### ET System Setup

Before using ET output mode, you must start an ET system. The ET system acts as a shared memory ring buffer for inter-process communication.

#### Starting ET System

**Basic startup:**
```bash
et_start -f /tmp/et_sys_coda -n 1000 -s 2000000
```

**Production startup with verbose output:**
```bash
et_start -f /tmp/et_vgexpid_ERSAP -v -d -n 1000 -s 1000000 -p 23911
```

#### ET Command-Line Options

```
-f <filename>        ET system file name (required)
                     This is a logical name, not a filesystem path
                     Example: /tmp/et_sys_coda, /tmp/et_vgexpid_ERSAP

-n <events>          Number of events in the ET system (default: 100)
                     More events = more buffering capacity
                     Typical: 1000-10000 for production

-s <size>            Event size in bytes (default: 1MB)
                     Must accommodate largest expected frame
                     Typical: 1000000 (1MB) to 10000000 (10MB)

-p <port>            TCP server port (default: auto-select)
                     Required for remote connections
                     Typical: 11111, 23911, or custom

-v                   Verbose output (print configuration details)

-d                   Run as daemon (detach from terminal)

-g <groups>          Number of event groups (default: 1)

-m                   Enable multicast for remote discovery
```

#### ET System Sizing Guidelines

**Event count (-n):**
- Small systems: 100-500 events
- Medium systems: 1000-2000 events
- Large systems: 5000-10000 events
- Rule of thumb: 10-20× the number of concurrent producers/consumers

**Event size (-s):**
- Must be larger than the largest expected aggregated frame
- Include overhead: add 20-30% to max frame size
- Examples:
  - `-s 1000000` (1 MB) for small frames
  - `-s 2000000` (2 MB) for typical CODA frames
  - `-s 10000000` (10 MB) for large multi-stream aggregations

**Port selection (-p):**
- Use explicit port for remote connections
- Common ports: 11111, 23911, or any available port > 1024
- Omit for local-only connections (uses broadcast discovery)

#### Example Configurations

**Local development:**
```bash
# Small local ET for testing
et_start -f /tmp/et_test -v -n 500 -s 1000000
```

**Production DAQ:**
```bash
# Large ET system for high-rate data acquisition
et_start -f /tmp/et_coda_prod -v -d -n 5000 -s 5000000 -p 11111
```

**Remote ET server:**
```bash
# ET system accessible from remote hosts
et_start -f /tmp/et_sys_remote -v -d -n 2000 -s 2000000 -p 23911 -m
```

#### Monitoring ET System

Check ET system status:
```bash
et_monitor -f /tmp/et_sys_coda
```

Monitor with refresh:
```bash
et_monitor -f /tmp/et_sys_coda -p 1  # Update every 1 second
```

#### Stopping ET System

```bash
# Graceful shutdown (after consumers disconnect)
et_kill -f /tmp/et_sys_coda

# Force shutdown
killall et_start
```

**Note:** Always stop coda-fb and other consumers before killing the ET system to avoid losing data in flight.

### Threading and Performance

#### Receiver Threads

```bash
--threads 4        # 4 receiver threads on ports 10000-10003
--cores 0 1 2 3    # Bind to specific CPU cores
```

**Recommendations:**
- Start with 1-2 threads
- Increase if packet loss occurs
- Use CPU affinity for deterministic performance

#### Builder Threads

```bash
--fb-threads 8     # 8 parallel builder threads
```

**Throughput scaling:**
- 1-2 threads: Low data rate (< 500 MB/s)
- 4-8 threads: Medium data rate (500 MB/s - 2 GB/s)
- 8-16 threads: High data rate (2-10 GB/s)
- 16-32 threads: Very high data rate (> 10 GB/s), requires many cores

**Architecture:** Builder threads receive time slices via hash-based distribution (by timestamp). Each thread maintains its own frame buffer, builds EVIO-6 banks independently, and has its own ET attachment. This achieves near-linear throughput scaling.

#### NUMA Optimization

```bash
--numa 0           # Bind memory allocation to NUMA node 0
--cores 0-15       # Use cores on same NUMA node
```

For multi-socket systems, bind threads and memory to the same NUMA node for best performance.

#### Socket Buffer Size

```bash
--bufsize 16777216  # 16 MB socket buffer
```

Increase if experiencing packet loss at high data rates. Check system limits:
```bash
# Linux: check max socket buffer size
sysctl net.core.rmem_max
# Increase if needed
sudo sysctl -w net.core.rmem_max=67108864
```

### File Output Features

**Automatic Rollover:**
- Files automatically roll over at 2GB (2,147,483,648 bytes)
- Sequential numbering: `file0000.evio`, `file0001.evio`, etc.
- Per-thread file sequences (each builder thread maintains separate files)
- Seamless, no data loss during rollover

**File Naming:**
```
{prefix}_thread{N}_file{NNNN}.evio

Examples:
  frames_thread0_file0000.evio
  frames_thread0_file0001.evio
  frames_thread1_file0000.evio
```

**Output Directory:**
- Created automatically if doesn't exist
- Must have write permissions
- Consider fast storage (SSD/NVMe) for high data rates

---

## Troubleshooting

### Build Issues

**E2SAR library not found:**
```bash
# Check pkg-config path
pkg-config --list-all | grep e2sar

# Set path if needed
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

**Boost not found or wrong version:**
```bash
# Check Boost version
ls /usr/local/include/boost/version.hpp

# Set Boost root if needed
export BOOST_ROOT=/path/to/boost
```

**ET library not found:**
```bash
# ET is optional - disable frame builder or install ET
# Check ET installation
ls /usr/local/lib/libet.* /usr/local/include/et.h

# Set library path if needed
export LIBRARY_PATH=/usr/local/lib:$LIBRARY_PATH
export CPATH=/usr/local/include:$CPATH
```

### Runtime Issues

**Permission denied on output directory:**
```bash
mkdir -p /data/frames
chmod 755 /data/frames
chown $USER /data/frames
```

**UDP port binding errors:**
```bash
# Check if ports available
sudo netstat -ulpn | grep :10000

# Kill conflicting process or use different port
coda-fb --port 20000 ...
```

**Control plane connection issues:**
```bash
# Test connectivity to control plane
telnet ctrl-plane-host 18347

# Try without TLS validation (insecure, testing only)
coda-fb --novalidate ...
```

**ET connection failed:**
```bash
# Check ET system is running
et_monitor -f /tmp/et_sys_coda

# Verify ET file name matches
et_start -f /tmp/et_sys_coda -n 1000 -s 2000000

# Check network connectivity for remote ET
telnet et-server.jlab.org 11111
```

**Frame builder requires at least one output mode:**
```bash
# Error: no output specified
coda-fb --uri '...' --ip 192.168.1.100

# Fix: add ET output
coda-fb --uri '...' --ip 192.168.1.100 \
  --et-file /tmp/et_sys_coda --et-station CODA_PAGG

# Or: add file output
coda-fb --uri '...' --ip 192.168.1.100 \
  --fb-output-dir /data/frames

# Or: add both
coda-fb --uri '...' --ip 192.168.1.100 \
  --et-file /tmp/et_sys_coda --et-station CODA_PAGG \
  --fb-output-dir /data/frames
```

**Disk full during file output:**
```bash
# Monitor disk space
df -h /data/frames

# Calculate expected usage
# Example: 1 GB/s data rate × 3600s run = 3.6 TB/hour
```

**Payload validation errors:**

If seeing "Skipping frame due to invalid payload" errors:
- Check EVIO payload format from data source
- Verify magic number (0xc0da0100 for CODA block header)
- Check endianness (payload parser auto-corrects but logs warning)
- Ensure data source sends complete frames

### Performance Issues

**Packet loss / missed frames:**
```bash
# Increase socket buffer
--bufsize 33554432  # 32 MB

# Add more receiver threads
--threads 4

# Check system UDP buffer limits
sysctl net.core.rmem_max
```

**Low throughput:**
```bash
# Increase builder threads
--fb-threads 16

# Use CPU affinity
--cores 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15

# Use NUMA binding
--numa 0

# Use faster storage for file output
# Mount NVMe/SSD at /data/frames
```

**High CPU usage:**
```bash
# Reduce threads if oversubscribed
--threads 2 --fb-threads 4

# Spread across NUMA nodes
--numa -1  # disable NUMA binding
```

---

## Architecture & Notes

### System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        coda-fb Process                        │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌──────────────────────────────────────────────────────┐  │
│  │         EJFAT Control Plane (gRPC)                    │  │
│  │  - Worker registration                                 │  │
│  │  - Load balancer configuration                        │  │
│  └──────────────────────────────────────────────────────┘  │
│                            │                                  │
│  ┌─────────────────────────▼──────────────────────────────┐ │
│  │       Receiver Threads (E2SAR Reassembler)             │ │
│  │  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐       │ │
│  │  │ Port   │  │ Port   │  │ Port   │  │ Port   │       │ │
│  │  │ 10000  │  │ 10001  │  │ 10002  │  │ 10003  │  ...  │ │
│  │  └────────┘  └────────┘  └────────┘  └────────┘       │ │
│  │       ▲          ▲          ▲          ▲               │ │
│  └───────┼──────────┼──────────┼──────────┼───────────────┘ │
│          │          │          │          │                  │
│       UDP Packets (fragmented from EJFAT Load Balancer)     │
│                                                               │
│  ┌──────────────────▼──────────────────────────────────┐   │
│  │           Payload Validation                         │   │
│  │  - Parse EVIO structure                              │   │
│  │  - Verify magic number (0xc0da0100)                  │   │
│  │  - Extract timestamp, frame number, ROC ID           │   │
│  │  - Auto-correct endianness if needed                 │   │
│  └──────────────────┬──────────────────────────────────┘   │
│                     │                                        │
│  ┌──────────────────▼──────────────────────────────────┐   │
│  │       Frame Builder (if enabled)                     │   │
│  │                                                        │   │
│  │  Hash by timestamp → distribute to builder threads   │   │
│  │                                                        │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐           │   │
│  │  │ Builder  │  │ Builder  │  │ Builder  │           │   │
│  │  │ Thread 0 │  │ Thread 1 │  │ Thread N │   ...     │   │
│  │  ├──────────┤  ├──────────┤  ├──────────┤           │   │
│  │  │Frame Buf │  │Frame Buf │  │Frame Buf │           │   │
│  │  │          │  │          │  │          │           │   │
│  │  │ Build    │  │ Build    │  │ Build    │           │   │
│  │  │ EVIO-6   │  │ EVIO-6   │  │ EVIO-6   │           │   │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘           │   │
│  │       │             │             │                  │   │
│  └───────┼─────────────┼─────────────┼──────────────────┘   │
│          │             │             │                       │
│  ┌───────▼─────────────▼─────────────▼──────────────────┐  │
│  │             Output Layer (parallel)                   │  │
│  │  ┌──────────────────┐    ┌──────────────────┐        │  │
│  │  │   ET System      │    │   File Output    │        │  │
│  │  │   (shared mem    │    │   (2GB rollover) │        │  │
│  │  │   or TCP)        │    │                  │        │  │
│  │  └──────────────────┘    └──────────────────┘        │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                               │
│  ┌──────────────────────────────────────────────────────┐  │
│  │         Statistics Reporting Thread                   │  │
│  │  - Periodic performance metrics                       │  │
│  │  - Frame/byte counters                                │  │
│  │  - Error tracking                                     │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                               │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

1. **Registration:** Process registers with EJFAT control plane as a worker node
2. **Reception:** Receiver threads listen on UDP ports, receive fragmented packets
3. **Reassembly:** E2SAR library reassembles UDP segments into complete data frames
4. **Validation:** EVIO payload parsed to extract timestamp, frame number, ROC ID; magic number and endianness verified
5. **Aggregation:** (Frame builder) Frames distributed to builder threads by timestamp hash; threads aggregate frames from multiple streams with matching timestamps
6. **EVIO-6 Construction:** Builder threads construct aggregated time frame banks with Stream Info Bank (SIB), Time Slice Segment (TSS), and Aggregation Info Segment (AIS)
7. **Output:** Parallel output to ET system and/or files with automatic 2GB rollover

### Threading Model

**Lock-Free Distribution:**
- Incoming slices hashed by timestamp to specific builder threads
- No contention during frame building
- Each thread has independent ET attachment
- Thread-local statistics (aggregated on stop)

**Throughput Scaling:**
- N receiver threads → N× UDP reception capacity
- M builder threads → M× frame building throughput
- Near-linear scaling up to CPU core count

### EVIO-6 Format

Frame builder outputs **EVIO-6 aggregated time frame banks** compatible with Jefferson Lab EMU PAGG format:

```
Word 0:  Total event length (32-bit words, excluding this word)
Word 1:  Tag (0xFFD0) | DataType (0x10) | Num (slice count + error bit)

Stream Info Bank (SIB):
  Word 2:  SIB length
  Word 3:  Tag (0xFFD1) | DataType (0x20) | Num

  Time Slice Segment (TSS):
    Word 4:  Tag (0x01) | padding (1) | length (3)
    Word 5:  Frame number
    Word 6:  Avg timestamp (bits 31-0)
    Word 7:  Avg timestamp (bits 63-32)

  Aggregation Info Segment (AIS):
    Word 8:   Tag (0x02) | padding (1) | length (slice count)
    Word 9+:  One word per slice: ROC_ID | reserved | stream_status

Time Slice Banks (original payloads):
  [Payload 1 from stream 1]
  [Payload 2 from stream 2]
  ...
  [Payload N from stream N]
```

**CODA Tags:**
- `0xFFD0`: STREAMING_PHYS (top-level physics event)
- `0xFFD1`: STREAMING_SIB_BUILT (Stream Info Bank - aggregated)
- `0x01`: STREAMING_TSS_BUILT (Time Slice Segment)
- `0x02`: STREAMING_AIS_BUILT (Aggregation Info Segment)

### Timestamp Synchronization

Frames from multiple data streams are synchronized by timestamp:
- All slices in an aggregated frame must have timestamps within `--timestamp-slop` ticks (default: 100)
- If timestamps diverge beyond threshold: warning logged, error bit set in header, frame still built
- Average timestamp calculated across all slices for Time Slice Segment
- Incomplete frames (missing expected streams) time out after `--frame-timeout` ms (default: 1000)

### Dependencies

**E2SAR Library:**
- Provides UDP reassembly and EJFAT control plane integration
- Must be installed with pkg-config support

**ET Library (Optional):**
- Jefferson Lab Event Transfer system for real-time inter-process communication
- Required for frame builder ET output mode
- Supports both shared memory (local) and TCP (remote) transports

**Boost:**
- Program options: command-line parsing
- Filesystem: path manipulation
- Thread/chrono: multi-threading
- ASIO: network address handling
- URL: EJFAT URI parsing

### Known Issues and Limitations

1. **Endianness:** Payload parser auto-detects and corrects wrong endianness, but logs warning. Data source should match system endianness for best performance.

2. **Frame Ordering:** Builder threads output frames independently; timestamp order not guaranteed across threads. Use single thread (`--fb-threads 1`) if strict ordering required.

3. **ET Station Mode:** ET station must be configured for PARALLEL mode to support multiple builder thread attachments.

4. **Expected Streams:** `--expected-streams` configures expected input count but missing stream detection is advisory only (logs warning, sets error bit).

5. **File Rollover:** Rollover happens at exactly 2GB; partial EVIO events at boundary will complete in current file (may slightly exceed 2GB).

### Performance Notes

**Typical Throughput:**
- 4 receiver threads + 8 builder threads: 2-5 GB/s (depends on frame size, CPU)
- 8 receiver threads + 16 builder threads: 5-10 GB/s (high-end system)

**Bottlenecks:**
- Network: 10 GbE NIC required for > 1 GB/s
- CPU: Builder threads are CPU-intensive (EVIO construction)
- Storage: File output requires SSD/NVMe for > 2 GB/s
- Memory: Large socket buffers (16-32 MB) prevent packet loss

**Optimization Tips:**
- Match receiver threads to number of UDP ports used by EJFAT load balancer
- Use NUMA binding on multi-socket systems
- Pin threads to physical cores (not hyperthreads) for deterministic latency
- Use ET output for highest throughput (no disk I/O)

---

## License

This software is developed for Jefferson Lab's CODA Data Acquisition system.

Copyright (c) 2024, Jefferson Science Associates

Licensed under the MIT License. See `LICENSE` file for details.

---

## Contact and Support

**For issues related to:**
- **E2SAR library**: Refer to E2SAR project documentation
- **ET library**: https://github.com/JeffersonLab/et
- **This frame builder**: Open an issue in the project repository

**Related Projects:**
- **EMU PAGG**: https://github.com/JeffersonLab/emu (Java reference implementation)
- **EJFAT**: LDRD streaming data acquisition project at Jefferson Lab
- **CODA**: Jefferson Lab Data Acquisition system

**Documentation:**
- Build system: Meson documentation (mesonbuild.com)
- EVIO format: CODA EVIO library documentation

---

**Version:** 1.0.0
**Last Updated:** Februry 2026

## Current command used for testing
```
coda-fb  -u $EJFAT_URI --withcp -v --ip 129.57.109.231 --et-file /tmp/et_vgexpid_ERSA
```

Note: in all commandline use options to avoid certificate validation
```
--withcp -v
```


