# E2SAR Standalone Receiver

A standalone C++ program that receives UDP packets from EJFAT load balancers, reconstructs fragmented frames, and persists them to files. This program is independent of the E2SAR package but links against the installed E2SAR libraries.

## Overview

The E2SAR Standalone Receiver:

1. **Registers** with the EJFAT Control Plane using hostname
2. **Receives** segmented UDP packets from EJFAT load balancers  
3. **Reconstructs** fragmented packets into complete event frames
4. **Persists** each frame as a separate file using atomic writes with memory-mapped I/O
5. **Reports** statistics on performance and errors

## Features

- **High Performance**: Multi-threaded receiver with configurable CPU core binding
- **Atomic File Writes**: Uses temporary files and atomic renames to prevent corruption
- **Memory-Mapped I/O**: Efficient file writing using mmap for large frames
- **NUMA Awareness**: Optional memory binding to specific NUMA nodes
- **Robust Error Handling**: Comprehensive error checking and graceful degradation
- **Real-time Statistics**: Periodic reporting of receiver performance
- **Signal Handling**: Graceful shutdown with proper control plane deregistration

## Prerequisites

### System Requirements

- C++17 compatible compiler (GCC 8+, Clang 6+)
- E2SAR library installed on the system
- Boost libraries (≥1.83.0, ≤1.86.0)
- gRPC++ (≥1.51.1)
- Protocol Buffers
- GLib 2.0

### Install Dependencies

**macOS (using Homebrew):**
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

### E2SAR Installation

Ensure E2SAR is installed and available. The build system will look for:
- E2SAR headers in system include paths
- E2SAR library (`libe2sar`) in system library paths
- E2SAR pkg-config file (`e2sar.pc`)

## Building

### 1. Configure Build

```bash
cd /Users/gurjyan/Documents/Devel/e2sar_receiver
meson setup builddir
```

**For debug builds:**
```bash
meson setup --buildtype=debug builddir
```

**For release builds:**
```bash
meson setup --buildtype=release builddir
```

### 2. Compile

```bash
meson compile -C builddir
```

### 3. Install (Optional)

```bash
meson install -C builddir
```

The executable will be installed to `/usr/local/bin/e2sar_receiver` by default.

### 4. Custom Install Location

```bash
meson setup --prefix=/path/to/install builddir
meson compile -C builddir
meson install -C builddir
```

## Usage

### Command Line Options

```bash
./builddir/e2sar_receiver --help
```

### Required Parameters

- `--uri, -u`: EJFAT URI for control plane connection
- `--output-dir, -o`: Directory to save received frames

### Network Parameters

- `--ip`: IP address for receiving UDP packets (conflicts with `--autoip`)
- `--port, -p`: Starting UDP port number (default: 10000)  
- `--autoip`: Auto-detect IP address from EJFAT URI (conflicts with `--ip`)

### File Output Parameters

- `--prefix`: Filename prefix for output files (default: 'frame')
- `--extension, -e`: File extension for output files (default: '.bin')

### Performance Parameters

- `--threads, -t`: Number of receiver threads (default: 1)
- `--bufsize, -b`: Socket buffer size in bytes (default: 3MB)
- `--timeout`: Event reassembly timeout in milliseconds (default: 500)

### Control Plane Parameters

- `--withcp, -c`: Enable control plane interactions (default: true)
- `--ipv6, -6`: Prefer IPv6 for control plane connections
- `--novalidate, -v`: Don't validate TLS certificates

### Advanced Parameters

- `--cores`: List of CPU cores to bind receiver threads to
- `--numa`: Bind memory allocation to specific NUMA node
- `--report-interval`: Statistics reporting interval in milliseconds (default: 5000)

## Examples

### Basic Usage

```bash
e2sar_receiver \
  --uri 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --output-dir /path/to/frames
```

### With Custom File Naming

```bash
e2sar_receiver \
  --uri 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --output-dir /data/events \
  --prefix experiment_001 \
  --extension .dat
```

### High Performance Configuration

```bash
e2sar_receiver \
  --uri 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --output-dir /fast/storage \
  --threads 8 \
  --cores 0 1 2 3 4 5 6 7 \
  --numa 0 \
  --bufsize 16777216
```

### Auto IP Detection

```bash
e2sar_receiver \
  --uri 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --autoip \
  --output-dir /path/to/frames
```

## Output Files

### File Naming Convention

Files are named using the pattern: `{prefix}_{eventNum}_{dataId}{extension}`

Examples:
- `frame_12345_4321.bin`
- `experiment_001_98765_1234.dat`

### Atomic Writes

Files are written atomically:
1. Create temporary file with `.` prefix (e.g., `.frame_12345_4321.bin`)
2. Write frame data using memory-mapped I/O
3. Atomically rename to final filename

This prevents partial or corrupted files from appearing in the output directory.

## Monitoring

### Statistics Output

The program periodically outputs statistics:

```
=== Statistics Report ===
Frames Received: 1250
Frames Written: 1248
Write Errors: 0
Receive Errors: 2
Events Lost (reassembly): 0
Events Lost (enqueue): 0
Data Errors: 0
gRPC Errors: 0
=========================
```

### Final Statistics

On exit (Ctrl+C), detailed statistics are printed including per-port fragment counts.

## Troubleshooting

### Build Issues

**E2SAR library not found:**
```bash
# Ensure E2SAR is installed and PKG_CONFIG_PATH includes e2sar.pc
export PKG_CONFIG_PATH=/path/to/e2sar/lib/pkgconfig:$PKG_CONFIG_PATH
```

**Boost not found:**
```bash
# Set BOOST_ROOT if Boost is installed in non-standard location
export BOOST_ROOT=/path/to/boost
```

### Runtime Issues

**Permission denied on output directory:**
```bash
mkdir -p /path/to/frames
chmod 755 /path/to/frames
```

**UDP port binding errors:**
```bash
# Check if ports are available
sudo netstat -ulpn | grep :10000
```

**Control plane connection issues:**
```bash
# Test network connectivity to control plane
telnet ctrl-plane-host 18347
```

## Performance Tuning

### For High Throughput

1. **Increase socket buffer size**: `--bufsize 33554432` (32MB)
2. **Use multiple threads**: `--threads 4` or more
3. **Pin to specific CPU cores**: `--cores 0 1 2 3`
4. **Use NUMA binding**: `--numa 0`
5. **Fast storage**: Use SSD or NVMe for output directory

### For Low Latency

1. **Reduce reassembly timeout**: `--timeout 100`
2. **Pin to dedicated cores**: `--cores 2 3` (isolated from OS)
3. **Disable CPU frequency scaling**
4. **Use tmpfs for temporary storage**

## Architecture

The receiver consists of:

- **Main Thread**: Command line processing, initialization, and coordination
- **Receiver Threads**: UDP packet reception and frame reassembly (E2SAR library)
- **Write Thread**: Frame processing and file writing (main thread)
- **Statistics Thread**: Periodic performance reporting
- **Signal Handler**: Graceful shutdown and cleanup

## License

This standalone receiver program is provided as an example of using E2SAR libraries. 
License terms follow those of the E2SAR project.

## Support

For issues related to:
- **E2SAR library functionality**: Refer to E2SAR project documentation
- **This standalone receiver**: Check command line options and configuration
- **Performance tuning**: See performance tuning section above

## Version History

- **v1.0.0**: Initial release with full functionality