/**
 * Standalone E2SAR Receiver - Receives UDP packets from EJFAT load balancer,
 * reconstructs them into frames, and persists frames to a file.
 *
 * This program registers with the EJFAT Control Plane, accepts segmented UDP
 * packets, reassembles them into complete events/frames, and appends all events
 * to a single output file. The file is properly closed when Ctrl+C is pressed.
 */

#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <cstring>
#include <mutex>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <locale>

#include <e2sar.hpp>
#ifdef ENABLE_FRAME_BUILDER
#include "e2sar_reassembler_framebuilder.hpp"
#include <et.h>
#else
// Forward declaration when frame builder is not available
namespace e2sar { class FrameBuilder; }
#endif

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace po = boost::program_options;
namespace bfs = boost::filesystem;
using namespace e2sar;

// Version information
#ifndef E2SAR_RECEIVER_VERSION
#define E2SAR_RECEIVER_VERSION "1.0.0"
#endif

// Global state for signal handling
bool threadsRunning = true;
Reassembler *reasPtr{nullptr};
e2sar::FrameBuilder *frameBuilderPtr{nullptr};  // Global frame builder pointer (nullptr if not built with ET)
std::atomic<bool> handlerTriggered{false};
int globalOutputFd{-1};  // Global file descriptor for single output file
std::mutex fileMutex;    // Mutex to protect file writes

// Note: Frame builder is always used when ENABLE_FRAME_BUILDER is defined

// Statistics
// Data Frames stage (UDP packets reassembled into data frames by E2SAR)
std::atomic<u_int64_t> dataFramesReceived{0};      // Data frames reassembled from UDP packets
std::atomic<u_int64_t> dataFramesBytesTotal{0};    // Total bytes of data frames

// Build Events stage (data frames aggregated into build events by frame builder)
std::atomic<u_int64_t> buildEventsWritten{0};      // Build events written to file/ET
std::atomic<u_int64_t> buildEventsBytesTotal{0};   // Total bytes of build events
std::atomic<u_int64_t> writeErrors{0};

// Error counters
std::atomic<u_int64_t> receivedWithError{0};
std::atomic<u_int64_t> payloadValidationErrors{0};
std::atomic<u_int64_t> wrongEndiannessCount{0};

// Timing for rate calculations
auto startTime = boost::chrono::high_resolution_clock::now();

u_int16_t reportThreadSleepMs{1000};

// Forward declarations (none currently needed)

/**
 * Get the local host IP address (first non-loopback IPv4 address)
 */
std::string getLocalHostIP(bool preferV6 = false) {
    struct ifaddrs *ifaddr, *ifa;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        std::cerr << "Failed to get network interfaces" << std::endl;
        return "";
    }

    std::string result;

    // Iterate through all network interfaces
    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr)
            continue;

        int family = ifa->ifa_addr->sa_family;

        // Check for IPv4 or IPv6
        if ((family == AF_INET && !preferV6) || (family == AF_INET6 && preferV6)) {
            int s = getnameinfo(ifa->ifa_addr,
                              (family == AF_INET) ? sizeof(struct sockaddr_in) :
                                                   sizeof(struct sockaddr_in6),
                              host, NI_MAXHOST,
                              nullptr, 0, NI_NUMERICHOST);
            if (s != 0) {
                continue;
            }

            // Skip loopback addresses
            std::string addr(host);
            if (addr != "127.0.0.1" && addr != "::1" && addr.find("127.") != 0) {
                result = addr;
                break;  // Use first non-loopback address found
            }
        }
    }

    freeifaddrs(ifaddr);
    return result;
}

void ctrlCHandler(int sig)
{
    if (handlerTriggered.exchange(true))
        return;

    std::cout << "\nCTRL-C received, initiating shutdown..." << std::endl;
    threadsRunning = false;

    // NOTE: We do NOT call exit() or delete objects here!
    // The signal handler just sets the flag to stop threads.
    // The main thread will detect this and perform proper cleanup.
}

/**
 * Perform final cleanup and print statistics
 * Called from main thread when exiting normally
 */
void performFinalCleanup()
{
    std::cout << "\nPerforming final cleanup..." << std::endl;

    // Clean up frame builder (already stopped in receiveAndWriteFrames)
    // NOTE: We intentionally leak the frameBuilderPtr to avoid hanging
    // in the destructor if Builder-0 thread is still running. The OS
    // will clean up all resources when the process exits.
#ifdef ENABLE_FRAME_BUILDER
    if (frameBuilderPtr != nullptr) {
        frameBuilderPtr->printStatistics();
        // DO NOT DELETE: Causes hang if thread was detached
        // delete frameBuilderPtr;
        frameBuilderPtr = nullptr;
    }
#endif

    // Close output file if open
    if (globalOutputFd >= 0)
    {
        std::cout << "Closing output file..." << std::endl;
        fsync(globalOutputFd);  // Ensure all data is written to disk
        close(globalOutputFd);
        globalOutputFd = -1;
    }

    if (reasPtr != nullptr)
    {
        std::cout << "Deregistering worker from control plane..." << std::endl;
        auto deregres = reasPtr->deregisterWorker();
        if (deregres.has_error())
            std::cerr << "Unable to deregister worker on exit: " << deregres.error().message() << std::endl;

        // NOTE: stopThreads() was already called in receiveAndWriteFrames()
        // Don't call it again here

        // Print final statistics
        // getStats() returns a tuple: <eventsRecvd, eventsReassembled, dataErrCnt, reassemblyLoss, enqueueLoss, lastE2SARError>
        auto stats = reasPtr->getStats();

        // Calculate final elapsed time and rates
        auto endTime = boost::chrono::high_resolution_clock::now();
        auto totalElapsedMs = boost::chrono::duration_cast<boost::chrono::milliseconds>(endTime - startTime).count();
        double totalElapsedSec = totalElapsedMs / 1000.0;

        // Calculate average data frame rates (UDP packets reassembled into data frames)
        double avgDataFrameRate = (totalElapsedSec > 0) ? (dataFramesReceived.load() / totalElapsedSec) : 0.0;
        double avgDataFrameDataRateMBps = (totalElapsedSec > 0) ? (dataFramesBytesTotal.load() / totalElapsedSec / (1024.0 * 1024.0)) : 0.0;

        // Calculate average build event rates (data frames aggregated into build events)
        double avgBuildEventRate = (totalElapsedSec > 0) ? (buildEventsWritten.load() / totalElapsedSec) : 0.0;
        double avgBuildEventDataRateMBps = (totalElapsedSec > 0) ? (buildEventsBytesTotal.load() / totalElapsedSec / (1024.0 * 1024.0)) : 0.0;

        std::cout << "\n======= Final Statistics =======" << std::endl;
        std::cout << "--- Data Frames (Reassembled from UDP) ---" << std::endl;
        std::cout << "\tData Frames: " << dataFramesReceived << std::endl;
        std::cout << "\tData Volume: " << std::fixed << std::setprecision(2)
                  << (dataFramesBytesTotal.load() / (1024.0 * 1024.0)) << " MB" << std::endl;
        std::cout << "\tAvg Frame Rate: " << std::fixed << std::setprecision(2)
                  << avgDataFrameRate << " frames/sec" << std::endl;
        std::cout << "\tAvg Data Rate: " << std::fixed << std::setprecision(2)
                  << avgDataFrameDataRateMBps << " MB/sec" << std::endl;
        std::cout << "--- Build Events (Aggregated/Written) ---" << std::endl;
        std::cout << "\tBuild Events: " << buildEventsWritten << std::endl;
        std::cout << "\tData Volume: " << std::fixed << std::setprecision(2)
                  << (buildEventsBytesTotal.load() / (1024.0 * 1024.0)) << " MB" << std::endl;
        std::cout << "\tAvg Event Rate: " << std::fixed << std::setprecision(2)
                  << avgBuildEventRate << " events/sec" << std::endl;
        std::cout << "\tAvg Data Rate: " << std::fixed << std::setprecision(2)
                  << avgBuildEventDataRateMBps << " MB/sec" << std::endl;
        std::cout << "--- Errors ---" << std::endl;
        std::cout << "\tWrite Errors: " << writeErrors << std::endl;
        std::cout << "\tReceive Errors: " << receivedWithError << std::endl;
        std::cout << "\tPayload Validation Errors: " << payloadValidationErrors << std::endl;
        std::cout << "\tWrong Endianness Count: " << wrongEndiannessCount << std::endl;
        std::cout << "--- Runtime ---" << std::endl;
        std::cout << "\tTotal Elapsed Time: " << std::fixed << std::setprecision(1)
                  << totalElapsedSec << " sec" << std::endl;
        std::cout << "================================" << std::endl;

        delete reasPtr;
        reasPtr = nullptr;
    }
}

/**
 * ============================================================================
 * EVIO Payload Metadata Structure
 * ============================================================================
 *
 * This structure holds metadata extracted from a reassembled EVIO frame's
 * payload. The metadata is parsed from specific words in the EVIO structure
 * to ensure data integrity and provide accurate timing information.
 */
struct EVIOMetadata {
    uint64_t timestamp;      // 64-bit timestamp extracted from payload words 15-16
                              // Used for synchronizing frames across multiple streams

    uint32_t frameNumber;    // Frame/event sequence number from payload word 14
                              // Identifies this specific frame in the data stream

    uint16_t dataId;         // ROC (Readout Controller) or Stream ID from payload word 10
                              // Identifies which data source produced this frame

    bool valid;              // Flag indicating if payload passed all validation checks
                              // false = frame should be skipped due to corruption/errors

    bool wrongEndian;        // Flag indicating if data had incorrect byte ordering
                              // true = data was byte-swapped but successfully corrected
};

#ifdef ENABLE_FRAME_BUILDER
// Direct ET functions removed - frame builder is always used
#if 0  // Keeping for reference only
bool initializeDirectET_REMOVED(const std::string& etFile, const std::string& etHost,
                        int etPort, int etEventSize) {
    std::cout << "Initializing direct ET connection..." << std::endl;

    // Create ET open configuration
    et_openconfig openConfig;
    et_open_config_init(&openConfig);

    // Set connection mode based on host parameter
    if (etHost.empty()) {
        et_open_config_setcast(openConfig, ET_BROADCAST);
        std::cout << "  Mode: broadcast (local network)" << std::endl;
    } else {
        et_open_config_setcast(openConfig, ET_DIRECT);
        et_open_config_sethost(openConfig, etHost.c_str());
        std::cout << "  Mode: direct connection to " << etHost;
        if (etPort > 0) std::cout << ":" << etPort;
        std::cout << std::endl;
    }

    // Configure port if specified (use setserverport for ET server's TCP port)
    if (etPort > 0) {
        et_open_config_setserverport(openConfig, etPort);
    }

    // Set timeout for connection attempts
    struct timespec timeout;
    timeout.tv_sec = 10;
    timeout.tv_nsec = 0;
    et_open_config_settimeout(openConfig, timeout);

    // Open ET system
    int status = et_open(&directEtSystem, etFile.c_str(), openConfig);
    et_open_config_destroy(openConfig);

    if (status != ET_OK) {
        std::cerr << "Failed to open ET system: " << etFile << " (status=" << status << ")" << std::endl;
        return false;
    }

    std::cout << "  ET system opened: " << etFile << std::endl;

    // Attach to Grand Central station (ID 0) - the primary entry point of ET system
    std::cout << "  Attaching to GRAND_CENTRAL station (ID 0)" << std::endl;
    status = et_station_attach(directEtSystem, 0, &directEtAttachment);
    if (status != ET_OK) {
        std::cerr << "Failed to attach to GRAND_CENTRAL station (status=" << status << ")" << std::endl;
        et_close(directEtSystem);
        directEtSystem = nullptr;
        return false;
    }

    std::cout << "  Attached to GRAND_CENTRAL successfully" << std::endl;
    std::cout << "  Event size: " << (etEventSize / (1024*1024)) << " MB" << std::endl;
    std::cout << "Direct ET connection initialized successfully" << std::endl;

    return true;
}

/**
 * Cleanup direct ET connection
 */
void cleanupDirectET_REMOVED() {
    // Removed - frame builder is always used
}
#endif  // 0 - reference code
#endif  // ENABLE_FRAME_BUILDER

/**
 * ============================================================================
 * EVIO-6 CODA Tags and Data Types
 * ============================================================================
 */
namespace CODATag {
    const uint16_t STREAMING_PHYS = 0xFFD0;      // Top-level streaming physics event
    const uint16_t STREAMING_SIB_BUILT = 0xFFD1; // Stream Info Bank (built/aggregated)
    const uint8_t  STREAMING_TSS_BUILT = 0x01;   // Time Slice Segment
    const uint8_t  STREAMING_AIS_BUILT = 0x02;   // Aggregation Info Segment
}

namespace DataType {
    const uint8_t BANK = 0x10;    // EVIO bank type
    const uint8_t SEGMENT = 0x20; // EVIO segment type
}

/**
 * ============================================================================
 * Byte Swap Utility for 32-bit Words
 * ============================================================================
 *
 * Converts a 32-bit word from one endianness to another by reversing byte order.
 * Used when EVIO payload is detected to have wrong endianness.
 *
 * Example: 0x12345678 -> 0x78563412
 *
 * @param val  The 32-bit value to byte-swap
 * @return     The byte-swapped 32-bit value
 */
inline uint32_t swap32(uint32_t val) {
    return ((val & 0x000000FF) << 24) |  // Move byte 0 to byte 3
           ((val & 0x0000FF00) << 8)  |  // Move byte 1 to byte 2
           ((val & 0x00FF0000) >> 8)  |  // Move byte 2 to byte 1
           ((val & 0xFF000000) >> 24);   // Move byte 3 to byte 0
}

/**
 * ============================================================================
 * Parse EVIO Payload and Extract Metadata
 * ============================================================================
 *
 * Validates a reassembled EVIO frame and extracts timing/identification metadata
 * from its payload structure. This ensures data integrity and provides accurate
 * information for frame aggregation.
 *
 * EVIO Payload Structure (32-bit words, 1-indexed as per specification):
 * -------------------------------------------------------------------------
 * Word 1-7:   [Header data - not parsed here]
 * Word 8:     0xc0da0100    - Magic number (correctness verification)
 * Word 9:     ROC bank length
 * Word 10:    0x0010_10_ss  - ROC_ID (ss = stream/ROC identifier)
 * Word 11:    Stream info bank length
 * Word 12:    0xFF30_20_ss  - Stream info header
 * Word 13:    0x31_01_LLLL  - Time slice segment header (LLLL = length)
 * Word 14:    Frame number  - Sequence number for this frame
 * Word 15:    Timestamp[31:0]  - Lower 32 bits of 64-bit timestamp
 * Word 16:    Timestamp[63:32] - Upper 32 bits of 64-bit timestamp
 *
 * @param payload      Pointer to the reassembled frame payload data
 * @param payloadSize  Size of the payload in bytes
 * @return             EVIOMetadata structure with extracted data and validation flags
 */
EVIOMetadata parseEVIOPayload(const u_int8_t* payload, size_t payloadSize) {
    // Initialize metadata structure with all zeros and invalid flags
    EVIOMetadata meta = {0, 0, 0, false, false};

    // ========================================================================
    // STEP 1: Validate Minimum Payload Size
    // ========================================================================
    // We need at least 16 32-bit words (64 bytes) to access all required fields
    if (payloadSize < 64) {
        std::cerr << "ERROR: Payload too small for EVIO format: " << payloadSize
                  << " bytes (minimum: 64 bytes)" << std::endl;
        return meta;  // Return invalid metadata
    }

    // ========================================================================
    // STEP 2: Interpret Payload as Array of 32-bit Words
    // ========================================================================
    // Cast byte array to 32-bit word array for easier access
    // NOTE: Assumes payload is properly aligned (should be from network stack)
    const uint32_t* words = reinterpret_cast<const uint32_t*>(payload);

    // ========================================================================
    // STEP 3: Check Magic Number at Word 8 (Index 7 in 0-Based Array)
    // ========================================================================
    // The magic number serves two purposes:
    // 1. Verifies frame was correctly reassembled (no missing/corrupt packets)
    // 2. Indicates correct byte ordering (endianness)

    uint32_t magic = words[7];  // Word 8 in 1-based indexing
    bool needsSwap = false;

    if (magic == 0xc0da0100) {
        // SUCCESS: Correct magic number and correct endianness
        needsSwap = false;

    } else if (magic == 0x0001dac0) {
        // WARNING: Magic number is byte-swapped (wrong endianness detected)
        // We can still use the data, but need to swap all words
        needsSwap = true;
        meta.wrongEndian = true;

    } else {
        // FAILURE: Invalid magic number - frame is corrupted or incorrectly assembled
        std::cerr << "ERROR: Invalid EVIO magic number at word 8: 0x" << std::hex << magic
                  << std::dec << " (expected 0xc0da0100 or 0x0001dac0)" << std::endl;
        return meta;  // Return invalid metadata
    }

    // ========================================================================
    // STEP 4: Define Helper Function for Reading Words with Byte Swapping
    // ========================================================================
    // This lambda automatically swaps bytes if wrong endianness was detected
    auto readWord = [&](size_t index) -> uint32_t {
        return needsSwap ? swap32(words[index]) : words[index];
    };

    // ========================================================================
    // STEP 5: Extract and Validate ROC_ID from Word 10 (Index 9)
    // ========================================================================
    // Word 10 format: 0xXXXX_10_ss where:
    //   - Upper 16 bits: varies by EVIO version (commonly 0x0010 or 0x0002)
    //   - Next 8 bits must be 0x10 (fixed identifier)
    //   - Lowest 8 bits (ss) = ROC/Stream identifier

    uint32_t word10 = readWord(9);  // Word 10 in 1-based indexing

    // Extract the three components of word 10
    [[maybe_unused]] uint16_t upper16 = (word10 >> 16) & 0xFFFF;  // Bits 31-16 (version-dependent)
    uint8_t  next8   = (word10 >> 8)  & 0xFF;    // Bits 15-8 (must be 0x10)
    uint8_t  ss      = word10 & 0xFF;             // Bits 7-0 (ROC ID)

    // Validate only the middle byte (0x10) - upper 16 bits may vary by format version
    if (next8 != 0x10) {
        std::cerr << "ERROR: Invalid ROC_ID format at word 10: 0x" << std::hex << word10
                  << std::dec << " (expected middle byte 0x10)" << std::endl;
        return meta;  // Return invalid metadata
    }

    // Extract the ROC/Stream ID from the lowest 8 bits
    meta.dataId = ss;

    // ========================================================================
    // STEP 6: Extract Frame Number from Word 14 (Index 13)
    // ========================================================================
    // This is a simple 32-bit sequence number identifying this frame
    meta.frameNumber = readWord(13);  // Word 14 in 1-based indexing

    // ========================================================================
    // STEP 7: Extract 64-bit Timestamp from Words 15-16 (Indices 14-15)
    // ========================================================================
    // The timestamp is split across two consecutive 32-bit words:
    //   - Word 15: Lower 32 bits [31:0]
    //   - Word 16: Upper 32 bits [63:32]

    uint32_t ts_low  = readWord(14);  // Word 15: timestamp bits [31:0]
    uint32_t ts_high = readWord(15);  // Word 16: timestamp bits [63:32]

    // Combine the two 32-bit halves into a single 64-bit timestamp
    // Example: ts_high=0x12345678, ts_low=0x9ABCDEF0 => 0x123456789ABCDEF0
    meta.timestamp = (static_cast<uint64_t>(ts_high) << 32) | ts_low;

    // ========================================================================
    // STEP 8: Mark Metadata as Valid
    // ========================================================================
    // All validation checks passed - metadata is ready to use
    meta.valid = true;

    return meta;
}

result<int> prepareToReceive(Reassembler &r)
{
    // Get hostname and register with control plane
    std::cout << "Getting hostname... " << std::flush;
    auto hostname_res = NetUtil::getHostName();
    if (hostname_res.has_error())
    {
        return E2SARErrorInfo{hostname_res.error().code(), hostname_res.error().message()};
    }
    std::cout << "done" << std::endl;

    std::cout << "Registering worker '" << hostname_res.value() << "' with control plane... " << std::flush;
    auto regres = r.registerWorker(hostname_res.value());
    if (regres.has_error())
    {
        return E2SARErrorInfo{E2SARErrorc::RPCError,
            "Unable to register worker node: " + regres.error().message()};
    }
    std::cout << "done" << std::endl;

    // Open sockets and start receiver threads
    auto openRes = r.openAndStart();
    if (openRes.has_error())
        return openRes;

    return 0;
}

/**
 * ============================================================================
 * Main Frame Reception and Processing Loop
 * ============================================================================
 *
 * Receives reassembled frames from the E2SAR reassembler and either:
 * - Sends them to the frame builder for aggregation (if enabled)
 * - Writes them directly to an output file (original mode)
 *
 * Each frame is validated by parsing its EVIO payload structure to ensure
 * data integrity and extract accurate metadata.
 *
 * @param r              Pointer to the E2SAR Reassembler
 * @param outputFd       File descriptor for output file (used in file-only mode)
 * @param frameBuilder   Pointer to frame builder (nullptr if not using frame builder)
 * @return               Result code (0 on success, error otherwise)
 */
result<int> receiveAndWriteFrames(Reassembler *r, int outputFd, e2sar::FrameBuilder* frameBuilder)
{
    // Variables to hold received frame data from reassembler
    u_int8_t *eventBuf{nullptr};    // Pointer to reassembled payload data
    size_t eventSize;                // Size of reassembled payload in bytes
    EventNum_t eventNum;             // Event number from reassembler
    u_int16_t dataId;                // Data ID from reassembler

    // Print startup message based on mode
    if (frameBuilder != nullptr) {
        std::cout << "Starting frame reception and frame building loop..." << std::endl;
    } else {
        std::cout << "Starting frame reception and file writing loop..." << std::endl;
    }

    // ========================================================================
    // MAIN RECEPTION LOOP
    // ========================================================================
    // Continue processing until Ctrl+C is pressed (threadsRunning = false)
    while(threadsRunning)
    {
        // ====================================================================
        // STEP 1: Receive Next Reassembled Frame
        // ====================================================================
        // Attempt to receive a frame with 1 second timeout
        // Returns -1 on timeout (not an error), error code if real error
        auto getEvtRes = r->recvEvent(&eventBuf, &eventSize, &eventNum, &dataId, 1000);

        if (getEvtRes.has_error())
        {
            // Error occurred during reception - log and continue
            receivedWithError++;
            continue;
        }

        // Timeout occurred (no frame received within 1 second) - continue waiting
        if (getEvtRes.value() == -1)
            continue;

        // Successfully received a frame
        dataFramesReceived++;
        dataFramesBytesTotal += eventSize;  // Track total data volume

        // ====================================================================
        // STEP 2: Parse and Validate EVIO Payload
        // ====================================================================
        // Extract metadata from payload and verify data integrity
        // This checks the magic number, endianness, and extracts timestamp/IDs
        EVIOMetadata meta = parseEVIOPayload(eventBuf, eventSize);

        if (!meta.valid) {
            // VALIDATION FAILED: Payload is corrupt or incorrectly assembled
            // This frame cannot be used - skip it and continue to next frame
            payloadValidationErrors++;
            std::cerr << "Skipping frame " << eventNum << " due to invalid payload" << std::endl;

            // Clean up the unusable frame buffer
            delete[] eventBuf;
            eventBuf = nullptr;
            continue;  // Skip to next frame
        }

        // ====================================================================
        // STEP 3: Check for Endianness Issues
        // ====================================================================
        if (meta.wrongEndian) {
            // WARNING: Frame had wrong byte ordering but was corrected
            // This indicates potential issue with data source but data is usable
            wrongEndiannessCount++;
            // Note: The parseEVIOPayload function has already byte-swapped
            // the data, so we can proceed normally
        }

        // ====================================================================
        // STEP 4: Use Extracted Metadata from Payload
        // ====================================================================
        // IMPORTANT: We use metadata extracted from the EVIO payload structure
        // rather than the values provided by the reassembler (eventNum, dataId).
        // This ensures we have the actual timestamp and ROC ID from the data stream.
        //
        // Why? The reassembler's eventNum and dataId may not match the actual
        // values embedded in the payload, especially in multi-stream scenarios.

        uint64_t timestamp   = meta.timestamp;     // 64-bit timestamp from payload words 15-16
        uint32_t frameNumber = meta.frameNumber;   // Frame number from payload word 14
        uint16_t rocId       = meta.dataId;        // ROC ID from payload word 10

        // ====================================================================
        // STEP 5: Send Frame to Frame Builder for Aggregation
        // ====================================================================
        // The frame builder will:
        // 1. Validate and strip CODA block header from input frame
        // 2. Group frames with others by timestamp
        // 3. Build EVIO-6 aggregated time frame bank structure
        // 4. Output to ET system and/or files with 2GB rollover
        //
        // We pass the extracted metadata from the payload:
        // - timestamp: For grouping frames across multiple streams
        // - frameNumber: For tracking and debugging
        // - rocId: For identifying which ROC/stream this came from
        // - eventBuf: The actual payload data (includes CODA header to be stripped)
        // - eventSize: Size of payload in bytes

#ifdef ENABLE_FRAME_BUILDER
        frameBuilder->addTimeSlice(
            timestamp,       // 64-bit timestamp for synchronization
            frameNumber,     // Frame sequence number
            rocId,           // ROC/Stream identifier
            eventBuf,        // Pointer to payload data (frame builder will strip CODA header)
            eventSize        // Size of payload
        );

        // Note: buildEventsWritten is updated from frame builder statistics in statsReportingThread
#else
        // Without frame builder support, write raw frame directly to file
        // (This is a fallback mode without EVIO-6 formatting)
        if (outputFd >= 0) {
            // Acquire mutex to ensure thread-safe file writing
            std::lock_guard<std::mutex> lock(fileMutex);

            // Write raw frame to file
            ssize_t bytesWritten = write(outputFd, eventBuf, eventSize);

            if (bytesWritten < 0) {
                std::cerr << "Error writing event " << eventNum << " to file: "
                          << strerror(errno) << std::endl;
                writeErrors++;
            }
            else if (static_cast<size_t>(bytesWritten) != eventSize) {
                std::cerr << "Incomplete write for event " << eventNum
                          << ": wrote " << bytesWritten << " of " << eventSize
                          << " bytes" << std::endl;
                writeErrors++;
            } else {
                buildEventsWritten++;
                buildEventsBytesTotal += eventSize;
            }
        }
#endif

        // ====================================================================
        // STEP 6: Clean Up Frame Buffer
        // ====================================================================
        // The event buffer was allocated by the reassembler's recvEvent()
        // We must delete it here to avoid memory leaks
        delete[] eventBuf;
        eventBuf = nullptr;

    }  // End of main reception loop

    // ========================================================================
    // CLEANUP AFTER LOOP EXIT (Ctrl+C pressed or error occurred)
    // ========================================================================
    std::cout << "\nFrame reception loop completed" << std::endl;

    // IMPORTANT: Stop reassembler FIRST to prevent new data from being added
    // to the frame builder while we're trying to shut it down!
    if (r != nullptr) {
        std::cout << "Stopping reassembler threads..." << std::endl;
        r->stopThreads();
        std::cout << "Reassembler threads stopped" << std::endl;
    }

    // Now stop frame builder (no more data will arrive)
#ifdef ENABLE_FRAME_BUILDER
    if (frameBuilder != nullptr) {
        frameBuilder->stop();
    }
#endif

    // Close output file if open
    if (outputFd >= 0) {
        std::cout << "Closing output file..." << std::endl;
        fsync(outputFd);  // Ensure all data is written to disk
        close(outputFd);
        std::cout << "Output file closed" << std::endl;
    }

    std::cout << "receiveAndWriteFrames() returning" << std::endl;
    return 0;
}

void statsReportingThread(Reassembler *r)
{
    while(threadsRunning)
    {
        // getStats() returns a tuple: <eventsRecvd, eventsReassembled, dataErrCnt, reassemblyLoss, enqueueLoss, lastE2SARError>
        auto stats = r->getStats();

        // Get frame builder statistics if available
#ifdef ENABLE_FRAME_BUILDER
        if (frameBuilderPtr != nullptr) {
            uint64_t fbBuilt, fbSlices, fbErrors, fbBytes;
            frameBuilderPtr->getStatistics(fbBuilt, fbSlices, fbErrors, fbBytes);

            // Update counters with frame builder statistics
            buildEventsWritten = fbBuilt;
            buildEventsBytesTotal = fbBytes;
        }
#endif

        // Calculate elapsed time and rates
        auto currentTime = boost::chrono::high_resolution_clock::now();
        auto elapsedMs = boost::chrono::duration_cast<boost::chrono::milliseconds>(currentTime - startTime).count();
        double elapsedSec = elapsedMs / 1000.0;

        // Calculate data frame rates (UDP packets reassembled into data frames)
        double dataFrameRate = (elapsedSec > 0) ? (dataFramesReceived.load() / elapsedSec) : 0.0;
        double dataFrameDataRateMBps = (elapsedSec > 0) ? (dataFramesBytesTotal.load() / elapsedSec / (1024.0 * 1024.0)) : 0.0;

        // Calculate build event rates (data frames aggregated into build events)
        double buildEventRate = (elapsedSec > 0) ? (buildEventsWritten.load() / elapsedSec) : 0.0;
        double buildEventDataRateMBps = (elapsedSec > 0) ? (buildEventsBytesTotal.load() / elapsedSec / (1024.0 * 1024.0)) : 0.0;

        std::cout << "\n=== Statistics Report ===" << std::endl;
        std::cout << "--- Data Frames (Reassembled from UDP) ---" << std::endl;
        std::cout << "  Data Frames: " << dataFramesReceived << std::endl;
        std::cout << "  Data Volume: " << std::fixed << std::setprecision(2)
                  << (dataFramesBytesTotal.load() / (1024.0 * 1024.0)) << " MB" << std::endl;
        std::cout << "  Frame Rate: " << std::fixed << std::setprecision(2)
                  << dataFrameRate << " frames/sec" << std::endl;
        std::cout << "  Data Rate: " << std::fixed << std::setprecision(2)
                  << dataFrameDataRateMBps << " MB/sec" << std::endl;
        std::cout << "--- Build Events (Aggregated/Written) ---" << std::endl;
        std::cout << "  Build Events: " << buildEventsWritten << std::endl;
        std::cout << "  Data Volume: " << std::fixed << std::setprecision(2)
                  << (buildEventsBytesTotal.load() / (1024.0 * 1024.0)) << " MB" << std::endl;
        std::cout << "  Event Rate: " << std::fixed << std::setprecision(2)
                  << buildEventRate << " events/sec" << std::endl;
        std::cout << "  Data Rate: " << std::fixed << std::setprecision(2)
                  << buildEventDataRateMBps << " MB/sec" << std::endl;
        std::cout << "--- Errors ---" << std::endl;
        std::cout << "  Write Errors: " << writeErrors << std::endl;
        std::cout << "  Receive Errors: " << receivedWithError << std::endl;
        std::cout << "--- Runtime ---" << std::endl;
        std::cout << "  Elapsed Time: " << std::fixed << std::setprecision(1)
                  << elapsedSec << " sec" << std::endl;
        std::cout << "=========================" << std::endl;

        auto until = boost::chrono::high_resolution_clock::now() + boost::chrono::milliseconds(reportThreadSleepMs);
        boost::this_thread::sleep_until(until);
    }
}

int main(int argc, char **argv)
{
    po::options_description od("E2SAR Standalone Receiver Options");

    // Command line variables
    std::string ejfat_uri;
    std::string recvIP;
    std::string outputDir;
    std::string filePrefix;
    std::string fileExtension;
    u_int16_t recvStartPort;
    size_t numThreads;
    int sockBufSize;
    int eventTimeoutMS;
    std::vector<int> coreList;
    int numaNode;
    bool autoIP, withCP, preferV6, validate;

    auto opts = od.add_options()("help,h", "show this help message");

    // Required parameters
    opts("uri,u", po::value<std::string>(&ejfat_uri)->required(),
         "EJFAT URI for control plane connection (required)");
    opts("output-dir,o", po::value<std::string>(&outputDir)->default_value(""),
         "directory to save received frames (required unless using frame builder)");

    // Network parameters
    opts("ip", po::value<std::string>(&recvIP)->default_value(""), 
         "IP address for receiving UDP packets (conflicts with --autoip)");
    opts("port,p", po::value<u_int16_t>(&recvStartPort)->default_value(10000), 
         "starting UDP port number (default: 10000)");
    opts("autoip", po::bool_switch(&autoIP)->default_value(false),
         "auto-detect local host IP address for receiving UDP packets (conflicts with --ip)");

    // File output parameters
    opts("prefix", po::value<std::string>(&filePrefix)->default_value("events"),
         "filename for output file (default: 'events')");
    opts("extension,e", po::value<std::string>(&fileExtension)->default_value(".bin"),
         "file extension for output file (default: '.bin')");

    // Frame builder parameters
    std::string etFile;
    std::string etHost;
    int etPort;
    std::string fbOutputDir;
    std::string fbOutputPrefix;
    int fbThreads;
    int etEventSize;
    int timestampSlop;
    int frameTimeout;
    int expectedStreams;

    // ET output options
    opts("et-file", po::value<std::string>(&etFile)->default_value(""),
         "ET system file name (empty to disable ET output)");
    opts("et-host", po::value<std::string>(&etHost)->default_value(""),
         "ET system host (empty for local/broadcast, hostname, or IP)");
    opts("et-port", po::value<int>(&etPort)->default_value(0),
         "ET system port (0 for default)");
    opts("et-event-size", po::value<int>(&etEventSize)->default_value(2*1024*1024),
         "ET event size in bytes (default: 2MB)");

    // File output options for frame builder
    opts("fb-output-dir", po::value<std::string>(&fbOutputDir)->default_value(""),
         "frame builder file output directory (empty to disable file output)");
    opts("fb-output-prefix", po::value<std::string>(&fbOutputPrefix)->default_value("frames"),
         "frame builder file output prefix (default: frames)");
    opts("fb-threads", po::value<int>(&fbThreads)->default_value(1),
         "number of parallel frame builder threads (default: 1)");

    // Frame building options
    opts("timestamp-slop", po::value<int>(&timestampSlop)->default_value(100),
         "maximum timestamp difference allowed in ticks (default: 100)");
    opts("frame-timeout", po::value<int>(&frameTimeout)->default_value(1000),
         "frame building timeout in milliseconds (default: 1000)");
    opts("expected-streams", po::value<int>(&expectedStreams)->default_value(1),
         "number of expected data streams for frame aggregation (default: 1)");

    // Performance parameters
    opts("threads,t", po::value<size_t>(&numThreads)->default_value(1), 
         "number of receiver threads (default: 1)");
    opts("bufsize,b", po::value<int>(&sockBufSize)->default_value(3*1024*1024), 
         "socket buffer size in bytes (default: 3MB)");
    opts("timeout", po::value<int>(&eventTimeoutMS)->default_value(500), 
         "event reassembly timeout in milliseconds (default: 500)");

    // Control plane parameters
    opts("withcp,c", po::bool_switch(&withCP)->default_value(true), 
         "enable control plane interactions (default: true)");
    opts("ipv6,6", po::bool_switch(&preferV6)->default_value(false), 
         "prefer IPv6 for control plane connections");
    opts("novalidate,v", po::bool_switch()->default_value(false), 
         "don't validate TLS certificates");

    // Advanced parameters
    opts("cores", po::value<std::vector<int>>(&coreList)->multitoken(), 
         "list of CPU cores to bind receiver threads to");
    opts("numa", po::value<int>(&numaNode)->default_value(-1), 
         "bind memory allocation to specific NUMA node (if >= 0)");
    opts("report-interval", po::value<u_int16_t>(&reportThreadSleepMs)->default_value(5000), 
         "statistics reporting interval in milliseconds (default: 5000)");

    po::variables_map vm;

    try {
        po::store(po::parse_command_line(argc, argv, od), vm);
        
        if (vm.count("help")) {
            std::cout << "E2SAR Standalone Receiver v" << E2SAR_RECEIVER_VERSION << std::endl;
            std::cout << "============================================" << std::endl;
            std::cout << od << std::endl;
            std::cout << "\n=== Example Usage ===" << std::endl;
            std::cout << "\nNOTE: Frame builder is always used to aggregate frames and build EVIO-6 format." << std::endl;
            std::cout << "      At least one output method (ET or file) must be specified." << std::endl;

            std::cout << "\n1. File output with EVIO-6 aggregation (2GB auto-rollover):" << std::endl;
            std::cout << "e2sar_receiver -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \\" << std::endl;
            std::cout << "               --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "               --fb-output-dir /data/frames \\" << std::endl;
            std::cout << "               --fb-output-prefix aggregated \\" << std::endl;
            std::cout << "               --fb-threads 4" << std::endl;

            std::cout << "\n2. ET output with EVIO-6 aggregation:" << std::endl;
            std::cout << "e2sar_receiver -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \\" << std::endl;
            std::cout << "               --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "               --et-file /tmp/et_sys_pagg \\" << std::endl;
            std::cout << "               --fb-threads 4" << std::endl;

            std::cout << "\n3. Dual output (ET + file backup):" << std::endl;
            std::cout << "e2sar_receiver -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \\" << std::endl;
            std::cout << "               --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "               --et-file /tmp/et_sys_pagg \\" << std::endl;
            std::cout << "               --fb-output-dir /data/backup \\" << std::endl;
            std::cout << "               --fb-output-prefix backup" << std::endl;

            std::cout << "\n4. Raw file output (fallback mode - only when frame builder unavailable):" << std::endl;
            std::cout << "e2sar_receiver -u 'ejfat://token@ctrl-plane:18347/lb/1?data=192.168.1.100:10000' \\" << std::endl;
            std::cout << "               --ip 192.168.1.100 --port 10000 \\" << std::endl;
            std::cout << "               --output-dir /path/to/output \\" << std::endl;
            std::cout << "               --prefix events --extension .dat" << std::endl;

            return 0;
        }
        
        po::notify(vm);
    } catch (const boost::program_options::error &e) {
        std::cerr << "Command line error: " << e.what() << std::endl;
        return -1;
    }

    // Validate parameters
    if (!autoIP && recvIP.empty()) {
        std::cerr << "Either --ip or --autoip must be specified" << std::endl;
        return -1;
    }

    if (autoIP && !recvIP.empty()) {
        std::cerr << "Cannot specify both --ip and --autoip" << std::endl;
        return -1;
    }

    validate = !vm["novalidate"].as<bool>();

    // Validate frame builder parameters (frame builder is always used)
#ifdef ENABLE_FRAME_BUILDER
    // Check that at least one output is enabled
    bool hasETOutput = !etFile.empty();
    bool hasFileOutput = !fbOutputDir.empty();

    if (!hasETOutput && !hasFileOutput) {
        std::cerr << "Frame builder requires at least one output mode:" << std::endl;
        std::cerr << "  ET output: specify --et-file" << std::endl;
        std::cerr << "  File output: specify --fb-output-dir" << std::endl;
        return -1;
    }
#else
    // Without frame builder, require direct file output
    if (outputDir.empty()) {
        std::cerr << "Output directory (--output-dir) is required (frame builder not available)" << std::endl;
        return -1;
    }

    // Validate output directory
    if (!bfs::exists(outputDir) || !bfs::is_directory(outputDir)) {
        std::cerr << "Output directory '" << outputDir << "' does not exist or is not a directory" << std::endl;
        return -1;
    }

    // Test write permissions
    struct stat dirStat;
    if (stat(outputDir.c_str(), &dirStat) != 0) {
        std::cerr << "Cannot access output directory: " << strerror(errno) << std::endl;
        return -1;
    }

    if (!(dirStat.st_mode & S_IWUSR)) {
        std::cerr << "No write permission for output directory" << std::endl;
        return -1;
    }

    // Ensure extension starts with '.' if not empty
    if (!fileExtension.empty() && fileExtension[0] != '.') {
        fileExtension.insert(fileExtension.begin(), '.');
    }
#endif

#ifdef ENABLE_FRAME_BUILDER
    // Validate thread count
    if (fbThreads < 1 || fbThreads > 32) {
        std::cerr << "Frame builder threads must be between 1 and 32" << std::endl;
        return -1;
    }

    std::cout << "Frame builder enabled:" << std::endl;
    if (hasETOutput) {
        std::cout << "  ET output: " << etFile;
        if (!etHost.empty()) std::cout << " @ " << etHost;
        if (etPort > 0) std::cout << ":" << etPort;
        std::cout << " (GRAND_CENTRAL station)" << std::endl;
    }
    if (hasFileOutput) {
        std::cout << "  File output: " << fbOutputDir << "/" << fbOutputPrefix << "_*.evio" << std::endl;
    }
    std::cout << "  Threads: " << fbThreads << std::endl;
#endif

    // Set up signal handler
    signal(SIGINT, ctrlCHandler);

    std::cout << "E2SAR Standalone Receiver v" << E2SAR_RECEIVER_VERSION << std::endl;
    std::cout << "Using E2SAR library v" << get_Version() << std::endl;
    std::cout << "=========================================" << std::endl;

    // Handle NUMA binding if requested
#ifdef NUMA_AVAILABLE
    if (numaNode >= 0) {
        auto numaRes = Affinity::setNUMABind(numaNode);
        if (numaRes.has_error()) {
            std::cerr << "Unable to bind to NUMA node " << numaNode << ": " << numaRes.error().message() << std::endl;
            return -1;
        }
        std::cout << "NUMA binding: node " << numaNode << std::endl;
    }
#else
    if (numaNode >= 0) {
        std::cerr << "NUMA support not available in this build" << std::endl;
        return -1;
    }
#endif

    try {
        // Parse EJFAT URI
        EjfatURI::TokenType tokenType{EjfatURI::TokenType::instance};
        auto uri_result = EjfatURI::getFromString(ejfat_uri, tokenType, preferV6);
        if (uri_result.has_error()) {
            std::cerr << "Invalid EJFAT URI: " << uri_result.error().message() << std::endl;
            return -1;
        }
        auto uri = uri_result.value();

        // Configure reassembler
        Reassembler::ReassemblerFlags rflags;
        rflags.useCP = withCP;
        rflags.withLBHeader = !withCP;
        rflags.rcvSocketBufSize = sockBufSize;
        rflags.useHostAddress = preferV6;
        rflags.validateCert = validate;
        rflags.eventTimeout_ms = eventTimeoutMS;

        std::cout << "Control plane: " << (rflags.useCP ? "enabled" : "disabled") << std::endl;
        std::cout << "Event timeout: " << rflags.eventTimeout_ms << " ms" << std::endl;
        std::cout << "Socket buffer size: " << sockBufSize << " bytes" << std::endl;
        std::cout << "Output directory: " << outputDir << std::endl;

        // Get IP address - either from command line or auto-detect local host IP
        boost::asio::ip::address data_ip;
        if (autoIP) {
            // Auto-detect local host IP address
            std::string localIP = getLocalHostIP(preferV6);
            if (localIP.empty()) {
                std::cerr << "Failed to auto-detect local host IP address" << std::endl;
                return -1;
            }
            std::cout << "Auto-detected local host IP: " << localIP << std::endl;
            data_ip = boost::asio::ip::make_address(localIP);
        } else {
            data_ip = boost::asio::ip::make_address(recvIP);
        }

        // Create reassembler
        if (coreList.size() > 0) {
            reasPtr = new Reassembler(uri, data_ip, recvStartPort, coreList, rflags);
            std::cout << "CPU cores: ";
            for (size_t i = 0; i < coreList.size(); ++i) {
                std::cout << coreList[i];
                if (i < coreList.size() - 1) std::cout << ", ";
            }
            std::cout << std::endl;
        } else {
            reasPtr = new Reassembler(uri, data_ip, recvStartPort, numThreads, rflags);
            std::cout << "Receiver threads: " << numThreads << std::endl;
        }

        std::cout << "Listening on: " << data_ip << ":"
                  << recvStartPort << "-" << (recvStartPort + numThreads - 1) << std::endl;

        // Register and start receiving
        auto prepareResult = prepareToReceive(*reasPtr);
        if (prepareResult.has_error()) {
            std::cerr << "Failed to prepare receiver: " << prepareResult.error().message() << std::endl;
            ctrlCHandler(0);
            return -1;
        }

        std::cout << "Receiver started successfully. Press Ctrl+C to stop." << std::endl;

        // Initialize frame builder (always used when available)
#ifdef ENABLE_FRAME_BUILDER
        std::cout << "\nInitializing frame builder..." << std::endl;

        frameBuilderPtr = new e2sar::FrameBuilder(
            etFile,
            etHost,
            etPort,
            fbOutputDir,
            fbOutputPrefix,
            fbThreads,
            etEventSize,
            timestampSlop,
            frameTimeout,
            expectedStreams  // Number of expected data streams for aggregation
        );

        if (!frameBuilderPtr->start()) {
            std::cerr << "Failed to start frame builder" << std::endl;
            delete frameBuilderPtr;
            frameBuilderPtr = nullptr;
            ctrlCHandler(0);
            return -1;
        }

        std::cout << "Frame builder started successfully\n" << std::endl;
#else
        // Without frame builder support, require direct file output
        // Create single output file
        std::ostringstream outputFileName;
        outputFileName << filePrefix << fileExtension;
        bfs::path outputFilePath{outputDir};
        outputFilePath /= outputFileName.str();

        mode_t fileMode{S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH};
        globalOutputFd = open(outputFilePath.c_str(), O_WRONLY|O_CREAT|O_TRUNC, fileMode);
        if (globalOutputFd < 0)
        {
            std::cerr << "Unable to create output file " << outputFilePath << ": " << strerror(errno) << std::endl;
            performFinalCleanup();
            return -1;
        }

        std::cout << "Writing all events to: " << outputFilePath << std::endl;
#endif

        // Start statistics reporting thread
        boost::thread statsThread(&statsReportingThread, reasPtr);

        // Start frame reception and writing
        auto result = receiveAndWriteFrames(reasPtr, globalOutputFd, frameBuilderPtr);

        if (result.has_error()) {
            std::cerr << "Error in frame reception: " << result.error().message() << std::endl;
        }

        // Normal cleanup
        performFinalCleanup();

    } catch (const E2SARException &e) {
        std::cerr << "E2SAR error: " << static_cast<std::string>(e) << std::endl;
        performFinalCleanup();
        return -1;
    } catch (const std::exception &e) {
        std::cerr << "Standard exception: " << e.what() << std::endl;
        performFinalCleanup();
        return -1;
    }

    return 0;
}