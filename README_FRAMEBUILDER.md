# E2SAR Frame Builder - Quick Start Guide

## What is the Frame Builder?

The E2SAR Frame Builder is an extension to the e2sar_receiver that:
- **Aggregates** reassembled frames from multiple data streams by timestamp
- **Formats** data into EVIO-6 compliant aggregated time frame banks
- **Outputs** to ET systems, files with 2GB auto-rollover, or both

## Quick Start

### 1. Check if ET library is available

```bash
cd e2sar-receiver
meson setup build
```

Look for: `ET Library: Found (frame builder enabled)`

### 2. Build

```bash
meson compile -C build
```

### 3. Run with file output

```bash
./build/e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-framebuilder \
  --fb-output-dir /tmp/frames \
  --fb-output-prefix test
```

### 4. Check output

```bash
ls -lh /tmp/frames/
# test_thread0_file0000.evio
# test_thread0_file0001.evio (after 2GB)
# test_thread1_file0000.evio
# ...
```

## Three Output Modes

### Mode 1: File Output (Archival)

Write aggregated frames to .evio files with automatic 2GB rollover:

```bash
e2sar_receiver \
  -u 'ejfat://...' \
  --ip 192.168.1.100 \
  --use-framebuilder \
  --fb-output-dir /data/frames \
  --fb-output-prefix run1234 \
  --fb-threads 4
```

**Use case:** Long-term storage, offline analysis

### Mode 2: ET Output (Real-time)

Send aggregated frames to ET system for immediate processing:

```bash
# Start ET first
et_start -f /tmp/et_sys_pagg -n 1000 -s 2000000

# Run receiver
e2sar_receiver \
  -u 'ejfat://...' \
  --ip 192.168.1.100 \
  --use-framebuilder \
  --et-file /tmp/et_sys_pagg \
  --et-station E2SAR_PAGG \
  --fb-threads 4
```

**Use case:** Real-time data processing, online monitoring

### Mode 3: Dual Output (Best of Both)

Send to ET AND archive to files simultaneously:

```bash
e2sar_receiver \
  -u 'ejfat://...' \
  --ip 192.168.1.100 \
  --use-framebuilder \
  --et-file /tmp/et_sys_pagg \
  --et-station E2SAR_PAGG \
  --fb-output-dir /data/backup \
  --fb-output-prefix backup \
  --fb-threads 4
```

**Use case:** Production (real-time + backup)

## Key Features

✅ **Multi-threaded** - Parallel frame building (configurable 1-32 threads)
✅ **EVIO-6 format** - Compliant with CODA data acquisition standards
✅ **2GB auto-rollover** - Automatic file rotation for file output
✅ **ET integration** - Multiple connection modes (local/remote)
✅ **Dual output** - ET + file simultaneously
✅ **Lock-free** - High throughput, minimal contention
✅ **Statistics** - Detailed metrics on shutdown

## Command-Line Options

### Essential Options

```bash
--use-framebuilder              # Enable frame builder
--fb-output-dir <dir>           # File output directory (file mode)
--fb-output-prefix <name>       # File prefix (default: frames)
--fb-threads <N>                # Builder threads (default: 4)
--et-file <file>                # ET system file (ET mode)
--et-station <name>             # ET station name (ET mode)
```

### Advanced Options

```bash
--et-host <host>                # ET host (empty=local, or hostname/IP)
--et-port <port>                # ET port (0=default, usually 11111)
--et-event-size <bytes>         # ET event size (default: 2MB)
--timestamp-slop <ticks>        # Max timestamp diff (default: 100)
--frame-timeout <ms>            # Frame timeout (default: 1000)
```

## Output Examples

### File Output

Files are created per-thread with automatic rollover:

```
/data/frames/run1234_thread0_file0000.evio  (2.0 GB)
/data/frames/run1234_thread0_file0001.evio  (2.0 GB)
/data/frames/run1234_thread0_file0002.evio  (1.2 GB, active)
/data/frames/run1234_thread1_file0000.evio  (2.0 GB)
/data/frames/run1234_thread1_file0001.evio  (1.8 GB, active)
...
```

- **Format:** EVIO-6 binary
- **Rollover:** Automatic at 2GB
- **Naming:** `{prefix}_thread{N}_file{NNNN}.evio`

### Statistics Output

On shutdown (Ctrl+C):

```
=== Frame Builder Statistics ===
  Builder Threads: 4
  Frames Built: 10523
  Slices Aggregated: 42092
  Build Errors: 0
  Timestamp Errors: 3
  Avg Slices/Frame: 4.0
  Files Created: 12
  Bytes Written: 24579891200 (22.88 GB)
=================================
```

## Documentation

| Document | What's In It |
|----------|--------------|
| **IMPLEMENTATION_SUMMARY.md** | Complete overview, architecture, what was built |
| **COMMAND_LINE_USAGE.md** | All command-line options, examples, troubleshooting |
| **FRAMEBUILDER_README.md** | Technical details, EVIO-6 format, configuration |
| **FILE_OUTPUT_GUIDE.md** | File output specifics, 2GB rollover, reading files |
| **ET_CONNECTION_GUIDE.md** | ET connection modes, parameters, network setup |
| **INTEGRATION_EXAMPLE.md** | Code integration examples |
| **MULTITHREADED_DESIGN.md** | Threading architecture, performance |

## Help

```bash
e2sar_receiver --help
```

Shows all options with examples.

## Common Use Cases

### 1. Production DAQ (Dual Output)

Real-time processing via ET + backup to files:

```bash
et_start -f /tmp/et_sys_clas12 -n 1000 -s 4194304
e2sar_receiver \
  -u 'ejfat://...' --ip 192.168.1.100 \
  --use-framebuilder \
  --et-file /tmp/et_sys_clas12 --et-station CLAS12_EB \
  --fb-output-dir /mnt/archive/run1234 --fb-threads 8
```

### 2. Offline Analysis (File Only)

Archive data for later analysis:

```bash
e2sar_receiver \
  -u 'ejfat://...' --ip 192.168.1.100 \
  --use-framebuilder \
  --fb-output-dir /data/runs/run5678 \
  --fb-output-prefix clas12_run5678 \
  --fb-threads 4
```

### 3. Online Monitoring (ET Only)

Send to ET for real-time monitoring:

```bash
e2sar_receiver \
  -u 'ejfat://...' --ip 192.168.1.100 \
  --use-framebuilder \
  --et-file /tmp/et_monitor --et-station MONITOR \
  --fb-threads 2
```

## Troubleshooting

### Error: "Frame builder requires at least one output mode"

**Fix:** Specify either ET or file output:
- ET: `--et-file /tmp/et_sys --et-station STATION`
- File: `--fb-output-dir /data/output`

### Error: "Failed to start frame builder"

**Check:**
1. ET system running? `et_monitor -f /tmp/et_sys_pagg`
2. Output directory writable? `ls -ld /data/frames`
3. ET host/port correct?

### Disk Full

**Monitor disk space:**
```bash
df -h /data/frames
```

**Calculate expected size:**
- Data rate × duration = total size
- Example: 1 GB/s × 3600s = 3.6 TB/hour

## Performance Tips

**Increase throughput:**
- More builder threads: `--fb-threads 8`
- Use SSD for file output
- Use ET output (no disk I/O)

**Reduce latency:**
- Fewer threads: `--fb-threads 2`
- ET output instead of files

**Balance CPU and I/O:**
- CPU-bound: Increase threads
- I/O-bound: Decrease threads or use ET

## Testing

### Test ET connection

```bash
et_start -f /tmp/et_test -n 100 -s 1000000
et_monitor -f /tmp/et_test
```

### Test file output

```bash
e2sar_receiver --use-framebuilder --fb-output-dir /tmp/test
ls -lh /tmp/test/
```

### Verify EVIO format

```bash
evio_dump /tmp/test/frames_thread0_file0000.evio
```

## Backward Compatibility

✅ Original receiver mode still works without frame builder:

```bash
e2sar_receiver \
  -u 'ejfat://...' \
  --ip 192.168.1.100 \
  --output-dir /data/output
```

Writes raw frames to single file (original behavior).

## What's Next?

1. ✅ Read **IMPLEMENTATION_SUMMARY.md** for complete overview
2. ✅ Read **COMMAND_LINE_USAGE.md** for all options
3. ✅ Read **FILE_OUTPUT_GUIDE.md** or **ET_CONNECTION_GUIDE.md** for specifics
4. ✅ Try examples above
5. ✅ Check statistics output

---

**Status:** ✅ Fully implemented and integrated
**Version:** 1.0.0
**Date:** 2024
