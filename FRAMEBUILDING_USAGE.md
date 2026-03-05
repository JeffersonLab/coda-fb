# Frame Building Usage Guide

## Overview

The CODA Frame Builder (`coda-fb`) supports two operating modes:

1. **Reassembly-Only Mode** (DEFAULT): Receives UDP packets, reassembles them into frames, and writes raw frames to a file
2. **Frame Building Mode** (OPTIONAL): Additionally aggregates frames by timestamp and builds EVIO-6 formatted output

## Mode Selection

Use the `--enable-framebuild` flag to control the mode:

```bash
--enable-framebuild=0    # Default: Reassembly-only mode
--enable-framebuild=1    # Enable frame building/aggregation
```

**Default behavior**: Reassembly-only mode (framebuilding disabled)

## Multithreading

Both modes support multithreading for high performance:

### Reassembly Threading
- Controlled by `--threads N` (default: 1)
- Creates N parallel UDP receiver threads
- Each thread handles a separate UDP port
- Example: `--threads 4` creates 4 parallel receiver threads

### Frame Building Threading (when enabled)
- Controlled by `--fb-threads M` (default: 1)
- Creates M parallel frame builder threads
- Frames are distributed across threads by timestamp hash
- Each thread has its own ET attachment and output file
- Example: `--fb-threads 4` creates 4 parallel builder threads

## Usage Examples

### 1. Default: Reassembly-Only Mode (Multithreaded)

Receive UDP packets, reassemble frames, write raw frames to file with 4 parallel receiver threads:

```bash
./coda-fb \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --threads 4 \
  --output-dir /data/raw \
  --prefix events
```

**Output**: Raw reassembled frames written to `/data/raw/events.bin`

**Threading**: 4 parallel UDP receiver threads (ports 10000-10003)

### 2. Frame Building with File Output

Enable frame building with EVIO-6 aggregation and 2GB auto-rollover:

```bash
./coda-fb \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --threads 4 \
  --enable-framebuild=1 \
  --fb-output-dir /data/frames \
  --fb-output-prefix aggregated \
  --fb-threads 2 \
  --expected-streams 8
```

**Output**: EVIO-6 formatted aggregated frames in `/data/frames/aggregated_thread{N}_file{M}.evio`

**Threading**:
- 4 parallel UDP receiver threads (reassembly)
- 2 parallel frame builder threads (aggregation)
- Total: 6 active threads plus main thread

### 3. Frame Building with ET Output

Send aggregated frames to an ET system:

```bash
./coda-fb \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --threads 8 \
  --enable-framebuild=1 \
  --et-file /tmp/et_sys_pagg \
  --et-host localhost \
  --et-port 11111 \
  --fb-threads 4 \
  --expected-streams 8
```

**Output**: Frames sent to ET system at `/tmp/et_sys_pagg` on `localhost:11111`

**Threading**:
- 8 parallel UDP receiver threads
- 4 parallel frame builder threads (each with own ET attachment)
- Total: 12 active threads

### 4. Dual Output (ET + File Backup)

Send frames to ET system AND write backup files:

```bash
./coda-fb \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --threads 4 \
  --enable-framebuild=1 \
  --et-file /tmp/et_sys_pagg \
  --fb-output-dir /data/backup \
  --fb-output-prefix backup \
  --fb-threads 2
```

**Output**:
- Frames sent to ET system
- Backup files in `/data/backup/backup_thread{N}_file{M}.evio`

## Testing the Implementation

### Test 1: Verify Default is Reassembly-Only

```bash
# Run without --enable-framebuild flag
./coda-fb -u 'ejfat://...' --ip 192.168.1.100 --port 10000 \
          --output-dir /tmp/test --prefix test --threads 2

# Expected output should show:
# - "Frame builder: DISABLED (reassembly-only mode)"
# - "Reassembly threads: 2 (parallel UDP receivers)"
# - Statistics showing "Mode: Reassembly-Only"
```

### Test 2: Verify Framebuilding Only When Enabled

```bash
# Run with --enable-framebuild=1
./coda-fb -u 'ejfat://...' --ip 192.168.1.100 --port 10000 \
          --enable-framebuild=1 --fb-output-dir /tmp/test \
          --fb-threads 3 --threads 2

# Expected output should show:
# - "Frame builder: ENABLED"
# - "Reassembly threads: 2 (parallel UDP receivers)"
# - "Builder Threads: 3"
# - Statistics showing "Mode: Frame Building"
```

### Test 3: Verify Multithreading

```bash
# Run with high thread counts
./coda-fb -u 'ejfat://...' --ip 192.168.1.100 --port 10000 \
          --threads 8 --enable-framebuild=1 \
          --fb-output-dir /tmp/test --fb-threads 4

# Monitor with:
# - Check process thread count: ps -eLf | grep coda-fb | wc -l
# - Should show >12 threads (8 receivers + 4 builders + main + stats)
# - Check CPU usage across multiple cores: htop or top
```

### Test 4: Verify Thread Safety (Stress Test)

```bash
# Run with high concurrency and monitor for data races
./coda-fb -u 'ejfat://...' --ip 192.168.1.100 --port 10000 \
          --threads 16 --enable-framebuild=1 \
          --fb-output-dir /tmp/stress --fb-threads 8

# Run for extended period and verify:
# - No crashes or hangs
# - Statistics are consistent (no negative numbers, no missing frames)
# - All output files are valid EVIO-6 format
# - Clean shutdown with Ctrl+C (all threads join properly)
```

## Performance Comparison

### Reassembly-Only Mode
- **Pros**: Lower latency, simpler code path, no aggregation overhead
- **Cons**: No frame aggregation, no EVIO-6 formatting
- **Use case**: Raw data collection, single-stream scenarios

### Frame Building Mode
- **Pros**: EVIO-6 formatted output, multi-stream aggregation, 2GB auto-rollover
- **Cons**: Higher CPU usage, additional latency for aggregation
- **Use case**: Multi-stream DAQ, downstream processing requiring EVIO-6

## Command-Line Options Reference

### Required Options
- `-u, --uri`: EJFAT URI for control plane connection

### Mode Selection
- `--enable-framebuild`: Enable frame building (default: false)

### Output Options (Reassembly-Only Mode)
- `--output-dir`: Output directory for raw frames
- `--prefix`: Filename prefix (default: events)
- `--extension`: File extension (default: .bin)

### Output Options (Frame Building Mode)
- `--et-file`: ET system file (empty to disable ET output)
- `--et-host`: ET system host (default: broadcast)
- `--et-port`: ET system port (default: ET default port)
- `--fb-output-dir`: Frame builder output directory
- `--fb-output-prefix`: Output file prefix (default: frames)

### Threading Options
- `--threads`: Number of reassembly threads (default: 1)
- `--fb-threads`: Number of frame builder threads (default: 1)

### Frame Building Options
- `--expected-streams`: Expected number of data streams (default: 1)
- `--timestamp-slop`: Max timestamp difference in ticks (default: 100)
- `--frame-timeout`: Frame timeout in ms (default: 1000)

## Implementation Notes

### Thread Safety
- All shared state is protected by mutexes or atomics
- Lock-free frame distribution via timestamp hashing
- Each builder thread has independent ET attachment
- File writes are mutex-protected

### Data Flow
```
UDP Packets (parallel ports)
    ↓
[Reassembler - N threads]     ← E2SAR library (parallel UDP receivers)
    ↓
[Main reception loop - 1 thread]
    ↓
[Frame Builder - M threads]   ← Parallel EVIO-6 construction
    ↓
ET System / Files
```

### Shutdown Behavior
- Ctrl+C triggers graceful shutdown
- Reassembler stops first (no more data ingress)
- Frame builder processes remaining buffered frames
- All threads join within 1 second timeout
- Files are flushed and closed
- Statistics are printed

## Troubleshooting

### Issue: "Frame builder requires at least one output mode"
**Solution**: When using `--enable-framebuild=1`, you must specify either `--et-file` or `--fb-output-dir`

### Issue: "Reassembly-only mode requires --output-dir"
**Solution**: When NOT using frame building (default), you must specify `--output-dir`

### Issue: Threads not joining on shutdown
**Solution**: This is handled automatically - threads are forcibly detached after 1s timeout to prevent hangs

### Issue: Low throughput with single thread
**Solution**: Increase `--threads` for reassembly and `--fb-threads` for frame building

### Issue: High CPU usage
**Solution**: Reduce thread counts or disable frame building if not needed
