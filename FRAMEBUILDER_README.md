# E2SAR Reassembler Frame Builder

## Overview

The E2SAR Reassembler Frame Builder extends the existing E2SAR receiver to aggregate multiple reassembled frames from different data streams into a single EVIO-6 formatted Time Frame Bank and publishes the result to an ET (Event Transfer) system.

## Design

This implementation is based on the **EMU PAGG (Primary Aggregator)** module from the Jefferson Lab CODA Data Acquisition system. The frame builder performs the following functions:

1. **Multi-Stream Aggregation**: Collects reassembled payloads from multiple independent data streams
2. **Timestamp Synchronization**: Synchronizes frames by timestamp to ensure time-aligned aggregation
3. **EVIO-6 Bank Construction**: Formats aggregated data into compliant EVIO-6 Time Frame Banks
4. **ET System Integration**: Publishes built frames to the ET ring for downstream processing

## Architecture

### Components

- **TimeSlice**: Represents a single reassembled frame from one data stream, containing:
  - Timestamp
  - Frame number
  - Data source ID (ROC ID or stream ID)
  - Stream status
  - Payload data

- **AggregatedFrame**: Container for all time slices with the same timestamp, with timeout tracking

- **BuilderThread**: Individual builder thread that:
  - Maintains its own frame buffer (thread-local)
  - Builds EVIO-6 formatted banks
  - Sends frames to ET using its own attachment
  - Tracks thread-local statistics (no contention)

- **FrameBuilder**: Main coordinator class that:
  - Manages ET system connection
  - Creates multiple ET attachments (one per builder thread)
  - Distributes incoming slices to builder threads by timestamp hash
  - Aggregates statistics from all threads
  - Controls thread lifecycle

### Multi-threaded Architecture

The frame builder uses **multiple parallel builder threads** for high throughput, following the EMU PAGG design pattern:

```
                         FrameBuilder
                              |
                 Distributes by timestamp hash
                              |
        +---------------------+---------------------+
        |                     |                     |
   BuilderThread-0      BuilderThread-1  ...  BuilderThread-N
        |                     |                     |
   [Frame Buffer]       [Frame Buffer]        [Frame Buffer]
        |                     |                     |
   ET Attachment-0      ET Attachment-1       ET Attachment-N
        |                     |                     |
        +---------------------+---------------------+
                              |
                         ET System
```

**Key Design Features:**

1. **Lock-Free Distribution**: Incoming slices are hashed by timestamp to specific builder threads
2. **Parallel Building**: Each thread independently builds EVIO-6 banks in parallel
3. **Separate ET Attachments**: Each thread has its own ET attachment, avoiding lock contention
4. **Thread-Local Buffers**: Each thread maintains its own frame buffer (no shared state)
5. **Thread-Local Statistics**: Statistics tracked per-thread, aggregated only on stop
6. **Configurable Parallelism**: Number of builder threads can be configured (default: 4)

**Throughput Benefits:**
- N threads = up to N× throughput for frame building
- No locking during normal operation (only during slice insertion)
- Parallel ET event allocation and publishing
- Load balanced by timestamp distribution

## EVIO-6 Aggregated Time Frame Bank Format

Based on EMU's `Evio.java` implementation, the frame builder constructs banks with the following structure:

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
  One complete payload from each input stream
```

### CODA Tags

From EMU `CODATag.java`:
- `STREAMING_PHYS` (0xFFD0): Top-level streaming physics event
- `STREAMING_SIB_BUILT` (0xFFD1): Stream Info Bank (built/aggregated)
- `STREAMING_TSS_BUILT` (0x01): Time Slice Segment
- `STREAMING_AIS_BUILT` (0x02): Aggregation Info Segment

### Data Types

From EMU EVIO library:
- `BANK` (0x10): EVIO bank type
- `SEGMENT` (0x20): EVIO segment type

## ET System Integration

The implementation follows EMU's `DataTransportImplEt.java` and `DataChannelImplEt.java` patterns:

### Connection Modes

The frame builder supports three ET connection modes:

#### 1. Local Broadcast Mode (Default)
```cpp
FrameBuilder builder("/tmp/et_sys_pagg", "", 0, "STATION", ...);
//                    file name           ^   ^
//                                  empty host, port=0
```
- Broadcasts on local network to find ET system
- Suitable for local ET systems
- Automatically discovers ET server

#### 2. Direct Connection by Hostname
```cpp
FrameBuilder builder("/tmp/et_sys_pagg", "et-server.jlab.org", 11111, "STATION", ...);
//                    file name           hostname              port
```
- Connects directly to specific host
- Required for remote ET systems
- No broadcast, immediate connection

#### 3. Direct Connection by IP Address
```cpp
FrameBuilder builder("/tmp/et_sys_pagg", "192.168.1.100", 11111, "STATION", ...);
//                    file name           IP address        port
```
- Connects directly to specific IP
- Useful when hostname resolution unavailable
- No broadcast, immediate connection

**When to use each mode:**
- **Broadcast**: ET system on same machine or local network
- **Hostname**: ET system on remote machine with DNS
- **IP Address**: ET system on remote machine without DNS or for explicit control

### Initialization
1. Configure ET open parameters (host, port, broadcast/direct mode)
2. Open ET system connection with 10-second timeout
3. Create or attach to named station (PARALLEL mode for multi-threading)
4. Create N attachments (one per builder thread)

### Event Handling
1. Each builder thread allocates ET events independently (`et_events_new`)
2. Copy EVIO-6 formatted data to event buffer
3. Set event length
4. Put events to ET system (`et_events_put`)
5. All operations parallel across threads

### Shutdown
1. Stop all builder threads
2. Detach from all attachments
3. Close ET system
4. Clean up resources

## Usage

### Integration with E2SAR Receiver

To integrate the frame builder with the existing e2sar_receiver:

```cpp
#include "e2sar_reassembler_framebuilder.hpp"

// Example 1: ET output only
e2sar::FrameBuilder builder(
    "/tmp/et_sys_pagg",      // ET system file
    "localhost",             // ET host (empty "" for local/broadcast)
    11111,                   // ET port (0 for default)
    "PAGG_STATION",          // Station name
    "",                      // File output dir (empty = disabled)
    "frames",                // File prefix (ignored when dir empty)
    4,                       // Number of builder threads
    2*1024*1024,             // 2MB event size
    100,                     // Timestamp slop (ticks)
    1000                     // Frame timeout (ms)
);

// Example 2: File output only
e2sar::FrameBuilder builder(
    "",                      // ET file (empty = disabled)
    "",                      // ET host
    0,                       // ET port
    "",                      // ET station (empty = disabled)
    "/data/output",          // File output directory
    "frames",                // File prefix
    4,                       // Number of builder threads
    2*1024*1024,             // Max buffer size
    100,                     // Timestamp slop (ticks)
    1000                     // Frame timeout (ms)
);

// Example 3: Dual output (ET + file simultaneously)
e2sar::FrameBuilder builder(
    "/tmp/et_sys_pagg",      // ET system file
    "localhost",             // ET host
    11111,                   // ET port
    "PAGG_STATION",          // Station name
    "/data/output",          // File output directory
    "frames",                // File prefix
    4,                       // Number of builder threads
    2*1024*1024,             // 2MB event size
    100,                     // Timestamp slop (ticks)
    1000                     // Frame timeout (ms)
);

// Start the builder
if (!builder.start()) {
    std::cerr << "Failed to start frame builder" << std::endl;
    return -1;
}

// In your event reception loop:
// Add time slices - will be sent to enabled outputs
builder.addTimeSlice(
    timestamp,      // Frame timestamp
    eventNum,       // Frame number
    dataId,         // Source ID
    eventBuf,       // Payload data
    eventSize       // Payload size
);

// On shutdown:
builder.stop();
```

### Configuration Parameters

#### Output Configuration

- **etFile**: Path to ET system file (e.g., "/tmp/et_sys_pagg")
  - This is the file name the ET system uses, not a path to a physical file
  - **Empty string ""**: Disables ET output
  - **Non-empty**: Enables ET output

- **etHost**: ET system host name (string)
  - **Empty string ""**: Use broadcast to find local ET system (default)
  - **"localhost"**: Connect to ET on localhost directly
  - **"hostname"**: Connect to specific host (e.g., "et-server.jlab.org")
  - **IP address**: Connect to specific IP (e.g., "192.168.1.100")

- **etPort**: ET server TCP port (integer)
  - **0**: Use default ET port (default)
  - **Specific port**: Connect to ET on specific port (e.g., 11111)
  - Must match the port ET system was started with

- **stationName**: ET station name to attach to
  - **Empty string ""**: Disables ET output
  - **Non-empty**: Enables ET output

- **fileDir**: Output directory for file output
  - **Empty string ""**: Disables file output
  - **Valid directory path**: Enables file output to specified directory
  - Directory will be created automatically if it doesn't exist

- **filePrefix**: Prefix for output file names (default: "frames")
  - Each builder thread creates separate files with format: `{prefix}_thread{N}_file{NNNN}.evio`
  - Example: `frames_thread0_file0000.evio`, `frames_thread0_file0001.evio`, etc.

**Output Modes:**
1. **ET only**: Provide `etFile` and `stationName`, leave `fileDir` empty
2. **File only**: Provide `fileDir`, leave `etFile` and `stationName` empty
3. **Dual output**: Provide both ET and file parameters

**Note:** At least one output mode must be enabled.

#### Threading and Performance

- **numBuilderThreads**: Number of parallel builder threads (default: 4)
  - Higher values increase throughput but use more resources
  - Recommended: 2-8 threads depending on CPU cores available
  - ET station must be configured for PARALLEL mode (if using ET output)
  - Each thread creates separate output files (if using file output)

- **eventSize**: Maximum buffer size in bytes (default: 1MB)
  - For ET output: Maximum ET event size (should match ET system config)
  - For file output: Maximum frame size before rollover check

#### Frame Building

- **timestampSlop**: Maximum allowed timestamp difference between slices (in ticks)

- **frameTimeoutMs**: Timeout for incomplete frames (milliseconds)

#### File Output Features

- **Automatic Rollover**: When a file reaches 2GB, it automatically closes and opens a new file
- **Sequential Numbering**: Files are numbered sequentially: `file0000.evio`, `file0001.evio`, etc.
- **Per-Thread Files**: Each builder thread maintains its own file sequence
- **EVIO-6 Format**: Output files contain EVIO-6 formatted aggregated time frame banks

## Building

Add to your meson.build:

```meson
framebuilder_sources = files(
  'src/e2sar_reassembler_framebuilder.cpp'
)

# Link with ET library
et_dep = dependency('et', required: true)

executable('e2sar_receiver_pagg',
  framebuilder_sources + receiver_sources,
  dependencies: [et_dep, e2sar_dep, boost_dep],
  install: true
)
```

## Dependencies

- **ET library**: Jefferson Lab Event Transfer system
- **C++14 or later**: For thread, atomic, mutex support
- **E2SAR library**: For reassembly (existing dependency)

## Timestamp Synchronization

The frame builder implements timestamp consistency checking similar to EMU PAGG:

1. All slices in a frame must have timestamps within `timestampSlop` ticks
2. If timestamps diverge beyond this threshold:
   - Warning is logged
   - Error bit is set in EVIO-6 header
   - Frame is still built and sent (non-fatal error)

3. Average timestamp is calculated across all slices for the Time Slice Segment

## Error Handling

### Fatal Errors (stop building)
- ET system connection failure
- ET event allocation failure
- Data larger than ET event size

### Non-Fatal Errors (flagged in header)
- Timestamp inconsistency beyond slop
- Missing expected slices (timeout)
- Stream status errors from input

Non-fatal errors set bit 7 in the `Num` field of EVIO-6 headers.

## Statistics

The frame builder tracks (aggregated across all builder threads):
- **Builder Threads**: Number of parallel builder threads
- **Frames Built**: Total aggregated frames built across all threads
- **Slices Aggregated**: Total time slices processed across all threads
- **Build Errors**: Number of errors encountered across all threads
- **Timestamp Errors**: Number of timestamp inconsistency warnings
- **Avg Slices/Frame**: Average number of slices per built frame
- **Files Created**: Total number of output files created (when file output enabled)
- **Bytes Written**: Total bytes written to files (when file output enabled)

Statistics are thread-local during operation (no contention) and aggregated when `stop()` or `printStatistics()` is called.

Access via `printStatistics()` or `getStatistics()` methods.

**Example Output:**
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

## EMU PAGG Reference

Key EMU source files analyzed:

### Frame Building Logic
- `StreamAggregator.java`: Main aggregation and building thread logic
  - BuildingThread class (lines 1550-2260)
  - Multi-stream aggregation (lines 1960-2250)

### EVIO-6 Format
- `Evio.java`: EVIO-6 bank construction
  - `combineRocStreams()` (lines 2841-3057): Combines ROC time slices
  - `combineAggregatedStreams()` (lines 2548-2796): Combines previously aggregated streams
  - Bank structure documentation (lines 29-210)

### ET Integration
- `DataTransportImplEt.java`: ET system initialization and configuration
- `DataChannelImplEt.java`: ET event allocation, writing, and channel management
  - Event handling (lines 1472-1478, 2269-2270)
  - newEvents/putEvents patterns

## Differences from EMU PAGG

This C++ implementation differs from the Java EMU PAGG in:

1. **Language**: C++ vs Java
2. **Concurrency**:
   - **Similarities**: Multiple parallel builder threads, timestamp-based distribution
   - **Differences**: C++ std::thread vs Java threads with LMAX Disruptor ring buffers
   - Both achieve lock-free operation during building
3. **Memory Management**: Manual (with RAII patterns) vs automatic garbage collection
4. **ET API**: C ET library vs JET (Java ET)
5. **Thread Distribution**: Hash-based vs ring-buffer sequences

**Similarities (faithful to EMU PAGG):**
- Multiple parallel builder threads for high throughput
- Timestamp-based frame aggregation
- Thread-local buffers and statistics
- Separate ET attachments per thread
- EVIO-6 format exactly matches EMU output
- Timestamp synchronization with configurable slop
- Non-fatal error handling

The **EVIO-6 format, timestamp synchronization logic, multi-threaded architecture, and ET integration patterns** are faithful to the EMU PAGG design.

## Future Enhancements

Potential improvements:

1. **Expected Slice Count**: Configure expected number of input streams to detect missing data
2. **Ring Buffer**: Use lock-free ring buffer (like LMAX Disruptor) for input distribution
3. **Frame Ordering**: Ensure frames sent to ET maintain timestamp order across threads
4. **Missing Frame Handling**: Generate empty frames for missing data (like EMU PAGG)
5. **Metrics Export**: Prometheus/monitoring integration
6. **CPU Affinity**: Pin builder threads to specific CPU cores
7. **Adaptive Threading**: Dynamically adjust thread count based on load

## Testing

To test the frame builder:

1. Set up an ET system:
```bash
et_start -f /tmp/et_sys_pagg -n 1000 -s 2000000
```

2. Run receiver with frame builder enabled

3. Monitor ET system:
```bash
et_monitor -f /tmp/et_sys_pagg
```

4. Verify EVIO-6 format with downstream consumer or EVIO dump tools

## License

Copyright (c) 2024, Jefferson Science Associates
Licensed under the same terms as the E2SAR receiver project.

## Contact

For questions about this implementation, contact the E2SAR development team or refer to EMU documentation at:
https://github.com/JeffersonLab/emu
