/**
 * E2SAR Reassembler Frame Builder - Multi-threaded Implementation
 *
 * This module extends the E2SAR receiver to aggregate multiple reassembled
 * data streams with the same event number into a single EVIO-6 formatted
 * Time Frame Bank and sends the result to an ET system.
 *
 * EVENT-NUMBER-BASED AGGREGATION STRATEGY:
 * =========================================
 * After reassembly, each input stream places its reassembled frames into its
 * own stream-dedicated FIFO queue. The frame-building thread reads frames from
 * these per-stream FIFOs using EVENT NUMBERS (from the reassembler) rather than
 * timestamps or payload-embedded frame numbers.
 *
 * STARTUP CORRECTION FACTORS:
 * At the beginning of a run, if event numbers from different streams are not
 * aligned, constant per-stream correction factors are computed to align all
 * streams to the minimum observed event number. These correction factors remain
 * fixed for the entire run.
 *
 * ALGORITHM:
 * 1. STARTUP: Wait for first frame from each expected stream
 * 2. STARTUP: Compute per-stream correction factors (offset = minEventNum - streamEventNum)
 * 3. Each stream has its own FIFO queue of reassembled frames
 * 4. Builder thread finds minimum CORRECTED event number across all non-empty FIFOs
 * 5. Checks if ALL streams are aligned (all have same corrected event number at head)
 * 6. If ALIGNED:
 *    - Consume from all FIFOs with this corrected event number
 *    - Build complete aggregated frame
 *    - Send to ET/file output
 * 7. If NOT ALIGNED:
 *    - Consume ONLY from FIFOs with the minimum corrected event number (lagging streams)
 *    - Build partial frame for the lagging streams
 *    - Continue advancing only the lagging streams until alignment is restored
 * 8. After alignment is restored, resume reading from all FIFOs normally
 *
 * KEY BEHAVIORS:
 * - Per-stream correction factors are computed once at startup and remain constant
 * - Never consume from a FIFO that already has a larger corrected event number
 * - Always advance only the streams with smaller corrected event numbers
 * - Only build complete frames when all streams show the same corrected event number
 * - Timeout handling: Force build after timeout even if not all streams present
 * - Frame number slop validates data quality (default 0 = exact match required)
 *
 * SYNCHRONIZATION & THREAD SAFETY:
 * - Stream FIFOs are protected by mutex
 * - Condition variable wakes builder thread when new data arrives
 * - Lock released during expensive build/send operations
 * - Event number distribution ensures same event goes to same builder thread
 *
 * Multi-threaded design based on EMU PAGG (Primary Aggregator):
 * - Multiple parallel builder threads for high throughput
 * - Lock-free frame distribution across threads by event number hash
 * - Each builder thread handles frames hashed to it by event number
 * - Parallel EVIO-6 bank construction and ET publishing
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#include "e2sar_reassembler_framebuilder.hpp"
#include <et.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <memory>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <sys/stat.h>
#include <arpa/inet.h>

namespace e2sar {

// EVIO-6 CODA Tags (from EMU Evio.java)
namespace CODATag {
    constexpr uint16_t STREAMING_PHYS = 0xFFD0;        // Streaming physics event
    constexpr uint16_t STREAMING_SIB_BUILT = 0xFFD1;   // Stream Info Bank (built)
    constexpr uint16_t STREAMING_TSS_BUILT = 0x01;     // Time Slice Segment (built)
    constexpr uint16_t STREAMING_AIS_BUILT = 0x02;     // Aggregation Info Segment (built)
}

namespace DataType {
    constexpr uint8_t BANK = 0x10;
    constexpr uint8_t SEGMENT = 0x20;
}

/**
 * Structure representing a single reassembled time slice from one stream
 */
struct TimeSlice {
    uint64_t timestamp;      // Frame timestamp
    uint32_t frameNumber;    // Frame number
    uint16_t dataId;         // Data source ID (ROC ID, stream ID, etc.)
    uint16_t streamStatus;   // Stream status bits
    std::unique_ptr<uint8_t[]> payloadPtr;  // Owns the reassembled payload buffer
    size_t payloadSize;                      // Size of payload in bytes

    TimeSlice() : timestamp(0), frameNumber(0), dataId(0), streamStatus(0), payloadSize(0) {}

    // Transfer ownership constructor - takes ownership of the buffer pointer
    TimeSlice(uint64_t ts, uint32_t frame, uint16_t id, uint8_t* data, size_t len)
        : timestamp(ts), frameNumber(frame), dataId(id), streamStatus(0)
        , payloadPtr(data), payloadSize(len) {}
};

/**
 * Aggregated frame containing time slices for a single frame number
 *
 * ALIGNMENT-BASED AGGREGATION:
 * - Slices are collected from per-stream FIFOs when aligned on same frame number
 * - This structure is built temporarily during the aggregation process
 * - May contain slices from all streams (complete) or subset (partial/lagging)
 * - Timestamp consistency is validated as a data quality check
 */
struct AggregatedFrame {
    uint64_t timestamp;       // Average timestamp (for validation and output)
    uint32_t frameNumber;     // PRIMARY KEY for aggregation
    std::vector<TimeSlice> slices;
    std::chrono::steady_clock::time_point arrivalTime;  // When first slice arrived

    AggregatedFrame() : timestamp(0), frameNumber(0) {
        arrivalTime = std::chrono::steady_clock::now();
    }

    void addSlice(TimeSlice&& slice) {
        slices.push_back(std::move(slice));
    }

    size_t getSliceCount() const {
        return slices.size();
    }

    bool isTimedOut(int timeoutMs) const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - arrivalTime);
        return elapsed.count() > timeoutMs;
    }
};

/**
 * Individual Builder Thread
 * Each thread builds frames assigned to it by hash of frame number
 */
class BuilderThread {
private:
    int threadIndex;
    int threadCount;
    std::string threadName;

    // ET system access (shared)
    et_sys_id etSystem;
    et_att_id etAttachment;
    bool useET;

    // File output
    std::string outputDir;
    std::string outputPrefix;
    bool useFileOutput;
    std::ofstream outputFile;
    uint64_t currentFileSize;
    uint64_t maxFileSize;  // 2GB
    int currentFileNumber;
    std::mutex fileMutex;

    // ALIGNMENT-BASED FRAME BUILDING:
    // Each stream has its own FIFO queue of reassembled frames.
    // Frames are built only when all streams are aligned on the same frame number.
    std::unordered_map<uint16_t, std::queue<TimeSlice>> streamFIFOs;  // Key: dataId (stream ID)

    // Track first arrival time for each frame number (for timeout handling)
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> frameArrivalTimes;

    // PER-STREAM EVENT NUMBER CORRECTION FACTORS:
    // At startup, if streams have misaligned event numbers, we compute a constant
    // correction factor for each stream to align them to the minimum observed event number.
    // These corrections remain fixed for the entire run.
    std::unordered_map<uint16_t, int64_t> streamEventNumCorrections;  // streamId -> offset
    bool correctionFactorsInitialized{false};
    uint32_t minInitialEventNum{UINT32_MAX};  // Lowest event number seen at startup

    std::mutex frameMutex;
    std::condition_variable frameCV;

    // Thread control
    std::thread thread;
    std::atomic<bool> running{false};

    // Configuration
    int frameNumberSlop;       // Max allowed frame number difference for validation (after correction)
    int frameTimeoutMs;        // How long to wait for all expected streams before partial build
    int etEventSize;
    int expectedStreamCount;   // Number of expected data streams per frame number

    // Statistics (thread-local, no contention)
    uint64_t framesBuilt{0};
    uint64_t slicesProcessed{0};
    uint64_t buildErrors{0};
    uint64_t frameNumberErrors{0};
    uint64_t filesCreated{0};
    uint64_t bytesWritten{0};

public:
    BuilderThread(int index, int count, et_sys_id sys, et_att_id att,
                  int fnSlop, int timeout, int evtSize,
                  bool enableET, bool enableFile,
                  const std::string& fileDir, const std::string& filePrefix,
                  int numExpectedStreams)
        : threadIndex(index)
        , threadCount(count)
        , etSystem(sys)
        , etAttachment(att)
        , useET(enableET)
        , outputDir(fileDir)
        , outputPrefix(filePrefix)
        , useFileOutput(enableFile)
        , currentFileSize(0)
        , maxFileSize(2ULL * 1024 * 1024 * 1024)  // 2GB
        , currentFileNumber(0)
        , frameNumberSlop(fnSlop)
        , frameTimeoutMs(timeout)
        , etEventSize(evtSize)
        , expectedStreamCount(numExpectedStreams)
    {
        threadName = "Builder-" + std::to_string(index);
    }

    /**
     * Open a new output file with sequential numbering
     */
    /**
     * Write EVIO-6 file header (14 words = 56 bytes)
     * This should be written once at the beginning of each new file
     */
    bool writeFileHeader() {
        // NOTE: This function assumes outputFile is open and fileMutex is held

        // EVIO-6 File Header: 14 words (32-bit) in BIG-ENDIAN
        uint32_t fileHeader[14] = {
            0x4556494F,  // WORD 0: File Type ID "EVIO" in ASCII
            0x00000000,  // WORD 1: File Number (0 if unused)
            0x0000000E,  // WORD 2: Header Length (14 words)
            0x00000000,  // WORD 3: Record Count (0 if unknown)
            0x00000000,  // WORD 4: File Index Array Length (0)
            0x00000006,  // WORD 5: Bit Info + Version (low 8 bits = 0x06 for EVIO6)
            0x00000000,  // WORD 6: User Header Length (0)
            0xC0DA0100,  // WORD 7: Magic Number
            0x00000000,  // WORD 8: User Register low 32 bits
            0x00000000,  // WORD 9: User Register high 32 bits
            0x00000000,  // WORD 10: Trailer Position low 32 bits (0 if no trailer)
            0x00000000,  // WORD 11: Trailer Position high 32 bits
            0x00000000,  // WORD 12: User Integer 1
            0x00000000   // WORD 13: User Integer 2
        };

        // Convert to big-endian if needed
        for (int i = 0; i < 14; i++) {
            fileHeader[i] = htonl(fileHeader[i]);
        }

        // Write file header
        outputFile.write(reinterpret_cast<const char*>(fileHeader), 56);
        if (!outputFile) {
            std::cerr << "[" << threadName << "] Failed to write file header" << std::endl;
            return false;
        }

        currentFileSize += 56;
        return true;
    }

    bool openNextFile() {
        // NOTE: This function assumes the caller already holds fileMutex
        // It is called from writeToFile() which holds the lock

        // Close current file if open
        if (outputFile.is_open()) {
            outputFile.close();
        }

        // Generate filename: {prefix}_thread{N}_file{M}.evio
        std::ostringstream filename;
        filename << outputDir << "/" << outputPrefix
                 << "_thread" << threadIndex
                 << "_file" << std::setfill('0') << std::setw(4) << currentFileNumber
                 << ".evio";

        std::string filepath = filename.str();

        // Open file in binary mode
        outputFile.open(filepath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!outputFile) {
            std::cerr << "[" << threadName << "] Failed to open output file: "
                      << filepath << std::endl;
            return false;
        }

        currentFileSize = 0;
        filesCreated++;

        std::cout << "[" << threadName << "] Opened output file: " << filepath << std::endl;

        // Write EVIO-6 file header at the beginning of the file
        if (!writeFileHeader()) {
            std::cerr << "[" << threadName << "] Failed to write EVIO-6 file header" << std::endl;
            outputFile.close();
            return false;
        }

        return true;
    }

    /**
     * Check if file rollover is needed and perform it
     */
    bool checkAndRollover() {
        if (currentFileSize >= maxFileSize) {
            std::cout << "[" << threadName << "] File size limit reached ("
                      << currentFileSize << " bytes), rolling over..." << std::endl;
            currentFileNumber++;
            return openNextFile();
        }
        return true;
    }

    /**
     * Write frame data to file
     */
    bool writeToFile(const std::vector<uint8_t>& frameData) {
        std::lock_guard<std::mutex> lock(fileMutex);

        if (!outputFile.is_open()) {
            if (!openNextFile()) {
                std::cerr << "[" << threadName << "] Failed to open output file" << std::endl;
                buildErrors++;
                return false;
            }
        }

        // Write frame data
        outputFile.write(reinterpret_cast<const char*>(frameData.data()), frameData.size());

        if (!outputFile) {
            std::cerr << "[" << threadName << "] Error writing to file" << std::endl;
            buildErrors++;
            return false;
        }

        currentFileSize += frameData.size();
        bytesWritten += frameData.size();

        // Check if we need to rollover at 2GB
        if (currentFileSize >= maxFileSize) {
            std::cout << "[" << threadName << "] File size limit reached ("
                      << (currentFileSize / (1024*1024)) << " MB), rolling over to next file..." << std::endl;
            currentFileNumber++;
            if (!openNextFile()) {
                return false;
            }
        }

        return true;
    }

    /**
     * Close output file
     */
    void closeFile() {
        std::lock_guard<std::mutex> lock(fileMutex);
        if (outputFile.is_open()) {
            outputFile.flush();
            outputFile.close();
            std::cout << "[" << threadName << "] Closed output file" << std::endl;
        }
    }

    /**
     * Add a time slice to this builder's per-stream FIFO
     *
     * EVENT-NUMBER-BASED AGGREGATION:
     * - Each stream has its own FIFO queue
     * - Slices are enqueued to their stream's FIFO
     * - Builder thread will check alignment using corrected event numbers
     * - Timeout tracking uses corrected event numbers (after startup)
     */
    void addTimeSlice(TimeSlice&& slice) {
        std::lock_guard<std::mutex> lock(frameMutex);

        uint16_t streamId = slice.dataId;
        uint32_t rawEventNum = slice.frameNumber;

        // Compute corrected event number for timeout tracking
        // During startup before corrections are initialized, use raw event number
        uint32_t trackingEventNum = correctionFactorsInitialized ?
                                    getCorrectedEventNum(streamId, rawEventNum) :
                                    rawEventNum;

        // Track first arrival time for this corrected event number (for timeout)
        if (frameArrivalTimes.find(trackingEventNum) == frameArrivalTimes.end()) {
            frameArrivalTimes[trackingEventNum] = std::chrono::steady_clock::now();
        }

        // Enqueue slice to its stream's FIFO
        streamFIFOs[streamId].push(std::move(slice));
        slicesProcessed++;

        // Signal builder thread
        frameCV.notify_one();
    }

    /**
     * Build EVIO-6 aggregated time frame bank
     */
    bool buildEVIO6Frame(const AggregatedFrame& frame, std::vector<uint8_t>& output) {
        // ========================================================================
        // STEP 1: Validate and Process Input Frames
        // ========================================================================
        // Each slice payload contains:
        // - Words 1-8: CODA block header (word 8 = 0xc0da0100 magic)
        // - Words 9+: ROC bank data (to be extracted)

        int sliceCount = frame.slices.size();
        bool hasError = checkFrameNumberConsistency(frame);
        if (hasError) frameNumberErrors++;

        // Calculate timestamp average
        uint64_t tsAvg = calculateAverageTimestamp(frame);

        // Build stream status: bit 7 = error flag, bits 0-6 = slice count
        int streamStatus = ((hasError ? 1 : 0) << 7) | (sliceCount & 0x7F);

        // Validate slices and calculate total ROC data size (first pass)
        struct ValidatedSlice {
            const uint8_t* rocData;  // Points to payload + 32 bytes
            size_t rocSize;          // Size of ROC data (payload size - 32)
        };
        std::vector<ValidatedSlice> validatedSlices;
        validatedSlices.reserve(frame.slices.size());

        for (const auto& slice : frame.slices) {
            // Verify minimum size (8 words = 32 bytes)
            if (slice.payloadSize < 32) {
                std::cerr << "[" << threadName << "] ERROR: Payload too small (" << slice.payloadSize
                         << " bytes), need at least 32 bytes for CODA header" << std::endl;
                hasError = true;
                continue;
            }

            // Validate word 8 = 0xc0da0100 (BIG endian)
            // Word 8 is at byte offset 28 (7*4)
            const uint32_t* words = reinterpret_cast<const uint32_t*>(slice.payloadPtr.get());
            uint32_t magic = words[7];

            // Check if magic matches (either endianness - we'll accept both)
            if (magic != 0xc0da0100 && magic != 0x0001dac0) {
                std::cerr << "[" << threadName << "] ERROR: Invalid CODA magic number at word 8: 0x"
                         << std::hex << std::setfill('0') << std::setw(8) << magic << std::dec
                         << " (expected 0xc0da0100 or 0x0001dac0)" << std::endl;
                hasError = true;
                continue;
            }

            // Record validated slice (ROC data starts at byte 32)
            ValidatedSlice vs;
            vs.rocData = slice.payloadPtr.get() + 32;
            vs.rocSize = slice.payloadSize - 32;
            validatedSlices.push_back(vs);
        }

        if (validatedSlices.empty()) {
            std::cerr << "[" << threadName << "] ERROR: No valid ROC banks after CODA header validation" << std::endl;
            return false;
        }

        // ========================================================================
        // STEP 2: Build EVIO-6 Record Header (14 words)
        // ========================================================================
        std::vector<uint32_t> eventWords;

        eventWords.push_back(0);  // Word 0: recordLength (filled later)
        eventWords.push_back(framesBuilt + 1);  // Word 1: recordNumber (1-indexed count of successfully built frames)
        eventWords.push_back(14); // Word 2: headerLength (always 14 for EVIO-6)
        eventWords.push_back(1);  // Word 3: eventIndexCount (1 event per record)
        eventWords.push_back(0);  // Word 4: indexArrayLength (0 = no index)

        // Word 5: bitInfo = version | flags | byteOrder
        uint32_t bitInfo = 6 |           // version 6
                           (1 << 9) |     // last block flag
                           (1 << 14) |    // header type = EVIO record
                           (1U << 31);    // BIG endian (bit 31=1)
        eventWords.push_back(bitInfo);

        eventWords.push_back(0);           // Word 6: userHeaderLength
        eventWords.push_back(0xc0da0100);  // Word 7: magic number
        eventWords.push_back(0);           // Word 8: uncompressedDataLength (filled later)

        // Word 9: compressionType (24 bits) | compressedDataLength (8 bits)
        eventWords.push_back(0);           // No compression

        // Words 10-13: userRegisters (4 words = 2 x 64-bit registers)
        eventWords.push_back(0);
        eventWords.push_back(0);
        eventWords.push_back(0);
        eventWords.push_back(0);

        // ========================================================================
        // STEP 3: Build Aggregated Frame Bank (0xFF60 structure)
        // ========================================================================

        // Track where event data starts (after 14-word record header)
        size_t aggregatedBankLengthIndex = eventWords.size();
        eventWords.push_back(0);  // aggregatedBankLength (filled later)

        // Aggregated frame bank header: 0xFF60 (tag) | 0x10 (BANK type) | streamStatus
        uint32_t aggBankHeader = (0xFF60 << 16) | (0x10 << 8) | streamStatus;
        eventWords.push_back(aggBankHeader);

        // ========================================================================
        // STEP 4: Build Stream Info Bank (0xFF31 structure)
        // ========================================================================

        size_t streamInfoLengthIndex = eventWords.size();
        eventWords.push_back(0);  // streamInfoLength (filled later)

        // Stream info bank header: 0xFF31 (tag) | 0x20 (SEGMENT type) | streamStatus
        uint32_t streamInfoHeader = (0xFF31 << 16) | (0x20 << 8) | streamStatus;
        eventWords.push_back(streamInfoHeader);

        // --- Time Slice Segment (TSS) ---
        // Tag (8 bits) | type (8 bits) | length (16 bits)
        uint32_t tssHeader = (0x32 << 24) | (0x01 << 16) | 3;  // 3 words of data
        eventWords.push_back(tssHeader);

        // TSS data: frameNumber, timestamp (64-bit split into 2 words)
        eventWords.push_back(static_cast<uint32_t>(frame.frameNumber));
        eventWords.push_back(static_cast<uint32_t>(tsAvg & 0xFFFFFFFF));        // timestamp low
        eventWords.push_back(static_cast<uint32_t>((tsAvg >> 32) & 0xFFFFFFFF)); // timestamp high

        // --- Aggregation Info Segment (AIS) ---
        // Tag (8 bits) | type (8 bits) | length (16 bits)
        uint32_t aisHeader = (0x42 << 24) | (0x01 << 16) | sliceCount;
        eventWords.push_back(aisHeader);

        // AIS data: ROC IDs (one word per ROC)
        // Format per word: ROC_ID (16 bits) | reserved (8 bits) | stream_status (8 bits)
        for (const auto& slice : frame.slices) {
            uint32_t aisEntry = (slice.dataId << 16) | slice.streamStatus;
            eventWords.push_back(aisEntry);
        }

        // Calculate streamInfoLength (words after length field up to here)
        size_t streamInfoLength = eventWords.size() - streamInfoLengthIndex - 1;
        eventWords[streamInfoLengthIndex] = static_cast<uint32_t>(streamInfoLength);

        // ========================================================================
        // STEP 5: Calculate Total Payload Size
        // ========================================================================

        size_t totalPayloadBytes = 0;
        for (const auto& vs : validatedSlices) {
            totalPayloadBytes += vs.rocSize;
            // Account for padding to 4-byte boundary
            if (vs.rocSize % 4 != 0) {
                totalPayloadBytes += 4 - (vs.rocSize % 4);
            }
        }

        size_t totalPayloadWords = totalPayloadBytes / 4;

        // ========================================================================
        // STEP 6: Fill in Length Fields
        // ========================================================================

        // aggregatedBankLength = all words after this field
        size_t aggregatedBankLength = (eventWords.size() - aggregatedBankLengthIndex - 1) + totalPayloadWords;
        eventWords[aggregatedBankLengthIndex] = static_cast<uint32_t>(aggregatedBankLength);

        // recordLength = total words in record (header + event data)
        size_t recordLength = 14 + aggregatedBankLength + 1;  // +1 for aggregatedBankLength field itself
        eventWords[0] = static_cast<uint32_t>(recordLength);

        // uncompressedDataLength = bytes after record header (EVIO6 spec: word 8 must be in bytes)
        size_t uncompressedDataLength = recordLength - 14;
        eventWords[8] = static_cast<uint32_t>(uncompressedDataLength * 4);  // Convert words to bytes

        // ========================================================================
        // STEP 7: Byte Swap to BIG Endian and Write Header/Metadata
        // ========================================================================

        // Pre-allocate full output size to avoid repeated reallocations
        size_t totalOutputSize = eventWords.size() * 4 + totalPayloadBytes;
        output.reserve(totalOutputSize);
        output.resize(eventWords.size() * 4);
        uint32_t* outputWords = reinterpret_cast<uint32_t*>(output.data());

        // Byte swap to BIG endian
        for (size_t i = 0; i < eventWords.size(); i++) {
            uint32_t val = eventWords[i];
            outputWords[i] = ((val & 0x000000FF) << 24) |
                            ((val & 0x0000FF00) << 8) |
                            ((val & 0x00FF0000) >> 8) |
                            ((val & 0xFF000000) >> 24);
        }

        // ========================================================================
        // STEP 8: Append ROC Banks Directly (Preserve Original Endianness)
        // ========================================================================

        for (const auto& vs : validatedSlices) {
            size_t currentSize = output.size();
            output.resize(currentSize + vs.rocSize);
            std::memcpy(output.data() + currentSize,
                       vs.rocData,
                       vs.rocSize);

            // Pad to 4-byte boundary if needed
            while (output.size() % 4 != 0) {
                output.push_back(0);
            }
        }

        return !hasError;
    }

    /**
     * Check frame number consistency across slices
     *
     * DATA QUALITY VALIDATION (not aggregation):
     * All slices in this frame were aggregated by corrected event number (exact match).
     * This function validates that all corrected event numbers are within the slop range
     * of the frame's corrected event number, as a sanity check on data quality.
     *
     * @return true if frame numbers are inconsistent (exceeds slop), false if OK
     */
    bool checkFrameNumberConsistency(const AggregatedFrame& frame) const {
        if (frame.slices.empty()) return false;
        if (frameNumberSlop <= 0) return false;  // Slop of 0 means no checking

        uint32_t targetCorrectedEventNum = frame.frameNumber;  // This is the corrected event number

        for (const auto& slice : frame.slices) {
            uint32_t rawEventNum = slice.frameNumber;
            uint32_t correctedEventNum = const_cast<BuilderThread*>(this)->getCorrectedEventNum(slice.dataId, rawEventNum);

            int64_t diff = std::abs(static_cast<int64_t>(correctedEventNum) - static_cast<int64_t>(targetCorrectedEventNum));
            if (diff > frameNumberSlop) {
                std::cerr << "[" << threadName << "] WARNING: Frame number inconsistency in corrected event "
                          << targetCorrectedEventNum << "! "
                          << "Stream " << slice.dataId << " has correctedEventNum=" << correctedEventNum
                          << ", Diff=" << diff
                          << ", Allowed=" << frameNumberSlop << std::endl;
                return true;  // Error detected
            }
        }

        return false;
    }

    /**
     * Calculate average timestamp
     */
    uint64_t calculateAverageTimestamp(const AggregatedFrame& frame) const {
        if (frame.slices.empty()) return 0;

        uint64_t total = 0;
        for (const auto& slice : frame.slices) {
            total += slice.timestamp;
        }

        return total / frame.slices.size();
    }

    /**
     * Send built frame to ET system
     */
    bool sendToET(const std::vector<uint8_t>& frameData) {
        et_event* events[1];
        int numEvents = 1;
        int status;

        // Get new ET events
        struct timespec timeout;
        timeout.tv_sec = 2;
        timeout.tv_nsec = 0;
        int numRead = 0;

        status = et_events_new(etSystem, etAttachment, events, ET_TIMED,
                               &timeout, etEventSize, numEvents, &numRead);
        if (status != ET_OK) {
            std::cerr << "[" << threadName << "] Failed to get new ET event: "
                      << status << std::endl;
            buildErrors++;
            return false;
        }

        // Copy data to ET event
        void* eventData;
        size_t eventLength;
        et_event_getdata(events[0], &eventData);
        et_event_getlength(events[0], &eventLength);

        if (frameData.size() > eventLength) {
            std::cerr << "[" << threadName << "] Frame data too large for ET event: "
                      << frameData.size() << " > " << eventLength << std::endl;
            et_events_dump(etSystem, etAttachment, events, numEvents);
            buildErrors++;
            return false;
        }

        std::memcpy(eventData, frameData.data(), frameData.size());
        et_event_setlength(events[0], frameData.size());

        // Put event back to ET system
        status = et_events_put(etSystem, etAttachment, events, numEvents);
        if (status != ET_OK) {
            std::cerr << "[" << threadName << "] Failed to put ET event: "
                      << status << std::endl;
            buildErrors++;
            return false;
        }

        framesBuilt++;
        bytesWritten += frameData.size();
        return true;
    }

    /**
     * Get the minimum CORRECTED event number across all non-empty stream FIFOs
     * Returns {found, minCorrectedEventNum}
     *
     * Uses correction factors to align event numbers from different streams.
     */
    std::pair<bool, uint32_t> getMinimumFrameNumber() {
        // NOTE: Caller must hold frameMutex
        uint32_t minFrame = UINT32_MAX;
        bool found = false;

        for (const auto& [streamId, fifo] : streamFIFOs) {
            if (!fifo.empty()) {
                uint32_t rawEventNum = fifo.front().frameNumber;
                uint32_t correctedEventNum = getCorrectedEventNum(streamId, rawEventNum);
                minFrame = std::min(minFrame, correctedEventNum);
                found = true;
            }
        }

        return {found, minFrame};
    }

    /**
     * Check if all non-empty stream FIFOs are aligned on the given CORRECTED event number
     * Returns: {aligned, list of stream IDs that have this corrected event number}
     *
     * Uses correction factors to compare event numbers after alignment.
     */
    std::pair<bool, std::vector<uint16_t>> checkAlignment(uint32_t correctedEventNum) {
        // NOTE: Caller must hold frameMutex
        std::vector<uint16_t> alignedStreams;
        bool allAligned = true;

        for (const auto& [streamId, fifo] : streamFIFOs) {
            if (!fifo.empty()) {
                uint32_t rawEventNum = fifo.front().frameNumber;
                uint32_t streamCorrectedEventNum = getCorrectedEventNum(streamId, rawEventNum);

                if (streamCorrectedEventNum == correctedEventNum) {
                    alignedStreams.push_back(streamId);
                } else {
                    // Found a stream with different corrected event number
                    allAligned = false;
                }
            }
        }

        return {allAligned, alignedStreams};
    }

    /**
     * Check if a frame number has timed out
     */
    bool hasFrameTimedOut(uint32_t frameNumber) {
        // NOTE: Caller must hold frameMutex
        auto it = frameArrivalTimes.find(frameNumber);
        if (it == frameArrivalTimes.end()) {
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
        return elapsed.count() > frameTimeoutMs;
    }

    /**
     * Compute per-stream event number correction factors at startup
     *
     * STARTUP CORRECTION ALGORITHM:
     * 1. Wait until we have at least one frame from each expected stream
     * 2. Find the minimum event number across all streams
     * 3. Compute correction offset for each stream = (minEventNum - streamEventNum)
     * 4. These offsets remain constant for the entire run
     *
     * This ensures all streams are aligned to the same event number sequence.
     * NOTE: Caller must hold frameMutex
     */
    void computeInitialCorrections() {
        // Check if we have data from all expected streams
        if (streamFIFOs.size() < static_cast<size_t>(expectedStreamCount)) {
            // Not enough streams yet, wait for more data
            return;
        }

        // Make sure all streams have at least one frame
        for (const auto& [streamId, fifo] : streamFIFOs) {
            if (fifo.empty()) {
                return;  // Still waiting for data from this stream
            }
        }

        // Find the minimum event number across all streams
        minInitialEventNum = UINT32_MAX;
        for (const auto& [streamId, fifo] : streamFIFOs) {
            if (!fifo.empty()) {
                uint32_t streamEventNum = fifo.front().frameNumber;
                minInitialEventNum = std::min(minInitialEventNum, streamEventNum);
            }
        }

        // Compute correction factor for each stream
        std::cout << "[" << threadName << "] Computing initial event number correction factors:" << std::endl;
        std::cout << "[" << threadName << "] Minimum initial event number: " << minInitialEventNum << std::endl;

        for (const auto& [streamId, fifo] : streamFIFOs) {
            if (!fifo.empty()) {
                uint32_t streamEventNum = fifo.front().frameNumber;
                int64_t correction = static_cast<int64_t>(minInitialEventNum) - static_cast<int64_t>(streamEventNum);
                streamEventNumCorrections[streamId] = correction;

                std::cout << "[" << threadName << "]   Stream " << streamId
                          << ": Raw EventNum=" << streamEventNum
                          << ", Correction=" << (correction >= 0 ? "+" : "") << correction
                          << ", Corrected=" << (streamEventNum + correction) << std::endl;
            }
        }

        correctionFactorsInitialized = true;
        std::cout << "[" << threadName << "] Correction factors initialized (fixed for this run)" << std::endl;
    }

    /**
     * Get corrected event number for a stream
     *
     * Applies the per-stream correction factor computed at startup.
     * If stream has no correction (new stream after startup), defaults to 0 offset.
     *
     * @param streamId  Stream identifier
     * @param rawEventNum  Raw event number from reassembler
     * @return Corrected event number
     */
    uint32_t getCorrectedEventNum(uint16_t streamId, uint32_t rawEventNum) {
        auto it = streamEventNumCorrections.find(streamId);
        if (it != streamEventNumCorrections.end()) {
            return rawEventNum + it->second;
        }
        // New stream not seen at startup - no correction
        return rawEventNum;
    }

    /**
     * Builder thread main loop - ALIGNMENT-BASED FRAME BUILDING
     *
     * ALGORITHM:
     * 1. Find minimum frame number across all stream FIFOs
     * 2. Check if all streams are aligned (all have this frame number)
     * 3. If aligned → consume from all streams and build complete frame
     * 4. If NOT aligned → consume only from streams with minimum frame number
     * 5. Handle timeout → force build if waiting too long
     */
    void threadFunc() {
        while (true) {
            std::unique_lock<std::mutex> lock(frameMutex);

            // Wait for data to arrive or check periodically
            frameCV.wait_for(lock, std::chrono::milliseconds(frameTimeoutMs / 2),
                [this]() {
                    // Wake up if any stream has data OR we're stopping
                    for (const auto& [streamId, fifo] : streamFIFOs) {
                        if (!fifo.empty()) return true;
                    }
                    return !running;
                });

            // Exit immediately if stopped
            if (!running) {
                break;
            }

            // ================================================================
            // STARTUP: COMPUTE CORRECTION FACTORS (ONCE)
            // ================================================================
            // On first frames from all streams, compute per-stream event number
            // correction factors to align them. These remain constant for the run.
            if (!correctionFactorsInitialized) {
                computeInitialCorrections();
                if (!correctionFactorsInitialized) {
                    // Still waiting for all streams to send first frame
                    continue;
                }
            }

            // ================================================================
            // ALIGNMENT-BASED FRAME BUILDING ALGORITHM (using corrected event numbers)
            // ================================================================

            // Step 1: Find minimum CORRECTED event number across all non-empty FIFOs
            auto [hasData, minCorrectedEventNum] = getMinimumFrameNumber();

            if (!hasData) {
                // No data available, wait for more
                continue;
            }

            // Step 2: Check alignment - which streams have this minimum corrected event number?
            auto [allAligned, streamsWithMinFrame] = checkAlignment(minCorrectedEventNum);

            if (streamsWithMinFrame.empty()) {
                // Should never happen, but handle gracefully
                continue;
            }

            // Step 3: Determine if we should build a frame
            bool shouldBuild = false;
            bool isComplete = false;
            bool isTimeout = hasFrameTimedOut(minCorrectedEventNum);

            if (allAligned && streamsWithMinFrame.size() >= static_cast<size_t>(expectedStreamCount)) {
                // CASE 1: All streams aligned AND all expected streams present
                shouldBuild = true;
                isComplete = true;
            } else if (allAligned && isTimeout) {
                // CASE 2: All present streams aligned, but some missing and timed out
                shouldBuild = true;
                isComplete = false;
            } else if (!allAligned) {
                // CASE 3: NOT aligned - advance only the lagging streams (with min corrected event number)
                shouldBuild = true;
                isComplete = false;
            }

            if (!shouldBuild) {
                // Wait for more data or timeout
                continue;
            }

            // Step 4: Consume slices from appropriate streams
            // - If aligned: consume from ALL streams with this corrected event number
            // - If NOT aligned: consume ONLY from streams with minimum corrected event number (lagging streams)
            AggregatedFrame aggregatedFrame;
            aggregatedFrame.frameNumber = minCorrectedEventNum;  // Use corrected event number
            aggregatedFrame.timestamp = 0;  // Will calculate average

            for (uint16_t streamId : streamsWithMinFrame) {
                auto& fifo = streamFIFOs[streamId];
                if (!fifo.empty()) {
                    uint32_t rawEventNum = fifo.front().frameNumber;
                    uint32_t correctedEventNum = getCorrectedEventNum(streamId, rawEventNum);

                    if (correctedEventNum == minCorrectedEventNum) {
                        // Pop slice from this stream's FIFO
                        TimeSlice slice = std::move(fifo.front());
                        fifo.pop();
                        aggregatedFrame.addSlice(std::move(slice));
                    }
                }
            }

            // Clean up timeout tracking for this corrected event number if all streams consumed it
            if (allAligned) {
                frameArrivalTimes.erase(minCorrectedEventNum);
            }

            // Release lock before expensive build/send operations
            lock.unlock();

            // Step 5: Log what we're doing (using corrected event number)
            if (allAligned && isComplete) {
                std::cout << "[" << threadName << "] CorrectedEventNum " << minCorrectedEventNum
                          << ": ALIGNED & COMPLETE (" << aggregatedFrame.slices.size()
                          << "/" << expectedStreamCount << " streams)" << std::endl;
            } else if (allAligned && !isComplete) {
                std::cout << "[" << threadName << "] CorrectedEventNum " << minCorrectedEventNum
                          << ": ALIGNED but PARTIAL (" << aggregatedFrame.slices.size()
                          << "/" << expectedStreamCount << " streams) - TIMEOUT" << std::endl;
            } else {
                std::cout << "[" << threadName << "] CorrectedEventNum " << minCorrectedEventNum
                          << ": NOT ALIGNED - advancing " << aggregatedFrame.slices.size()
                          << " lagging stream(s)" << std::endl;
            }

            // Check if we should stop before expensive operations
            if (!running) {
                lock.lock();
                break;
            }

            // Step 6: Build EVIO-6 frame
            std::vector<uint8_t> builtFrame;
            if (buildEVIO6Frame(aggregatedFrame, builtFrame)) {
                bool success = true;

                // Check if we should stop before ET/file operations
                if (!running) {
                    lock.lock();
                    break;
                }

                // Send to ET if enabled
                if (useET && running) {
                    success = sendToET(builtFrame) && success;

                    // Check again after potentially blocking ET call
                    if (!running) {
                        lock.lock();
                        break;
                    }
                }

                // Write to file if enabled
                if (useFileOutput && running) {
                    success = writeToFile(builtFrame) && success;
                }

                if (success) {
                    framesBuilt++;
                }
            }

            // Reacquire lock for next iteration
            lock.lock();
        }

        std::cout << "[" << threadName << "] Builder thread stopped" << std::endl;
    }

    /**
     * Start the builder thread
     */
    void start() {
        running = true;
        thread = std::thread(&BuilderThread::threadFunc, this);
    }

    /**
     * Signal the thread to stop (non-blocking)
     */
    void signalStop() {
        running = false;

        // Notify aggressively - call multiple times with small delays
        // to ensure the thread wakes up even if it's in the middle of a wait_for timeout
        for (int i = 0; i < 5; i++) {
            frameCV.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    /**
     * Wait for the thread to finish (with timeout and detach fallback)
     */
    void waitForStop() {
        if (!thread.joinable()) {
            return;
        }

        // Give thread up to 1 second to finish current operation and exit
        // We can't truly "timeout" on join(), so we wait then detach as fallback
        auto start = std::chrono::steady_clock::now();
        const int max_wait_ms = 1000;

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            if (elapsed.count() > max_wait_ms) {
                // Timeout - detach and let OS clean up
                thread.detach();
                return;
            }
        }
    }

    /**
     * Stop the builder thread (calls both signal and wait)
     */
    void stop() {
        signalStop();
        waitForStop();
    }

    /**
     * Get statistics
     */
    void getStats(uint64_t& built, uint64_t& slices, uint64_t& errors, uint64_t& fnErrors,
                  uint64_t& files, uint64_t& bytes) const {
        built = framesBuilt;
        slices = slicesProcessed;
        errors = buildErrors;
        fnErrors = frameNumberErrors;
        files = filesCreated;
        bytes = bytesWritten;
    }

    bool isRunning() const { return running; }
};

/**
 * FrameBuilder Constructor
 */
FrameBuilder::FrameBuilder(const std::string& etFile,
             const std::string& etHost,
             int etPort,
             const std::string& fileDir,
             const std::string& filePrefix,
             int numBuilderThreads,
             int eventSize,
             int fnSlop,
             int timeout,
             int numExpectedStreams)
    : etSystemFile(etFile)
    , etHostName(etHost)
    , etPort(etPort)
    , etEventSize(eventSize)
    , enableET(!etFile.empty())
    , enableFileOutput(!fileDir.empty())
    , fileOutputDir(fileDir)
    , fileOutputPrefix(filePrefix)
    , builderThreadCount(numBuilderThreads)
    , frameNumberSlop(fnSlop)
    , frameTimeoutMs(timeout)
    , expectedStreams(numExpectedStreams)
{
    etSystem = nullptr;

    // Show configuration
    std::cout << "Frame builder configuration:" << std::endl;
    std::cout << "  ET output: " << (enableET ? "enabled" : "disabled");
    if (enableET) {
        std::cout << " (file: " << etSystemFile << ")";
    }
    std::cout << std::endl;
    std::cout << "  File output: " << (enableFileOutput ? "enabled" : "disabled");
    if (enableFileOutput) {
        std::cout << " (dir: " << fileOutputDir << ", prefix: " << fileOutputPrefix << ")";
    }
    std::cout << std::endl;
    std::cout << "  Expected streams: " << expectedStreams << std::endl;
    std::cout << "  Frame timeout: " << frameTimeoutMs << " ms" << std::endl;

    // Validate that at least one output is enabled
    if (!enableET && !enableFileOutput) {
        std::cerr << "ERROR: At least one output (ET or file) must be enabled" << std::endl;
        throw std::invalid_argument("No output enabled");
    }
}

/**
 * FrameBuilder Destructor
 */
FrameBuilder::~FrameBuilder() {
    if (running) {
        stop();
    }
}

/**
 * Initialize ET system connection and create multiple attachments
 */
bool FrameBuilder::initializeET() {
    // Skip ET initialization if not enabled
    if (!enableET) {
        std::cout << "ET output disabled, skipping ET initialization" << std::endl;
        return true;
    }

    et_openconfig openConfig;
    int status;

    std::cout << "Initializing ET system with " << builderThreadCount
              << " builder threads..." << std::endl;
    std::cout << "  ET file: " << etSystemFile << std::endl;
    if (!etHostName.empty()) {
        std::cout << "  ET host: " << etHostName << std::endl;
    }
    if (etPort > 0) {
        std::cout << "  ET port: " << etPort << std::endl;
    }

    // Initialize open configuration
    status = et_open_config_init(&openConfig);
    if (status != ET_OK) {
        std::cerr << "Failed to initialize ET open config: " << status << std::endl;
        return false;
    }

    // Configure host if specified
    if (!etHostName.empty()) {
        et_open_config_sethost(openConfig, etHostName.c_str());
        // If host is specified, use direct connection instead of broadcast
        et_open_config_setcast(openConfig, ET_DIRECT);
    } else {
        // Use broadcast to find ET system
        et_open_config_setcast(openConfig, ET_BROADCAST);
    }

    // Configure port if specified
    if (etPort > 0) {
        et_open_config_setserverport(openConfig, etPort);
    }

    // Wait for ET system if not immediately available
    et_open_config_setwait(openConfig, ET_OPEN_WAIT);

    // Set timeout for connection attempts
    struct timespec timeout;
    timeout.tv_sec = 10;  // 10 second timeout
    timeout.tv_nsec = 0;
    et_open_config_settimeout(openConfig, timeout);

    // Open ET system
    status = et_open(&etSystem, etSystemFile.c_str(), openConfig);
    et_open_config_destroy(openConfig);

    if (status != ET_OK) {
        std::cerr << "Failed to open ET system '" << etSystemFile << "'";
        if (!etHostName.empty()) {
            std::cerr << " on host '" << etHostName << "'";
        }
        if (etPort > 0) {
            std::cerr << " port " << etPort;
        }
        std::cerr << ": " << status << std::endl;
        return false;
    }

    std::cout << "Successfully opened ET system: " << etSystemFile;
    if (!etHostName.empty()) {
        std::cout << " on " << etHostName;
    }
    if (etPort > 0) {
        std::cout << ":" << etPort;
    }
    std::cout << std::endl;

    // Attach to Grand Central station (ID 0) for injecting events
    // Frame builder acts as ET producer, not consumer
    std::cout << "  Attaching to GRAND_CENTRAL station (ID 0) for event injection" << std::endl;

    // Create multiple attachments (one per builder thread)
    for (int i = 0; i < builderThreadCount; i++) {
        et_att_id attachment;
        status = et_station_attach(etSystem, 0, &attachment);  // Station ID 0 = Grand Central
        if (status != ET_OK) {
            std::cerr << "Failed to attach to GRAND_CENTRAL (thread " << i << "): "
                      << status << std::endl;
            // Clean up existing attachments
            for (auto att : etAttachments) {
                et_station_detach(etSystem, att);
            }
            et_close(etSystem);
            return false;
        }
        etAttachments.push_back(attachment);
        std::cout << "Created ET attachment " << i << " to GRAND_CENTRAL" << std::endl;
    }

    std::cout << "Successfully attached to GRAND_CENTRAL station with "
              << builderThreadCount << " attachments" << std::endl;
    return true;
}

/**
 * Add a reassembled time slice - distributes to appropriate builder thread's per-stream FIFO
 *
 * THREAD DISTRIBUTION BY FRAME NUMBER:
 * All slices with the same frame number go to the same builder thread.
 * This ensures proper alignment checking without cross-thread coordination.
 *
 * STREAM FIFO ARCHITECTURE:
 * Each builder thread maintains separate FIFO queues for each stream.
 * Slices are enqueued to their stream's FIFO and processed by the alignment algorithm.
 */
void FrameBuilder::addTimeSlice(uint64_t timestamp, uint32_t frameNumber, uint16_t dataId,
                  uint8_t* data, size_t dataLen) {

    // Hash frame number to determine which builder thread handles this frame
    // CRITICAL: Use frame number (not timestamp) so all slices of same frame go to same thread
    int threadIndex = static_cast<int>(frameNumber % builderThreadCount);

    // Create time slice (transfers ownership of data buffer)
    TimeSlice slice(timestamp, frameNumber, dataId, data, dataLen);

    // Send to appropriate builder thread's per-stream FIFO (move to avoid copy)
    builderThreads[threadIndex]->addTimeSlice(std::move(slice));
    slicesAggregated++;
}

/**
 * Start all builder threads
 */
bool FrameBuilder::start() {
    // Initialize ET system if enabled
    if (enableET) {
        if (!initializeET()) {
            return false;
        }
    }

    // Create output directory if file output is enabled
    if (enableFileOutput) {
        namespace fs = std::filesystem;
        fs::path outputPath(fileOutputDir);

        if (!fs::exists(outputPath)) {
            std::error_code ec;
            if (!fs::create_directories(outputPath, ec)) {
                std::cerr << "Failed to create output directory '" << fileOutputDir
                          << "': " << ec.message() << std::endl;
                return false;
            }
            std::cout << "Created output directory: " << fileOutputDir << std::endl;
        }
    }

    // Print configuration
    std::cout << "Starting frame builder with " << builderThreadCount
              << " parallel threads" << std::endl;
    if (enableET) {
        std::cout << "  ET output: enabled (GRAND_CENTRAL station)" << std::endl;
    }
    if (enableFileOutput) {
        std::cout << "  File output: enabled (dir: " << fileOutputDir
                  << ", prefix: " << fileOutputPrefix << ")" << std::endl;
    }

    // Create and start builder threads
    for (int i = 0; i < builderThreadCount; i++) {
        et_att_id attachment = enableET ? etAttachments[i] : 0;

        auto builder = std::make_unique<BuilderThread>(
            i,
            builderThreadCount,
            etSystem,
            attachment,
            frameNumberSlop,
            frameTimeoutMs,
            etEventSize,
            enableET,
            enableFileOutput,
            fileOutputDir,
            fileOutputPrefix,
            expectedStreams
        );
        builder->start();
        builderThreads.push_back(std::move(builder));
    }

    running = true;
    std::cout << "Frame builder started successfully" << std::endl;
    return true;
}

/**
 * Stop all builder threads
 */
void FrameBuilder::stop() {
    std::cout << "Stopping frame builder..." << std::endl;

    running = false;

    // First, signal all threads to stop (set their running flags)
    for (auto& builder : builderThreads) {
        builder->signalStop();
    }

    // Then, wait for all threads to finish
    for (auto& builder : builderThreads) {
        builder->waitForStop();
    }

    std::cout << "All builder threads finished" << std::endl;

    // Collect statistics from all threads
    framesBuilt = 0;
    buildErrors = 0;
    frameNumberErrors = 0;
    filesCreated = 0;
    bytesWritten = 0;

    std::cout << "Collecting statistics..." << std::endl;

    for (auto& builder : builderThreads) {
        uint64_t built, slices, errors, fnErrors, files, bytes;
        builder->getStats(built, slices, errors, fnErrors, files, bytes);
        framesBuilt += built;
        buildErrors += errors;
        frameNumberErrors += fnErrors;
        filesCreated += files;
        bytesWritten += bytes;
    }

    std::cout << "Statistics collected" << std::endl;

    // NOTE: Do not call builderThreads.clear() here!
    // If any thread was detached (forced timeout), clearing the vector
    // would invoke the BuilderThread destructor while the thread is still
    // running, causing undefined behavior. The FrameBuilder destructor
    // will handle cleanup when the object is destroyed at program exit.
    // builderThreads.clear();

    // Detach and close ET if enabled
    if (enableET && etSystem) {
        for (auto attachment : etAttachments) {
            et_station_detach(etSystem, attachment);
        }
        etAttachments.clear();

        et_close(etSystem);
        etSystem = nullptr;
    }

    std::cout << "Frame builder stopped" << std::endl;
    printStatistics();
}

/**
 * Print statistics
 */
void FrameBuilder::printStatistics() const {
    std::cout << "\n=== Frame Builder Statistics ===" << std::endl;
    std::cout << "  Builder Threads: " << builderThreadCount << std::endl;
    std::cout << "  Frames Built: " << framesBuilt << std::endl;
    std::cout << "  Slices Aggregated: " << slicesAggregated << std::endl;
    std::cout << "  Build Errors: " << buildErrors << std::endl;
    std::cout << "  Frame Number Errors: " << frameNumberErrors << std::endl;
    if (slicesAggregated > 0 && framesBuilt > 0) {
        std::cout << "  Avg Slices/Frame: "
                  << (static_cast<double>(slicesAggregated) / framesBuilt)
                  << std::endl;
    }
    if (enableFileOutput) {
        std::cout << "  Files Created: " << filesCreated << std::endl;
        std::cout << "  Bytes Written: " << bytesWritten;
        if (bytesWritten >= 1024*1024*1024) {
            std::cout << " (" << (bytesWritten / (1024.0*1024.0*1024.0)) << " GB)";
        } else if (bytesWritten >= 1024*1024) {
            std::cout << " (" << (bytesWritten / (1024.0*1024.0)) << " MB)";
        }
        std::cout << std::endl;
    }
    std::cout << "=================================" << std::endl;
}

/**
 * Get current statistics (aggregates from all threads on-demand)
 */
void FrameBuilder::getStatistics(uint64_t& built, uint64_t& slices, uint64_t& errors, uint64_t& bytes) const {
    // Aggregate current statistics from all builder threads
    built = 0;
    slices = 0;
    errors = 0;
    bytes = 0;

    for (const auto& builder : builderThreads) {
        uint64_t threadBuilt, threadSlices, threadErrors, threadTsErrors, threadFiles, threadBytes;
        builder->getStats(threadBuilt, threadSlices, threadErrors, threadTsErrors, threadFiles, threadBytes);
        built += threadBuilt;
        slices += threadSlices;
        errors += threadErrors;
        bytes += threadBytes;
    }

    // Add the FrameBuilder-level slice count
    slices += slicesAggregated.load();
}

} // namespace e2sar
