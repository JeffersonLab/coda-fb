# CODA Frame Builder

Multi-threaded frame aggregator for Jefferson Lab CODA DAQ. Receives UDP packets from EJFAT load balancers, reassembles frames, aggregates by timestamp, and outputs EVIO6 format to ET systems or files.

## Dependencies

**Required:**
- C++17 compiler (GCC 8+, Clang 6+)
- Meson ≥0.55, Ninja
- E2SAR library (with pkg-config)
- Boost ≥1.83.0 (system, program_options, chrono, thread, filesystem, url)
- gRPC++ ≥1.51.1
- Protocol Buffers
- GLib 2.0

**Optional:**
- ET library (for ET output)

**Install dependencies (Ubuntu/Debian):**
```bash
sudo apt install libboost-all-dev libgrpc++-dev libprotobuf-dev libglib2.0-dev pkg-config meson ninja-build
```

## Build

```bash
# Setup
meson setup builddir --buildtype=release

# Compile
meson compile -C builddir

# Install
meson install -C builddir  # installs to $CODA/Linux-x86_64/bin or ~/.local/bin
```

**Outputs:** `coda-fb` and `evio_event_parser` executables

## Usage

### coda-fb (Frame Builder)

**Basic (reassembly only):**
```bash
coda-fb --uri 'ejfat://token@host:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 --port 10000 \
  --threads 4 --output-dir /data/raw
```

**Frame building to file:**
```bash
coda-fb --uri 'ejfat://...' --ip 192.168.1.100 --port 10000 \
  --enable-framebuild=1 --fb-output-dir /data/frames \
  --fb-threads 8 --threads 4 --expected-streams 8
```

**Frame building to ET:**
```bash
coda-fb --uri 'ejfat://...' --ip 192.168.1.100 --port 10000 \
  --enable-framebuild=1 --et-file /tmp/et_sys --et-host localhost \
  --fb-threads 8 --threads 4
```

**Current test run CL for 3 ROC configuration:**
```bash
./coda-fb -u "$EJFAT_URI" -v --withcp --ip 129.57.109.231 --threads 4 --enable-framebuild=1 --expected-streams=3 --fb-threads 1 --fb-output-dir $CODA_DATA
```

**Key options:**
- `--uri`: EJFAT control plane URI (required)
- `--ip`, `--port`: Local IP and starting port
- `--threads N`: Parallel UDP receiver threads (default: 1)
- `--enable-framebuild`: Enable aggregation (default: false)
- `--fb-threads M`: Parallel builder threads (default: 1)
- `--fb-output-dir`: Output directory for EVIO6 files
- `--et-file`: ET system file path
- `--expected-streams N`: Expected data streams for aggregation

### evio_event_parser (Validator)

**Validate EVIO6 file:**
```bash
evio_event_parser frames_thread0_file0000.evio
```

**Inspect structure:**
```bash
evio_event_parser frames_thread0_file0000.evio --verbose
```

**Exit codes:** 0 = valid, 1 = invalid

**Validates:**
- EVIO6 headers (file/record magic numbers 0x4556494F, 0xC0DA0100)
- Streaming physics event format (tags 0xFF60, 0xFF31, 0x32, 0x42)
- Length consistency

## Architecture

```
UDP Packets → [N Receiver Threads] → [M Builder Threads] → ET / Files
              (E2SAR reassembly)     (EVIO6 aggregation)
```

## License

MIT License - Copyright (c) 2024 Jefferson Science Associates
