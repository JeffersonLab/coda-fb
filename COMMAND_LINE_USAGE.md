# E2SAR Receiver Command-Line Usage

## Overview

The e2sar_receiver now supports three output modes:
1. **Basic file output**: Write raw reassembled frames to a single file
2. **Frame builder with file output**: Aggregate frames and write EVIO-6 format with 2GB auto-rollover
3. **Frame builder with ET output**: Send aggregated frames to ET system
4. **Frame builder with dual output**: Both ET and file output simultaneously

## Command-Line Options

### Basic Options

```
--uri, -u <uri>              EJFAT URI for control plane connection (required)
--ip <address>               IP address for receiving UDP packets
--port, -p <port>            Starting UDP port number (default: 10000)
--autoip                     Auto-detect IP address from EJFAT URI
--output-dir, -o <dir>       Directory to save frames (required for basic mode)
--prefix <name>              Output filename prefix (default: events)
--extension, -e <ext>        File extension (default: .bin)
```

### Frame Builder Options

```
--use-framebuilder           Enable frame builder for aggregation

ET Output:
  --et-file <file>           ET system file name (e.g., /tmp/et_sys_pagg)
  --et-host <host>           ET host (empty for local, or hostname/IP)
  --et-port <port>           ET port (0 for default, typically 11111)
  --et-station <name>        ET station name (default: E2SAR_PAGG)
  --et-event-size <bytes>    ET event size (default: 2MB)

File Output:
  --fb-output-dir <dir>      Frame builder output directory
  --fb-output-prefix <name>  File prefix (default: frames)
  --fb-threads <count>       Number of builder threads (default: 4)

Frame Building:
  --timestamp-slop <ticks>   Max timestamp difference (default: 100)
  --frame-timeout <ms>       Frame timeout in milliseconds (default: 1000)
```

### Performance Options

```
--threads, -t <count>        Number of receiver threads (default: 1)
--bufsize, -b <bytes>        Socket buffer size (default: 3MB)
--timeout <ms>               Event reassembly timeout (default: 500ms)
--cores <list>               CPU cores to bind threads to
--numa <node>                NUMA node for memory allocation
```

## Usage Examples

### 1. Basic File Output (Original Mode)

Write all reassembled frames to a single file:

```bash
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --output-dir /data/output \
  --prefix run1234 \
  --extension .dat
```

**Output:**
- Single file: `/data/output/run1234.dat`
- Contains raw reassembled frames appended sequentially
- Closed on Ctrl+C

---

### 2. Frame Builder: File Output Only

Aggregate frames and write EVIO-6 format with automatic 2GB rollover:

```bash
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-framebuilder \
  --fb-output-dir /data/frames \
  --fb-output-prefix clas12_run1234 \
  --fb-threads 4 \
  --timestamp-slop 100 \
  --frame-timeout 1000
```

**Output:**
- Multiple files with 2GB auto-rollover:
  ```
  /data/frames/clas12_run1234_thread0_file0000.evio
  /data/frames/clas12_run1234_thread0_file0001.evio
  /data/frames/clas12_run1234_thread1_file0000.evio
  /data/frames/clas12_run1234_thread1_file0001.evio
  ...
  ```
- EVIO-6 formatted aggregated time frame banks
- One file sequence per builder thread

---

### 3. Frame Builder: ET Output Only

Send aggregated frames to ET system (local):

```bash
# Start ET system first
et_start -f /tmp/et_sys_pagg -n 1000 -s 2000000

# Run receiver
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-framebuilder \
  --et-file /tmp/et_sys_pagg \
  --et-station E2SAR_PAGG \
  --fb-threads 4
```

**Output:**
- Frames sent to ET system (shared memory)
- Station: `E2SAR_PAGG`
- Parallel attachments (one per thread)

---

### 4. Frame Builder: ET Output to Remote Host

Send to ET system on remote server:

```bash
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-framebuilder \
  --et-file /tmp/et_sys_pagg \
  --et-host et-server.jlab.org \
  --et-port 11111 \
  --et-station E2SAR_PAGG \
  --et-event-size 4194304 \
  --fb-threads 8
```

**Configuration:**
- ET host: `et-server.jlab.org:11111`
- Event size: 4MB
- 8 parallel builder threads

---

### 5. Frame Builder: Dual Output (ET + File)

Send to ET for real-time processing, archive to files for permanent storage:

```bash
# Start ET system
et_start -f /tmp/et_sys_pagg -n 1000 -s 2000000

# Run receiver with dual output
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-framebuilder \
  --et-file /tmp/et_sys_pagg \
  --et-host localhost \
  --et-port 11111 \
  --et-station E2SAR_PAGG \
  --fb-output-dir /mnt/archive/run1234 \
  --fb-output-prefix clas12_run1234 \
  --fb-threads 4 \
  --et-event-size 2097152
```

**Dual Output:**
- **ET**: Real-time processing via ET system
- **File**: Permanent archival to `/mnt/archive/run1234/`
- Both outputs receive identical EVIO-6 frames

**Use Cases:**
- Real-time analysis + long-term storage
- Backup in case ET consumer fails
- Offline reprocessing from archived files

---

### 6. High-Throughput Configuration

Multiple receiver threads, multiple builder threads:

```bash
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --threads 4 \
  --use-framebuilder \
  --fb-output-dir /data/frames \
  --fb-output-prefix run1234 \
  --fb-threads 8 \
  --bufsize 10485760 \
  --cores 0 1 2 3 4 5 6 7 8 9 10 11
```

**Performance:**
- 4 receiver threads on ports 10000-10003
- 8 parallel frame builder threads
- 10MB socket buffers
- CPU affinity to cores 0-11

---

## Output Modes Comparison

| Mode | Command Flag | Output Location | Format | Rollover | Use Case |
|------|--------------|----------------|--------|----------|----------|
| **Basic** | (default) | Single file | Raw frames | No | Simple recording |
| **FB File** | `--use-framebuilder` + `--fb-output-dir` | Multiple files | EVIO-6 | 2GB auto | Archival with structure |
| **FB ET** | `--use-framebuilder` + `--et-file` | ET system | EVIO-6 | N/A | Real-time processing |
| **FB Dual** | Both above | ET + files | EVIO-6 | 2GB auto | Production (backup) |

---

## Frame Builder Requirements

### Minimum Configuration

At least ONE output mode must be enabled:
- **ET output**: Specify `--et-file` AND `--et-station`
- **File output**: Specify `--fb-output-dir`

Example error if neither specified:
```
Frame builder requires at least one output mode:
  ET output: specify --et-file and --et-station
  File output: specify --fb-output-dir
```

### Thread Count

Valid range: 1-32 threads (default: 4)

**Recommendations:**
- **Low data rate**: 2-4 threads
- **Medium data rate**: 4-8 threads
- **High data rate**: 8-16 threads
- **Very high rate**: 16-32 threads (requires many CPU cores)

---

## ET Connection Modes

### Local ET (Broadcast Discovery)

```bash
--et-file /tmp/et_sys_pagg
# No host or port specified → uses broadcast
```

Searches local network via UDP broadcast.

### Direct Connection by Hostname

```bash
--et-file /tmp/et_sys_pagg \
--et-host et-server.jlab.org \
--et-port 11111
```

Connects directly to specific hostname and port.

### Direct Connection by IP

```bash
--et-file /tmp/et_sys_pagg \
--et-host 192.168.100.50 \
--et-port 11111
```

Connects directly to IP address (no DNS needed).

---

## File Output Details

### File Naming

Format: `{prefix}_thread{N}_file{NNNN}.evio`

Examples:
- `frames_thread0_file0000.evio` (first file, thread 0)
- `frames_thread0_file0001.evio` (second file after rollover)
- `frames_thread1_file0000.evio` (first file, thread 1)

### Automatic Rollover

- **Trigger**: File reaches 2GB (2,147,483,648 bytes)
- **Action**: Close current file, increment counter, open new file
- **Seamless**: No data loss during rollover
- **Per-thread**: Each thread manages its own file sequence

### Output Directory

- Specified with `--fb-output-dir`
- Created automatically if doesn't exist
- Must have write permissions

---

## Statistics Output

When frame builder is active, statistics include:

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

Displayed on Ctrl+C shutdown.

---

## Troubleshooting

### Error: "Output directory required when not using frame builder"

**Cause**: Running without `--use-framebuilder` and no `--output-dir`

**Solution**: Add `--output-dir` or use `--use-framebuilder`

### Error: "Frame builder requires at least one output mode"

**Cause**: Used `--use-framebuilder` but didn't specify ET or file output

**Solution**: Add either:
- `--et-file /tmp/et_sys_pagg --et-station STATION_NAME`
- `--fb-output-dir /path/to/output`
- Or both for dual output

### Error: "Failed to start frame builder"

**Possible causes:**
- ET system not running (check with `et_monitor -f /tmp/et_sys_pagg`)
- Output directory not writable
- Invalid ET host/port

### ET Connection Issues

**Test ET system:**
```bash
et_monitor -f /tmp/et_sys_pagg
```

**Test network connectivity:**
```bash
telnet et-server.jlab.org 11111
```

### Disk Full During File Output

**Monitor disk space:**
```bash
df -h /data/frames
```

**Calculate expected usage:**
- Data rate × run duration = total size
- Example: 1 GB/s × 3600s = 3.6 TB/hour

---

## Performance Tuning

### Optimize Throughput

1. **Increase receiver threads**: `--threads 4`
2. **Increase builder threads**: `--fb-threads 8`
3. **Increase socket buffers**: `--bufsize 10485760`
4. **Use CPU affinity**: `--cores 0 1 2 3 4 5 6 7`
5. **Use SSD for file output**

### Reduce Latency

1. **Decrease frame timeout**: `--frame-timeout 500`
2. **Decrease timestamp slop**: `--timestamp-slop 50`
3. **Fewer builder threads**: `--fb-threads 2`
4. **Use ET output** (no disk I/O)

### Balance CPU and I/O

**CPU-bound:**
- Increase threads
- Use all cores

**I/O-bound (file output):**
- Reduce threads
- Use faster storage
- Consider ET output instead

---

## See Also

- `FRAMEBUILDER_README.md` - Frame builder architecture
- `FILE_OUTPUT_GUIDE.md` - Detailed file output documentation
- `ET_CONNECTION_GUIDE.md` - ET connection scenarios
- `INTEGRATION_EXAMPLE.md` - Code integration examples
