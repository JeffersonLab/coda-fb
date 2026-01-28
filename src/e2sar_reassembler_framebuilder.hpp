/**
 * E2SAR Reassembler Frame Builder - Multi-threaded Header
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#ifndef E2SAR_REASSEMBLER_FRAMEBUILDER_HPP
#define E2SAR_REASSEMBLER_FRAMEBUILDER_HPP

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>
#include <et.h>

namespace e2sar {

// Forward declarations
struct TimeSlice;
struct AggregatedFrame;
class BuilderThread;

/**
 * Frame Builder - Multi-threaded aggregator and EVIO-6 builder
 *
 * This class aggregates reassembled frames from multiple data streams,
 * synchronizes them by timestamp, builds EVIO-6 compliant aggregated
 * time frame banks, and sends them to an ET system using multiple
 * parallel builder threads for high throughput.
 *
 * Architecture:
 * - Multiple builder threads run in parallel
 * - Incoming slices are distributed by timestamp hash
 * - Each thread independently builds and publishes frames
 * - Each thread has its own ET attachment for lock-free operation
 * - Thread-local statistics avoid contention
 *
 * Based on EMU PAGG (Primary Aggregator) multi-threaded design.
 */
class FrameBuilder {
private:
    // ET system handle
    et_sys_id etSystem;
    std::vector<et_att_id> etAttachments;

    // ET configuration
    std::string etSystemFile;
    std::string etHostName;
    int etPort;
    int etEventSize;
    bool enableET;

    // File output configuration
    bool enableFileOutput;
    std::string fileOutputDir;
    std::string fileOutputPrefix;

    // Builder threads
    int builderThreadCount;
    std::vector<std::unique_ptr<BuilderThread>> builderThreads;

    // Global control
    std::atomic<bool> running{false};

    // Statistics (aggregated from all threads)
    std::atomic<uint64_t> framesBuilt{0};
    std::atomic<uint64_t> slicesAggregated{0};
    std::atomic<uint64_t> buildErrors{0};
    std::atomic<uint64_t> timestampErrors{0};
    std::atomic<uint64_t> filesCreated{0};
    std::atomic<uint64_t> bytesWritten{0};

    // Configuration
    int timestampSlop;
    int frameTimeoutMs;
    int expectedStreams;  // Number of expected data streams (UDP ports)

    // Private methods
    bool initializeET();

public:
    /**
     * Constructor
     *
     * @param etFile ET system file name (e.g., "/tmp/et_sys_pagg")
     * @param etHost ET system host name (empty string "" for local/broadcast,
     *               or specific hostname/IP like "localhost" or "192.168.1.100")
     * @param etPort ET server port (0 for default ET port, or specific port like 11111)
     * @param stationName ET station name to attach to
     * @param fileDir Output directory for file output (empty to disable file output)
     * @param filePrefix Prefix for output file names (default: "frames")
     * @param numBuilderThreads Number of parallel builder threads (default: 4)
     * @param eventSize Maximum ET event size in bytes (default: 1MB)
     * @param tsSlop Maximum timestamp difference allowed (in ticks) (default: 100)
     * @param timeout Frame timeout in milliseconds (default: 1000)
     *
     * Note: At least one output mode (ET or file) must be enabled.
     *       - To enable ET output: provide valid etFile and stationName
     *       - To enable file output: provide valid fileDir
     *       - Both can be enabled simultaneously for dual output
     */
    FrameBuilder(const std::string& etFile,
                 const std::string& etHost,
                 int etPort,
                 const std::string& fileDir,
                 const std::string& filePrefix = "frames",
                 int numBuilderThreads = 4,
                 int eventSize = 1024*1024,
                 int tsSlop = 100,
                 int timeout = 1000,
                 int expectedStreams = 1);

    /**
     * Destructor
     */
    ~FrameBuilder();

    /**
     * Add a reassembled time slice to the aggregation buffer
     *
     * Thread-safe: Can be called from multiple threads simultaneously.
     * Slices are distributed to builder threads by timestamp hash.
     *
     * @param timestamp Frame timestamp (used for aggregation and distribution)
     * @param frameNumber Frame number
     * @param dataId Data source identifier (ROC ID, stream ID)
     * @param data Pointer to reassembled payload data
     * @param dataLen Length of payload data in bytes
     */
    void addTimeSlice(uint64_t timestamp, uint32_t frameNumber, uint16_t dataId,
                      const uint8_t* data, size_t dataLen);

    /**
     * Start the frame builder and all builder threads
     *
     * Initializes ET connection, creates multiple attachments,
     * and starts all builder threads.
     *
     * @return true on success, false on failure
     */
    bool start();

    /**
     * Stop the frame builder and all builder threads
     *
     * Stops all builder threads, aggregates statistics,
     * and closes ET connection.
     */
    void stop();

    /**
     * Print aggregated statistics from all builder threads
     */
    void printStatistics() const;

    /**
     * Get current aggregated statistics
     *
     * @param built Total frames built across all threads
     * @param slices Total slices aggregated across all threads
     * @param errors Total build errors across all threads
     * @param bytes Total bytes written across all threads
     */
    void getStatistics(uint64_t& built, uint64_t& slices, uint64_t& errors, uint64_t& bytes) const;

    // Prevent copying
    FrameBuilder(const FrameBuilder&) = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;
};

} // namespace e2sar

#endif // E2SAR_REASSEMBLER_FRAMEBUILDER_HPP
