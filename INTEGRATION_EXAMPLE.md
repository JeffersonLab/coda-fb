# Frame Builder Integration Example

This document shows how to integrate the frame builder with the existing `e2sar_receiver.cpp`.

## Modifications Required

### 1. Include the Frame Builder Header

```cpp
#include "e2sar_reassembler_framebuilder.hpp"
```

### 2. Add Command-Line Options

Add new options to the program for ET configuration:

```cpp
// ET system parameters
opts("et-file", po::value<std::string>()->default_value("/tmp/et_sys_pagg"),
     "ET system file name");
opts("et-host", po::value<std::string>()->default_value(""),
     "ET system host name (empty for local/broadcast, or specific host)");
opts("et-port", po::value<int>()->default_value(0),
     "ET server port (0 for default, or specific port like 11111)");
opts("et-station", po::value<std::string>()->default_value("E2SAR_PAGG"),
     "ET station name to attach to");
opts("et-threads", po::value<int>()->default_value(4),
     "Number of parallel frame builder threads (default: 4)");
opts("et-event-size", po::value<int>()->default_value(2*1024*1024),
     "ET event size in bytes (default: 2MB)");
opts("timestamp-slop", po::value<int>()->default_value(100),
     "Maximum timestamp difference allowed (ticks)");
opts("frame-timeout", po::value<int>()->default_value(1000),
     "Frame building timeout in milliseconds (default: 1000)");
opts("use-et", po::bool_switch()->default_value(false),
     "Enable ET output (otherwise write to file)");
```

### 3. Global Frame Builder Pointer

Add global pointer for signal handler access:

```cpp
e2sar::FrameBuilder *frameBuilderPtr{nullptr};
```

### 4. Update Signal Handler

Modify `ctrlCHandler` to stop the frame builder:

```cpp
void ctrlCHandler(int sig)
{
    if (handlerTriggered.exchange(true))
        return;

    std::cout << "\nStopping receiver threads..." << std::endl;
    threadsRunning = false;

    // Give threads time to stop
    boost::chrono::milliseconds duration(1000);
    boost::this_thread::sleep_for(duration);

    // Stop frame builder if running
    if (frameBuilderPtr != nullptr) {
        std::cout << "Stopping frame builder..." << std::endl;
        frameBuilderPtr->stop();
        delete frameBuilderPtr;
        frameBuilderPtr = nullptr;
    }

    // Close output file if open
    if (globalOutputFd >= 0)
    {
        std::cout << "Closing output file..." << std::endl;
        fsync(globalOutputFd);
        close(globalOutputFd);
        globalOutputFd = -1;
    }

    // ... rest of original signal handler
}
```

### 5. Modify receiveAndWriteFrames Function

Update the function to optionally use the frame builder:

```cpp
result<int> receiveAndWriteFrames(Reassembler *r, int outputFd,
                                   e2sar::FrameBuilder* frameBuilder)
{
    u_int8_t *eventBuf{nullptr};
    size_t eventSize;
    EventNum_t eventNum;
    u_int16_t dataId;

    std::cout << "Starting frame reception and processing loop..." << std::endl;

    while(threadsRunning)
    {
        // Receive event with 1 second timeout
        auto getEvtRes = r->recvEvent(&eventBuf, &eventSize, &eventNum, &dataId, 1000);

        if (getEvtRes.has_error())
        {
            receivedWithError++;
            continue;
        }

        // Timeout occurred, continue loop
        if (getEvtRes.value() == -1)
            continue;

        framesReceived++;

        // Route to frame builder or file based on configuration
        if (frameBuilder != nullptr) {
            // Send to frame builder for aggregation
            // Assume timestamp is embedded in eventNum for now
            // In production, extract real timestamp from payload
            uint64_t timestamp = eventNum;  // Replace with actual timestamp extraction

            frameBuilder->addTimeSlice(
                timestamp,
                eventNum,
                dataId,
                eventBuf,
                eventSize
            );

            framesWritten++;
            if (framesWritten % 100 == 0) {
                std::cout << "Sent " << framesWritten << " frames to builder..." << std::endl;
            }
        }
        else {
            // Original file writing code
            std::lock_guard<std::mutex> lock(fileMutex);

            ssize_t bytesWritten = write(outputFd, eventBuf, eventSize);
            if (bytesWritten < 0)
            {
                std::cerr << "Error writing event " << eventNum << " to file: "
                          << strerror(errno) << std::endl;
                writeErrors++;
            }
            else if (static_cast<size_t>(bytesWritten) != eventSize)
            {
                std::cerr << "Incomplete write for event " << eventNum
                          << ": wrote " << bytesWritten << " of " << eventSize
                          << " bytes" << std::endl;
                writeErrors++;
            }
            else
            {
                framesWritten++;
                if (framesWritten % 100 == 0) {
                    std::cout << "Written " << framesWritten << " frames..." << std::endl;
                }
            }
        }

        // Clean up event buffer
        delete[] eventBuf;
        eventBuf = nullptr;
    }

    std::cout << "Frame reception loop completed" << std::endl;
    return 0;
}
```

### 6. Update main() Function

Initialize and start the frame builder:

```cpp
int main(int argc, char **argv)
{
    // ... existing command line parsing ...

    std::string etFile;
    std::string etHost;
    int etPort;
    std::string etStation;
    int etThreads;
    int etEventSize;
    int timestampSlop;
    int frameTimeout;
    bool useET;

    // Get ET parameters from command line
    etFile = vm["et-file"].as<std::string>();
    etHost = vm["et-host"].as<std::string>();
    etPort = vm["et-port"].as<int>();
    etStation = vm["et-station"].as<std::string>();
    etThreads = vm["et-threads"].as<int>();
    etEventSize = vm["et-event-size"].as<int>();
    timestampSlop = vm["timestamp-slop"].as<int>();
    frameTimeout = vm["frame-timeout"].as<int>();
    useET = vm["use-et"].as<bool>();

    // ... existing validation and setup ...

    try {
        // ... existing reassembler setup ...

        // Initialize frame builder if ET output enabled
        if (useET) {
            std::cout << "Initializing frame builder for ET output..." << std::endl;
            std::cout << "  ET file: " << etFile << std::endl;
            if (!etHost.empty()) {
                std::cout << "  ET host: " << etHost << std::endl;
            }
            if (etPort > 0) {
                std::cout << "  ET port: " << etPort << std::endl;
            }
            std::cout << "  ET station: " << etStation << std::endl;
            std::cout << "  Builder threads: " << etThreads << std::endl;
            std::cout << "  ET event size: " << etEventSize << " bytes" << std::endl;
            std::cout << "  Timestamp slop: " << timestampSlop << " ticks" << std::endl;
            std::cout << "  Frame timeout: " << frameTimeout << " ms" << std::endl;

            frameBuilderPtr = new e2sar::FrameBuilder(
                etFile,
                etHost,
                etPort,
                etStation,
                etThreads,
                etEventSize,
                timestampSlop,
                frameTimeout
            );

            if (!frameBuilderPtr->start()) {
                std::cerr << "Failed to start frame builder" << std::endl;
                delete frameBuilderPtr;
                frameBuilderPtr = nullptr;
                ctrlCHandler(0);
                return -1;
            }

            std::cout << "Frame builder started successfully" << std::endl;
        }
        else {
            // Create single output file (original behavior)
            std::ostringstream outputFileName;
            outputFileName << filePrefix << fileExtension;
            bfs::path outputFilePath{outputDir};
            outputFilePath /= outputFileName.str();

            mode_t fileMode{S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH};
            globalOutputFd = open(outputFilePath.c_str(), O_WRONLY|O_CREAT|O_TRUNC, fileMode);
            if (globalOutputFd < 0)
            {
                std::cerr << "Unable to create output file " << outputFilePath
                          << ": " << strerror(errno) << std::endl;
                ctrlCHandler(0);
                return -1;
            }

            std::cout << "Writing all events to: " << outputFilePath << std::endl;
        }

        // ... existing thread setup ...

        // Start frame reception and writing
        auto result = receiveAndWriteFrames(reasPtr, globalOutputFd, frameBuilderPtr);

        // ... rest of main function ...
    }
    catch (...) {
        // ... exception handling ...
    }

    return 0;
}
```

## Complete Example Usage

### File Output (Original Receiver Behavior)
```bash
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --output-dir /data/output \
  --prefix events \
  --extension .bin
```

### Frame Builder: File Output Mode

#### File Output with Automatic Rollover
```bash
# Write aggregated frames to files with automatic 2GB rollover
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-framebuilder \
  --fb-output-dir /data/frames \
  --fb-output-prefix aggregated \
  --fb-threads 4 \
  --timestamp-slop 100 \
  --frame-timeout 1000
```

**Output files:**
```
/data/frames/aggregated_thread0_file0000.evio  (2GB, closed)
/data/frames/aggregated_thread0_file0001.evio  (2GB, closed)
/data/frames/aggregated_thread0_file0002.evio  (1.2GB, active)
/data/frames/aggregated_thread1_file0000.evio  (2GB, closed)
/data/frames/aggregated_thread1_file0001.evio  (1.8GB, active)
...
```

### Frame Builder: ET Output Mode

#### Local ET System
```bash
# First, start ET system locally
et_start -f /tmp/et_sys_pagg -n 1000 -s 2000000

# Then run receiver with local ET output (using broadcast to find ET)
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-et \
  --et-file /tmp/et_sys_pagg \
  --et-station E2SAR_PAGG \
  --et-threads 4 \
  --et-event-size 2097152 \
  --timestamp-slop 100 \
  --frame-timeout 1000
```

#### Remote ET System on Specific Host
```bash
# ET system running on remote host "et-server.jlab.org" port 11111
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-et \
  --et-file /tmp/et_sys_pagg \
  --et-host et-server.jlab.org \
  --et-port 11111 \
  --et-station E2SAR_PAGG \
  --et-threads 4 \
  --et-event-size 2097152
```

#### Remote ET System by IP Address
```bash
# ET system running on specific IP
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-et \
  --et-file /tmp/et_sys_pagg \
  --et-host 192.168.100.50 \
  --et-port 11111 \
  --et-station E2SAR_PAGG \
  --et-threads 8
```

### Frame Builder: Dual Output Mode (ET + File)

#### Send to ET and Write Files Simultaneously
```bash
# Start ET system first
et_start -f /tmp/et_sys_pagg -n 1000 -s 2000000

# Run receiver with both outputs enabled
e2sar_receiver \
  -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \
  --ip 192.168.1.100 \
  --port 10000 \
  --use-framebuilder \
  --et-file /tmp/et_sys_pagg \
  --et-host localhost \
  --et-port 11111 \
  --et-station E2SAR_PAGG \
  --fb-output-dir /data/backup \
  --fb-output-prefix frames_backup \
  --fb-threads 4 \
  --et-event-size 2097152 \
  --timestamp-slop 100 \
  --frame-timeout 1000
```

**Use cases for dual output:**
- **Real-time processing + archival**: Send to ET for immediate processing, write to files for permanent storage
- **Backup**: Files provide backup in case downstream ET consumer fails
- **Offline analysis**: Files can be analyzed later without replaying from source
- **Data validation**: Compare ET and file outputs for consistency checking

## Extracting Timestamps from Payloads

The example above assumes timestamps are in `eventNum`. In production, you'll need to extract actual timestamps from the reassembled payload. This depends on your data format:

```cpp
// Example: Extract timestamp from EVIO payload
uint64_t extractTimestamp(const uint8_t* payload, size_t len) {
    // Parse your specific payload format
    // For EVIO banks, this might involve:
    // 1. Parse bank headers
    // 2. Find Stream Info Bank (SIB)
    // 3. Extract timestamp from Time Slice Segment

    // Placeholder - replace with actual extraction logic
    if (len < 32) return 0;

    // Example: timestamp at offset 20 (8 bytes, little-endian)
    uint64_t ts;
    std::memcpy(&ts, payload + 20, sizeof(ts));
    return ts;
}

// Use in receiveAndWriteFrames:
uint64_t timestamp = extractTimestamp(eventBuf, eventSize);
frameBuilder->addTimeSlice(timestamp, eventNum, dataId, eventBuf, eventSize);
```

## Build Configuration

Update `meson.build`:

```meson
# Find ET library
et_dep = dependency('et', required: get_option('enable_et'))

# Source files
framebuilder_src = []
if et_dep.found()
  framebuilder_src = files('src/e2sar_reassembler_framebuilder.cpp')
  add_project_arguments('-DENABLE_FRAME_BUILDER', language: 'cpp')
endif

receiver_src = files('src/e2sar_receiver.cpp')

# Build executable
executable('e2sar_receiver',
  receiver_src + framebuilder_src,
  dependencies: [e2sar_dep, boost_dep, et_dep],
  install: true
)
```

Add to `meson_options.txt`:
```meson
option('enable_et', type: 'feature', value: 'auto',
       description: 'Enable ET system output via frame builder')
```

## Testing the Integration

1. **Start ET system**:
   ```bash
   et_start -f /tmp/et_sys_pagg -n 1000 -s 2000000
   ```

2. **Run receiver with ET output**:
   ```bash
   e2sar_receiver --use-et --et-file /tmp/et_sys_pagg
   ```

3. **Monitor ET system**:
   ```bash
   et_monitor -f /tmp/et_sys_pagg
   ```

4. **Consume from ET** (example consumer):
   ```bash
   et_consumer -f /tmp/et_sys_pagg -s E2SAR_PAGG
   ```

## Troubleshooting

### ET Connection Fails
- Verify ET system is running: `et_monitor -f /tmp/et_sys_pagg`
- Check file permissions
- Ensure file path is accessible

### No Frames Built
- Check that multiple slices are arriving with same timestamp
- Verify timestamp extraction logic
- Check frame timeout setting
- Monitor statistics output

### Performance Issues
- Increase ET event count (`-n` parameter in et_start)
- Increase ET event size if frames are large
- Adjust frame timeout for your data rate
- Consider multiple build threads (future enhancement)

## Next Steps

After integration:
1. Test with real data streams
2. Tune timestamp slop for your detector timing
3. Optimize frame timeout based on data rate
4. Add monitoring/metrics
5. Implement missing frame detection if needed
