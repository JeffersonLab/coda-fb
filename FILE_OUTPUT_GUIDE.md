# Frame Builder File Output Guide

## Overview

The E2SAR frame builder can write aggregated EVIO-6 frames directly to output files with automatic 2GB rollover. This provides an alternative or complement to ET system output.

## Features

- **EVIO-6 Format**: Output files contain properly formatted EVIO-6 aggregated time frame banks
- **Automatic Rollover**: When a file reaches 2GB, it automatically closes and opens a new file
- **Sequential Numbering**: Files are numbered sequentially with zero-padded counters
- **Per-Thread Files**: Each builder thread maintains its own file sequence for lock-free operation
- **Dual Output**: Can write to files and send to ET system simultaneously
- **Binary Format**: Files use binary EVIO-6 format (.evio extension)

## File Naming Convention

Files follow this naming pattern:
```
{prefix}_thread{N}_file{NNNN}.evio
```

Where:
- `{prefix}`: User-specified prefix (e.g., "frames", "aggregated", "run1234")
- `{N}`: Builder thread number (0 to numThreads-1)
- `{NNNN}`: Zero-padded sequential file number (0000, 0001, 0002, ...)

**Examples:**
```
frames_thread0_file0000.evio
frames_thread0_file0001.evio
frames_thread1_file0000.evio
frames_thread1_file0001.evio
aggregated_thread0_file0000.evio
run1234_thread0_file0000.evio
```

## Configuration

### Constructor Parameters

```cpp
FrameBuilder(
    const std::string& etFile,         // "" to disable ET
    const std::string& etHost,         // "" when ET disabled
    int etPort,                        // 0 when ET disabled
    const std::string& stationName,    // "" to disable ET
    const std::string& fileDir,        // Output directory (empty to disable)
    const std::string& filePrefix,     // File prefix (default: "frames")
    int numBuilderThreads,             // Number of parallel threads
    int eventSize,                     // Max buffer size
    int tsSlop,                        // Timestamp slop
    int timeout                        // Frame timeout
);
```

### Enable File Output Only

```cpp
e2sar::FrameBuilder builder(
    "",                    // ET disabled
    "",                    // ET disabled
    0,                     // ET disabled
    "",                    // ET disabled
    "/data/output",        // File output directory
    "frames",              // File prefix
    4,                     // 4 builder threads
    2*1024*1024,           // 2MB max buffer
    100,                   // Timestamp slop
    1000                   // Frame timeout ms
);
```

### Enable Dual Output (ET + File)

```cpp
e2sar::FrameBuilder builder(
    "/tmp/et_sys_pagg",    // ET file
    "localhost",           // ET host
    11111,                 // ET port
    "PAGG_STATION",        // ET station
    "/data/backup",        // File output directory
    "backup",              // File prefix
    4,                     // 4 builder threads
    2*1024*1024,           // 2MB event size
    100,                   // Timestamp slop
    1000                   // Frame timeout ms
);
```

## Automatic 2GB Rollover

### How It Works

1. Each builder thread maintains a counter for the current file size
2. After writing each frame, the thread checks if the file has exceeded 2GB (2,147,483,648 bytes)
3. If exceeded:
   - Current file is flushed and closed
   - File counter is incremented
   - New file is opened with updated counter
   - Writing continues to new file

### Rollover Example

```
frames_thread0_file0000.evio  (0 bytes → 2,147,483,648 bytes) [CLOSED]
frames_thread0_file0001.evio  (0 bytes → 2,147,483,648 bytes) [CLOSED]
frames_thread0_file0002.evio  (0 bytes → 1,532,891,200 bytes) [ACTIVE]
```

### Why 2GB?

- **Filesystem Compatibility**: Works with older filesystems (FAT32, etc.)
- **Manageability**: Easier to copy, archive, and process smaller files
- **Error Isolation**: File corruption affects only one 2GB segment
- **Parallel Processing**: Multiple files can be processed in parallel

## File Format

Each output file contains a sequence of EVIO-6 formatted aggregated time frame banks, written back-to-back:

```
[EVIO-6 Frame 1]
[EVIO-6 Frame 2]
[EVIO-6 Frame 3]
...
```

### EVIO-6 Frame Structure

Each frame follows the aggregated time frame bank structure:

```
Top-Level Bank (STREAMING_PHYS)
├── Stream Info Bank (SIB)
│   ├── Time Slice Segment (TSS)
│   │   ├── Frame number
│   │   ├── Average timestamp (64-bit)
│   │   └── Reserved
│   └── Aggregation Info Segment (AIS)
│       └── One word per slice: ROC_ID | stream_status
├── Time Slice 1 (original payload)
├── Time Slice 2 (original payload)
└── Time Slice N (original payload)
```

## Usage Examples

### Example 1: File Output Only

```cpp
#include "e2sar_reassembler_framebuilder.hpp"

// Create builder with file output
e2sar::FrameBuilder builder(
    "",                      // No ET
    "",
    0,
    "",
    "/data/frames",          // Output directory
    "clas12_run1234",        // Prefix with run info
    8,                       // 8 threads for high throughput
    2*1024*1024,
    100,
    1000
);

// Start
if (!builder.start()) {
    std::cerr << "Failed to start frame builder" << std::endl;
    return -1;
}

// Add frames
for (auto& frame : frames) {
    builder.addTimeSlice(
        frame.timestamp,
        frame.frameNumber,
        frame.dataId,
        frame.data,
        frame.size
    );
}

// Stop and print statistics
builder.stop();
```

**Output files:**
```
/data/frames/clas12_run1234_thread0_file0000.evio
/data/frames/clas12_run1234_thread0_file0001.evio
/data/frames/clas12_run1234_thread1_file0000.evio
...
```

### Example 2: Dual Output with Different Prefixes

```cpp
// Production setup: Send to ET for real-time processing,
// archive to files for permanent storage
e2sar::FrameBuilder builder(
    "/tmp/et_sys_clas12",
    "et-server.jlab.org",
    23911,
    "CLAS12_EB",
    "/mnt/archive/run1234",
    "clas12_run1234",
    4,
    4*1024*1024,              // 4MB events for CLAS12
    50,                        // Tighter timestamp slop
    500                        // Faster timeout
);

builder.start();

// Process events - sent to ET and archived to files
// ...

builder.stop();
builder.printStatistics();
```

**Statistics Output:**
```
=== Frame Builder Statistics ===
  Builder Threads: 4
  Frames Built: 125043
  Slices Aggregated: 500172
  Build Errors: 0
  Timestamp Errors: 12
  Avg Slices/Frame: 4.0
  Files Created: 48
  Bytes Written: 103079215104 (96.0 GB)
=================================
```

## Command-Line Integration

### File Output Only

```bash
e2sar_receiver \
  --use-framebuilder \
  --fb-output-dir /data/frames \
  --fb-output-prefix run1234 \
  --fb-threads 4
```

### ET + File Output

```bash
e2sar_receiver \
  --use-framebuilder \
  --et-file /tmp/et_sys_pagg \
  --et-station PAGG \
  --fb-output-dir /data/backup \
  --fb-output-prefix backup_run1234 \
  --fb-threads 4
```

## Performance Considerations

### Thread Count

- **More threads = higher throughput** but more output files
- Each thread writes independently (no contention)
- Recommended: 2-8 threads depending on CPU and I/O capacity

**Example with 4 threads:**
```
Total throughput: ~4 GB/s (1 GB/s per thread)
Output files: 4 parallel file sequences
```

### I/O Performance

- **SSD recommended** for high-throughput applications
- Each thread performs sequential writes (good for SSD and HDD)
- No seeking or random access during write
- Files buffered by OS (uses std::ofstream buffering)

### Disk Space

- Monitor disk space when using file output
- Each builder thread can generate 2GB files rapidly
- Example: 4 threads at 1 GB/s = 4 GB/s total = 14.4 TB/hour

## Reading Output Files

### Using EVIO Tools

Output files are standard EVIO-6 format and can be read with EVIO tools:

```bash
# Dump file contents
evio_dump frames_thread0_file0000.evio

# Convert to XML
evio2xml -i frames_thread0_file0000.evio -o frames.xml

# Analyze with ROOT
root -l 'analyze_evio.C("frames_thread0_file0000.evio")'
```

### Using EVIO Library

```cpp
#include <evioFileChannel.hxx>

evio::evioFileChannel chan("frames_thread0_file0000.evio", "r");
chan.open();

while (chan.read()) {
    evioDOMTree tree(chan);
    // Process tree
}

chan.close();
```

## Troubleshooting

### Directory Not Found

**Error:**
```
Failed to create output directory '/data/frames': No such file or directory
```

**Solution:**
- Ensure parent directory exists
- Check write permissions
- Frame builder will attempt to create directory automatically

### Disk Full

**Error:**
```
Failed to open output file: No space left on device
```

**Solution:**
- Monitor disk space before starting
- Calculate expected output size: (data rate × duration)
- Use dual output with ET to avoid blocking if disk fills

### File Naming Conflicts

If the frame builder is restarted and old files exist:
- Existing files are **not overwritten**
- File counter increments until unused number is found
- Manual cleanup of old files recommended before restart

### Performance Issues

**Symptoms:**
- Slow write speed
- Frame builder blocking
- Increasing frame timeout errors

**Solutions:**
- Reduce number of builder threads
- Use faster storage (SSD)
- Increase frame timeout
- Consider ET output instead (zero-copy to shared memory)

## Best Practices

1. **Directory Structure**: Use run-specific subdirectories
   ```
   /data/frames/run1234/
   /data/frames/run1235/
   ```

2. **Naming Convention**: Include run information in prefix
   ```
   "clas12_run1234"
   "gluex_run5678_stream1"
   ```

3. **Disk Monitoring**: Monitor available disk space
   ```bash
   df -h /data/frames
   ```

4. **File Management**: Implement archival/cleanup strategy
   ```bash
   # Archive old runs
   tar -czf run1234.tar.gz /data/frames/run1234/

   # Clean up archived files
   rm -rf /data/frames/run1234/
   ```

5. **Backup Strategy**: Use dual output for critical data
   - ET for real-time processing
   - Files for permanent archival

6. **Thread Configuration**: Match thread count to I/O capacity
   - Start with 4 threads
   - Increase if I/O not saturated
   - Decrease if I/O becomes bottleneck

## Statistics

When file output is enabled, statistics include:

- **Files Created**: Total number of files opened across all threads
- **Bytes Written**: Total bytes written to files

Example:
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

## Comparison: File vs ET Output

| Feature | File Output | ET Output |
|---------|-------------|-----------|
| **Latency** | High (disk I/O) | Low (shared memory) |
| **Throughput** | Limited by disk | Very high |
| **Persistence** | Permanent | Temporary (ring buffer) |
| **Consumer coupling** | Loose (read anytime) | Tight (must keep up) |
| **Use case** | Archival, offline analysis | Real-time processing |
| **Disk usage** | High | None |
| **Recovery** | Can replay from files | Cannot replay |

**Recommendation:** Use dual output for best of both worlds.

## See Also

- `FRAMEBUILDER_README.md` - Full frame builder documentation
- `INTEGRATION_EXAMPLE.md` - Integration examples with e2sar_receiver
- `ET_CONNECTION_GUIDE.md` - ET system connection guide
- EVIO-6 format specification: https://coda.jlab.org/drupal/
