/**
 * EVIO6 Event Parser and Validator
 *
 * This program reads and validates EVIO6-format frame-built event files
 * produced by coda-fb. It parses the hierarchical EVIO structure, validates
 * against both the CODA Online Data Formats specification (coda_data_format.pdf)
 * and the actual coda-fb writer implementation.
 *
 * Expected Structure (from coda-fb):
 *  - File Header (14 words, EVIO6 format)
 *  - Records (each with 14-word header + event data)
 *    - Aggregated Frame Bank (tag 0xFF60, type 0x10)
 *      - Stream Info Bank (tag 0xFF31, type 0x20)
 *        - Time Slice Segment (tag 0x32, type 0x01)
 *        - Aggregation Info Segment (tag 0x42, type 0x01)
 *      - ROC Payload Banks (one per source)
 *
 * Copyright (c) 2024, Jefferson Science Associates
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <string>
#include <sstream>
#include <algorithm>

// EVIO6 Constants
namespace EVIO6 {
    constexpr uint32_t FILE_ID_EVIO = 0x4556494F;  // "EVIO" in ASCII
    constexpr uint32_t MAGIC_NUMBER = 0xC0DA0100;  // Big-endian magic
    constexpr uint32_t MAGIC_NUMBER_LE = 0x0001DAC0;  // Little-endian magic
    constexpr uint32_t HEADER_LENGTH = 14;  // words
    constexpr uint8_t  VERSION = 6;

    // CODA Tags (from coda-fb implementation)
    constexpr uint16_t TAG_AGG_FRAME = 0xFF60;   // Aggregated frame bank
    constexpr uint16_t TAG_STREAM_INFO = 0xFF31; // Stream Info Bank
    constexpr uint8_t  TAG_TIME_SLICE = 0x32;    // Time Slice Segment
    constexpr uint8_t  TAG_AGG_INFO = 0x42;      // Aggregation Info Segment
    constexpr uint16_t TAG_ROC_BANK = 0xFF30;    // ROC Time Slice Bank

    // EVIO Data Types
    constexpr uint8_t TYPE_BANK = 0x10;
    constexpr uint8_t TYPE_SEGMENT = 0x20;
    constexpr uint8_t TYPE_INT = 0x01;
}

// Utility functions for byte swapping (big-endian <-> host)
uint32_t ntoh32(uint32_t net) {
    return ((net & 0x000000FF) << 24) |
           ((net & 0x0000FF00) << 8) |
           ((net & 0x00FF0000) >> 8) |
           ((net & 0xFF000000) >> 24);
}

uint64_t ntoh64(uint64_t net) {
    uint32_t high = ntoh32(static_cast<uint32_t>(net & 0xFFFFFFFF));
    uint32_t low = ntoh32(static_cast<uint32_t>((net >> 32) & 0xFFFFFFFF));
    return (static_cast<uint64_t>(high) << 32) | low;
}

// FADC250 Hit data structure
struct FADCHit {
    int crate;        // ROC ID
    int slot;         // Payload ID (slot number)
    int channel;      // Channel number (0-15)
    int charge;       // Integrated charge (13 bits)
    uint64_t time;    // Absolute hit time in nanoseconds

    FADCHit(int c, int s, int ch, int q, uint64_t t)
        : crate(c), slot(s), channel(ch), charge(q), time(t) {}

    FADCHit() : crate(0), slot(0), channel(0), charge(0), time(0) {}
};

// Validation result tracking
struct ValidationResult {
    bool success = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    void addError(const std::string& msg) {
        errors.push_back(msg);
        success = false;
    }

    void addWarning(const std::string& msg) {
        warnings.push_back(msg);
    }

    void print() const {
        std::cout << "\n=== Validation Summary ===\n";
        std::cout << "Status: " << (success ? "SUCCESS" : "FAILED") << "\n";
        std::cout << "Errors: " << errors.size() << "\n";
        std::cout << "Warnings: " << warnings.size() << "\n";

        if (!warnings.empty()) {
            std::cout << "\nWarnings:\n";
            for (const auto& w : warnings) {
                std::cout << "  [WARN] " << w << "\n";
            }
        }

        if (!errors.empty()) {
            std::cout << "\nErrors:\n";
            for (const auto& e : errors) {
                std::cout << "  [ERROR] " << e << "\n";
            }
        }
        std::cout << "==========================\n";
    }
};

class EVIO6Parser {
private:
    std::vector<uint8_t> fileData;
    size_t currentPos = 0;
    ValidationResult result;
    bool verbose = false;
    bool fadcVerbose = false;
    int recordCount = 0;
    uint64_t currentFrameTimestamp = 0;
    std::vector<FADCHit> currentEventHits;
    std::vector<int> currentEventROCIds;

    // Read 32-bit word at current position (big-endian)
    uint32_t read32() {
        if (currentPos + 4 > fileData.size()) {
            result.addError("Unexpected end of file at offset " +
                          std::to_string(currentPos));
            return 0;
        }

        uint32_t val = 0;
        std::memcpy(&val, &fileData[currentPos], 4);
        currentPos += 4;
        return ntoh32(val);  // Convert from big-endian
    }

    // Peek at 32-bit word without advancing position
    uint32_t peek32(size_t offset = 0) const {
        if (currentPos + offset + 4 > fileData.size()) {
            return 0;
        }

        uint32_t val = 0;
        std::memcpy(&val, &fileData[currentPos + offset], 4);
        return ntoh32(val);
    }

    // Read 64-bit word (big-endian)
    uint64_t read64() {
        uint32_t low = read32();
        uint32_t high = read32();
        return (static_cast<uint64_t>(high) << 32) | low;
    }

    void printIndent(int level) const {
        for (int i = 0; i < level; i++) std::cout << "  ";
    }

    void printHeader(const std::string& title, int level = 0) const {
        if (!verbose) return;
        printIndent(level);
        std::cout << "=== " << title << " ===\n";
    }

    void printField(const std::string& name, uint64_t value,
                   const std::string& extra = "", int level = 0) const {
        if (!verbose) return;
        printIndent(level);
        std::cout << name << ": " << value;
        if (!extra.empty()) {
            std::cout << " (" << extra << ")";
        }
        std::cout << "\n";
    }

    void printHex(const std::string& name, uint32_t value, int level = 0) const {
        if (!verbose) return;
        printIndent(level);
        std::cout << name << ": 0x" << std::hex << std::setfill('0')
                  << std::setw(8) << value << std::dec << "\n";
    }

    std::vector<FADCHit> decodeFADC250Payload(
        uint64_t frameTimestampNs,  // IMPORTANT: Assumed to be in nanoseconds
        int rocId,
        int slotId,  // Slot number from payload bank tag
        const uint8_t* payloadData,
        size_t payloadBytes
    ) {
        /**
         * FADC250 Data Word Format (32 bits):
         * Bit 31:    0 (data word identifier, 1=header)
         * Bits 17-30: Time offset (14 bits, 0-16383, in 4ns bins)
         * Bits 13-16: Channel number (4 bits, 0-15)
         * Bits 0-12:  Integrated charge (13 bits, 0-8191)
         *
         * NO BLOCK HEADER: Payload contains only hit data words.
         * Slot number comes from the EVIO payload bank tag.
         *
         * TIMESTAMP UNITS WARNING:
         * This function assumes frameTimestampNs is in nanoseconds.
         * If the frame timestamp is in different units (e.g., 4ns ticks from TI),
         * the absolute hit times will be incorrect.
         * TODO: Verify timestamp units from CODA payload specification.
         */
        std::vector<FADCHit> hits;

        // Validate payload size is multiple of 4
        if (payloadBytes % 4 != 0) {
            result.addWarning("FADC250 payload size (" + std::to_string(payloadBytes) +
                            " bytes) not multiple of 4");
            payloadBytes = (payloadBytes / 4) * 4;  // Truncate
        }

        size_t numWords = payloadBytes / 4;

        if (fadcVerbose) {
            std::cout << "# DEBUG: decodeFADC250Payload crate=" << rocId << " slot=" << slotId
                     << " bytes=" << payloadBytes << " words=" << numWords << "\n";
        }

        if (numWords == 0) {
            if (fadcVerbose) {
                std::cout << "# DEBUG: No words to decode (empty payload)\n";
            }
            return hits;
        }

        // Decode all words as hit data (no block header)
        for (size_t i = 0; i < numWords; i++) {
            // Read 32-bit word in BIG_ENDIAN
            uint32_t word = 0;
            std::memcpy(&word, payloadData + (i * 4), 4);
            word = ntoh32(word);

            // Skip words that look like headers (bit 31 = 1)
            // Though there shouldn't be any in this format
            if (word & 0x80000000) {
                if (verbose) {
                    printIndent(5);
                    std::cout << "[FADC250] Skipping header word: 0x" << std::hex << word << std::dec << "\n";
                }
                continue;
            }

            // Extract hit data fields (bit field extraction verified correct)
            int charge = word & 0x1FFF;                        // Bits 0-12 (13 bits: 0-8191)
            int channel = (word >> 13) & 0x000F;               // Bits 13-16 (4 bits: 0-15)
            uint64_t timeOffset = ((word >> 17) & 0x3FFF) * 4; // Bits 17-30 (14 bits: 0-16383), * 4ns

            uint64_t hitTime = frameTimestampNs + timeOffset;

            // Validation: Channel must be 0-15 (FADC250 has 16 channels)
            if (channel > 15) {
                result.addWarning("Invalid FADC250 channel " + std::to_string(channel) +
                                " (must be 0-15) in ROC " + std::to_string(rocId) +
                                " slot " + std::to_string(slotId));
                continue;
            }

            // Validation: Charge should fit in 13 bits (0-8191)
            // This check is redundant due to mask, but kept for clarity
            if (charge > 8191) {
                result.addWarning("FADC250 charge overflow: " + std::to_string(charge) +
                                " in ROC " + std::to_string(rocId) + " slot " + std::to_string(slotId));
            }

            hits.emplace_back(rocId, slotId, channel, charge, hitTime);
        }

        // Sort by time
        std::sort(hits.begin(), hits.end(),
                 [](const FADCHit& a, const FADCHit& b) { return a.time < b.time; });

        return hits;
    }

public:
    EVIO6Parser(bool verbose_mode = false, bool fadc_verbose_mode = false)
        : verbose(verbose_mode), fadcVerbose(fadc_verbose_mode) {}

    bool loadFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file) {
            std::cerr << "ERROR: Cannot open file: " << filename << std::endl;
            return false;
        }

        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        fileData.resize(fileSize);
        file.read(reinterpret_cast<char*>(fileData.data()), fileSize);

        if (!file) {
            std::cerr << "ERROR: Failed to read file: " << filename << std::endl;
            return false;
        }

        std::cout << "Loaded file: " << filename << " (" << fileSize << " bytes)\n";
        return true;
    }

    void parseFileHeader() {
        printHeader("EVIO6 File Header", 0);

        size_t startPos = currentPos;

        // Word 1: File ID
        uint32_t fileId = read32();
        printHex("File ID", fileId, 1);

        if (fileId != EVIO6::FILE_ID_EVIO) {
            result.addError("Invalid file ID: expected 0x4556494F (EVIO), got 0x" +
                          std::to_string(fileId));
            return;
        }

        // Word 2: File Number
        uint32_t fileNumber = read32();
        printField("File Number", fileNumber, "", 1);

        // Word 3: Header Length
        uint32_t headerLength = read32();
        printField("Header Length", headerLength, "words", 1);

        if (headerLength != EVIO6::HEADER_LENGTH) {
            result.addError("Invalid header length: expected 14, got " +
                          std::to_string(headerLength));
        }

        // Word 4: Record Count
        uint32_t recordCount = read32();
        printField("Record Count", recordCount, "", 1);

        // Word 5: Index Array Length
        uint32_t indexArrayLength = read32();
        printField("Index Array Length", indexArrayLength, "bytes", 1);

        // Word 6: Bit Info + Version
        uint32_t bitInfoVersion = read32();
        uint8_t version = bitInfoVersion & 0xFF;
        uint32_t bitInfo = (bitInfoVersion >> 8) & 0xFFFFFF;

        printField("Version", version, "", 1);
        printHex("Bit Info", bitInfo, 1);

        if (version != EVIO6::VERSION) {
            result.addError("Invalid EVIO version: expected 6, got " +
                          std::to_string(version));
        }

        // Word 7: User Header Length
        uint32_t userHeaderLength = read32();
        printField("User Header Length", userHeaderLength, "bytes", 1);

        // Word 8: Magic Number
        uint32_t magic = read32();
        printHex("Magic Number", magic, 1);

        if (magic != EVIO6::MAGIC_NUMBER) {
            result.addError("Invalid magic number: expected 0xC0DA0100, got 0x" +
                          std::to_string(magic));
        }

        // Words 9-10: User Register
        uint64_t userReg = read64();
        printHex("User Register", static_cast<uint32_t>(userReg), 1);

        // Words 11-12: Trailer Position
        uint64_t trailerPos = read64();
        printField("Trailer Position", trailerPos, "bytes", 1);

        // Words 13-14: User Integers
        uint32_t userInt1 = read32();
        uint32_t userInt2 = read32();
        printField("User Integer 1", userInt1, "", 1);
        printField("User Integer 2", userInt2, "", 1);

        size_t bytesRead = currentPos - startPos;
        if (bytesRead != 56) {
            result.addError("File header size mismatch: read " +
                          std::to_string(bytesRead) + " bytes, expected 56");
        }
    }

    void parseRecordHeader() {
        printHeader("EVIO6 Record Header #" + std::to_string(recordCount), 0);
        recordCount++;

        size_t startPos = currentPos;

        // Word 1: Record Length
        uint32_t recordLength = read32();
        printField("Record Length", recordLength, "words (inclusive)", 1);

        if (recordLength == 0) {
            result.addError("Invalid record length: 0");
            return;
        }

        // Word 2: Record Number
        uint32_t recordNumber = read32();
        printField("Record Number", recordNumber, "", 1);

        // Word 3: Header Length
        uint32_t headerLength = read32();
        printField("Header Length", headerLength, "words", 1);

        if (headerLength != EVIO6::HEADER_LENGTH) {
            result.addError("Invalid record header length: expected 14, got " +
                          std::to_string(headerLength));
        }

        // Word 4: Event Index Count
        uint32_t eventCount = read32();
        printField("Event Index Count", eventCount, "", 1);

        // Word 5: Index Array Length
        uint32_t indexArrayLength = read32();
        printField("Index Array Length", indexArrayLength, "bytes", 1);

        // Word 6: Bit Info + Version
        uint32_t bitInfoVersion = read32();
        uint8_t version = bitInfoVersion & 0xFF;
        uint32_t bitInfo = (bitInfoVersion >> 8) & 0xFFFFFF;
        bool isLastRecord = (bitInfo & (1 << 9)) != 0;
        bool hasBigEndian = (bitInfoVersion & 0x80000000) != 0;

        printField("Version", version, "", 1);
        printHex("Bit Info", bitInfo, 1);
        printField("Is Last Record", isLastRecord, "", 1);
        printField("Big Endian", hasBigEndian, "", 1);

        if (version != EVIO6::VERSION) {
            result.addError("Invalid record EVIO version: expected 6, got " +
                          std::to_string(version));
        }

        // Word 7: User Header Length
        uint32_t userHeaderLength = read32();
        printField("User Header Length", userHeaderLength, "bytes", 1);

        // Word 8: Magic Number
        uint32_t magic = read32();
        printHex("Magic Number", magic, 1);

        if (magic != EVIO6::MAGIC_NUMBER) {
            result.addError("Invalid magic number in record: expected 0xC0DA0100, got 0x" +
                          std::to_string(magic));
        }

        // Word 9: Uncompressed Data Length
        uint32_t uncompressedLen = read32();
        printField("Uncompressed Data Length", uncompressedLen, "bytes", 1);

        // Word 10: Compression Type + Compressed Length
        uint32_t compressInfo = read32();
        uint8_t compressType = (compressInfo >> 28) & 0xF;
        uint32_t compressedLen = compressInfo & 0x0FFFFFFF;
        printField("Compression Type", compressType, "", 1);
        printField("Compressed Length", compressedLen, "words", 1);

        // Words 11-14: User Registers (2 x 64-bit)
        uint64_t userReg1 = read64();
        uint64_t userReg2 = read64();
        printHex("User Register 1", static_cast<uint32_t>(userReg1), 1);
        printHex("User Register 2", static_cast<uint32_t>(userReg2), 1);

        size_t bytesRead = currentPos - startPos;
        if (bytesRead != 56) {
            result.addError("Record header size mismatch: read " +
                          std::to_string(bytesRead) + " bytes, expected 56");
        }
    }

    void parseAggregatedFrameBank() {
        printHeader("Aggregated Frame Bank", 1);

        // Bank Length (exclusive)
        uint32_t bankLength = read32();
        printField("Bank Length", bankLength, "words (exclusive)", 2);

        // Bank Header: tag (16) | type (8) | streamStatus (8)
        uint32_t bankHeader = read32();
        uint16_t tag = (bankHeader >> 16) & 0xFFFF;
        uint8_t type = (bankHeader >> 8) & 0xFF;
        uint8_t streamStatus = bankHeader & 0xFF;

        printHex("Tag", tag, 2);
        printField("Type", type, "0x10 = BANK", 2);
        printField("Stream Status", streamStatus, "", 2);

        if (tag != EVIO6::TAG_AGG_FRAME) {
            result.addError("Invalid aggregated frame tag: expected 0xFF60, got 0x" +
                          std::to_string(tag));
        }

        if (type != EVIO6::TYPE_BANK) {
            result.addError("Invalid aggregated frame type: expected 0x10 (BANK), got 0x" +
                          std::to_string(type));
        }
    }

    void parseStreamInfoBank() {
        printHeader("Stream Info Bank", 2);

        // Bank Length
        uint32_t bankLength = read32();
        printField("Bank Length", bankLength, "words (exclusive)", 3);

        // Bank Header: tag (16) | type (8) | streamStatus (8)
        uint32_t bankHeader = read32();
        uint16_t tag = (bankHeader >> 16) & 0xFFFF;
        uint8_t type = (bankHeader >> 8) & 0xFF;
        uint8_t streamStatus = bankHeader & 0xFF;

        printHex("Tag", tag, 3);
        printField("Type", type, "0x20 = SEGMENT", 3);
        printField("Stream Status", streamStatus, "", 3);

        if (tag != EVIO6::TAG_STREAM_INFO) {
            result.addError("Invalid stream info tag: expected 0xFF31, got 0x" +
                          std::to_string(tag));
        }

        if (type != EVIO6::TYPE_SEGMENT) {
            result.addError("Invalid stream info type: expected 0x20 (SEGMENT), got 0x" +
                          std::to_string(type));
        }
    }

    void parseTimeSliceSegment() {
        printHeader("Time Slice Segment (TSS)", 3);

        // Segment Header: tag (8) | type (8) | length (16)
        uint32_t segHeader = read32();
        uint8_t tag = (segHeader >> 24) & 0xFF;
        uint8_t type = (segHeader >> 16) & 0xFF;
        uint16_t length = segHeader & 0xFFFF;

        printHex("Tag", tag, 4);
        printField("Type", type, "0x01 = INT", 4);
        printField("Length", length, "words", 4);

        if (tag != EVIO6::TAG_TIME_SLICE) {
            result.addError("Invalid time slice segment tag: expected 0x32, got 0x" +
                          std::to_string((int)tag));
        }

        if (type != EVIO6::TYPE_INT) {
            result.addError("Invalid time slice segment type: expected 0x01 (INT), got 0x" +
                          std::to_string((int)type));
        }

        if (length != 3) {
            result.addWarning("Time slice segment length is " + std::to_string(length) +
                            " words, expected 3");
        }

        // TSS Data: frameNumber, timestamp_low, timestamp_high
        uint32_t frameNumber = read32();
        uint32_t tsLow = read32();
        uint32_t tsHigh = read32();
        uint64_t timestamp = (static_cast<uint64_t>(tsHigh) << 32) | tsLow;

        printField("Frame Number", frameNumber, "", 4);
        printField("Timestamp", timestamp, "", 4);

        currentFrameTimestamp = timestamp;  // Store for FADC decoding
    }

    void parseAggregationInfoSegment() {
        printHeader("Aggregation Info Segment (AIS)", 3);

        // Segment Header: tag (8) | type (8) | length (16)
        uint32_t segHeader = read32();
        uint8_t tag = (segHeader >> 24) & 0xFF;
        uint8_t type = (segHeader >> 16) & 0xFF;
        uint16_t length = segHeader & 0xFFFF;

        printHex("Tag", tag, 4);
        printField("Type", type, "0x01 = INT", 4);
        printField("Length", length, "ROC count", 4);

        if (tag != EVIO6::TAG_AGG_INFO) {
            result.addError("Invalid aggregation info segment tag: expected 0x42, got 0x" +
                          std::to_string((int)tag));
        }

        if (type != EVIO6::TYPE_INT) {
            result.addError("Invalid aggregation info segment type: expected 0x01 (INT), got 0x" +
                          std::to_string((int)type));
        }

        // AIS Data: ROC IDs
        for (int i = 0; i < length; i++) {
            uint32_t rocEntry = read32();
            uint16_t rocId = (rocEntry >> 16) & 0xFFFF;
            uint8_t reserved = (rocEntry >> 8) & 0xFF;
            uint8_t status = rocEntry & 0xFF;

            if (verbose) {
                printIndent(4);
                std::cout << "ROC " << i << ": ID=0x" << std::hex << rocId
                         << ", Status=0x" << (int)status << std::dec << "\n";
            }
        }
    }

    void parseROCPayloadBank(int rocIndex) {
        printHeader("ROC Payload Bank #" + std::to_string(rocIndex), 2);

        // ROC Bank Length
        uint32_t bankLength = read32();
        printField("ROC Bank Length", bankLength, "words (exclusive)", 3);

        // ROC Bank Header
        uint32_t bankHeader = read32();
        uint16_t tag = (bankHeader >> 16) & 0xFFFF;
        uint8_t type = (bankHeader >> 8) & 0xFF;
        uint8_t streamStatus = bankHeader & 0xFF;

        printHex("Tag", tag, 3);
        printField("Type", type, "", 3);
        printField("Stream Status", streamStatus, "", 3);

        // Debug: Show ROC bank type when verbose enabled
        if (verbose) {
            printIndent(3);
            std::cout << "[ROC BANK] Tag=" << tag << " Type=0x" << std::hex << (int)type << std::dec;
            if (type == 0x10) {
                std::cout << " (BANK - should contain sub-banks)\n";
            } else if (type == 0x20) {
                std::cout << " (SEGMENT - direct data)\n";
            } else if (type == 0x01) {
                std::cout << " (INT - 32-bit integers)\n";
            } else {
                std::cout << " (unknown type)\n";
            }
        }

        // Get ROC ID from stored list (use index if not available)
        int rocId = (rocIndex < currentEventROCIds.size()) ? currentEventROCIds[rocIndex] : rocIndex;

        // Check if this is a BANK (0x10) containing sub-banks, or direct data
        if (fadcVerbose) {
            std::cout << "# DEBUG: ROC " << rocId << " type=0x" << std::hex << (int)type << std::dec
                     << " tag=" << tag << " length=" << bankLength << "\n";
        }

        if (type == 0x10) {
            // ROC bank contains sub-banks (one per FADC slot)
            // bankLength is exclusive, so actual data is (bankLength - 1) words
            size_t rocDataWords = bankLength - 1;
            size_t rocDataEndPos = currentPos + (rocDataWords * 4);

            if (fadcVerbose) {
                std::cout << "# DEBUG: ROC BANK type=0x10, rocDataWords=" << rocDataWords
                         << " rocDataEndPos=" << rocDataEndPos
                         << " currentPos=" << currentPos << "\n";
            }

            if (verbose) {
                printIndent(3);
                std::cout << "[ROC BANK] Parsing sub-banks (slots) within ROC " << rocId << "\n";
            }

            // Parse all sub-banks as potential payload banks
            // Don't skip any - even tag 0xFF30 can contain FADC data
            int subBankIndex = 0;

            if (fadcVerbose) {
                std::cout << "# DEBUG: Entering sub-bank loop, condition: currentPos(" << currentPos
                         << ") < rocDataEndPos(" << rocDataEndPos << ") = "
                         << (currentPos < rocDataEndPos ? "true" : "false") << "\n";
            }

            while (currentPos < rocDataEndPos && currentPos < fileData.size()) {
                if (fadcVerbose) {
                    std::cout << "# DEBUG: Sub-bank loop iteration " << subBankIndex
                             << " at position " << currentPos << "\n";
                }
                // Read payload bank header
                uint32_t payloadBankLength = read32();
                uint32_t payloadBankHeader = read32();

                uint16_t payloadTag = (payloadBankHeader >> 16) & 0xFFFF;
                uint8_t payloadType = (payloadBankHeader >> 8) & 0xFF;
                uint8_t payloadNum = payloadBankHeader & 0xFF;  // Bits 7-0

                // Payload data size (bankLength - 1 for the header we already read)
                size_t payloadDataWords = (payloadBankLength > 1) ? (payloadBankLength - 1) : 0;
                size_t payloadBytes = payloadDataWords * 4;

                if (fadcVerbose) {
                    std::cout << "# DEBUG: Payload bank header=0x" << std::hex << payloadBankHeader << std::dec
                             << " tag=0x" << std::hex << payloadTag << std::dec
                             << " type=0x" << std::hex << (int)payloadType << std::dec
                             << " num=" << (int)payloadNum
                             << " payloadBytes=" << payloadBytes << "\n";
                }

                // Skip Stream Info Bank (tag 0xFF30) - it doesn't contain detector data
                if (payloadTag == 0xFF30) {
                    if (fadcVerbose) {
                        std::cout << "# DEBUG: Skipping Stream Info Bank (tag=0xFF30)\n";
                    }
                    currentPos += payloadBytes;
                    subBankIndex++;
                    continue;
                }

                // For payload banks: slot number is in bits 4-0 of the Tag field
                // (Payload Port # from the PP ID structure, range 0-20)
                int slotId = payloadTag & 0x1F;  // Bits 4-0

                if (fadcVerbose) {
                    std::cout << "# DEBUG: Extracted slotId=" << slotId << " from tag bits 4-0\n";
                }

                if (verbose) {
                    printIndent(4);
                    std::cout << "[PAYLOAD BANK] Slot=" << slotId
                             << " (Tag=0x" << std::hex << payloadTag << std::dec
                             << ") Length=" << payloadBankLength << " words\n";
                }

                subBankIndex++;

                if (payloadBytes > 0) {
                    if (fadcVerbose) {
                        std::cout << "# DEBUG: Decoding payload: rocId=" << rocId
                                 << " slotId=" << slotId
                                 << " payloadBytes=" << payloadBytes << "\n";
                    }

                    const uint8_t* payloadData = &fileData[currentPos];

                    // Decode FADC250 hit data (no block header, just hit words)
                    std::vector<FADCHit> hits = decodeFADC250Payload(
                        currentFrameTimestamp,
                        rocId,
                        slotId,  // Use payload bank tag as slot number
                        payloadData,
                        payloadBytes
                    );

                    // Print hits if FADC verbose enabled (one line per hit)
                    if (fadcVerbose) {
                        if (hits.empty()) {
                            std::cout << "# DEBUG: No hits decoded from slot " << slotId
                                     << " (" << payloadBytes << " bytes)\n";
                        } else {
                            for (const auto& hit : hits) {
                                std::cout << "crate=" << hit.crate
                                         << ", slot=" << hit.slot
                                         << ", channel=" << hit.channel
                                         << ", charge=" << hit.charge
                                         << ", time=" << hit.time << "\n";
                            }
                        }
                    }

                    // Accumulate for event summary
                    currentEventHits.insert(currentEventHits.end(), hits.begin(), hits.end());

                    currentPos += payloadBytes;
                } else {
                    if (fadcVerbose) {
                        std::cout << "# DEBUG: Payload bytes is 0, skipping decode\n";
                    }
                }
            }

            if (fadcVerbose) {
                std::cout << "# DEBUG: Exited sub-bank loop, currentPos=" << currentPos
                         << " rocDataEndPos=" << rocDataEndPos << "\n";
            }
        } else {
            // ROC bank contains direct data (old format or single slot)
            size_t payloadWords = bankLength - 1;
            size_t payloadBytes = payloadWords * 4;

            if (currentPos + payloadBytes > fileData.size()) {
                result.addError("ROC payload extends beyond file boundary");
                return;
            }

            printField("Payload Size", payloadBytes, "bytes", 3);

            // Decode FADC250 payload (use ROC ID as slot fallback)
            const uint8_t* payloadData = &fileData[currentPos];
            std::vector<FADCHit> hits = decodeFADC250Payload(
                currentFrameTimestamp,
                rocId,
                tag,  // Use ROC bank tag as fallback slot
                payloadData,
                payloadBytes
            );

            // Print hits if FADC verbose enabled (one line per hit)
            if (fadcVerbose && !hits.empty()) {
                for (const auto& hit : hits) {
                    std::cout << hit.crate << " "
                             << hit.slot << " "
                             << hit.channel << " "
                             << hit.charge << " "
                             << hit.time << "\n";
                }
            }

            // Accumulate for event summary
            currentEventHits.insert(currentEventHits.end(), hits.begin(), hits.end());

            currentPos += payloadBytes;
        }
    }

    void parseEvent() {
        // Clear state from previous event
        currentEventHits.clear();
        currentEventROCIds.clear();

        if (fadcVerbose) {
            std::cout << "# DEBUG: parseEvent() called at position " << currentPos << "\n";
        }

        // Parse aggregated frame structure
        parseAggregatedFrameBank();
        parseStreamInfoBank();
        parseTimeSliceSegment();  // Now stores currentFrameTimestamp

        // Extract ROC IDs from aggregation info segment before parsing it
        size_t savedPos = currentPos;
        uint32_t aisHeader = read32();
        uint16_t rocCount = aisHeader & 0xFFFF;

        // Read and store ROC IDs
        for (int i = 0; i < rocCount; i++) {
            uint32_t rocEntry = read32();
            uint16_t rocId = (rocEntry >> 16) & 0xFFFF;
            currentEventROCIds.push_back(rocId);
        }

        // Restore position and parse segment normally
        currentPos = savedPos;
        parseAggregationInfoSegment();

        // Parse ROC payload banks
        if (fadcVerbose) {
            std::cout << "# DEBUG: Parsing " << rocCount << " ROC banks\n";
        }

        for (int i = 0; i < rocCount && currentPos < fileData.size(); i++) {
            parseROCPayloadBank(i);
        }

        // Event hits already printed during parsing (one line per hit)
        if (fadcVerbose) {
            std::cout << "# DEBUG: Event complete, total hits in currentEventHits: "
                     << currentEventHits.size() << "\n";
        }
    }

    void parse() {
        currentPos = 0;
        recordCount = 0;

        std::cout << "\n=== Starting EVIO6 File Parsing ===\n\n";

        // Parse file header
        parseFileHeader();

        if (!result.success) {
            std::cout << "\nFile header validation failed. Stopping.\n";
            return;
        }

        // Parse records until end of file
        int maxRecords = 100;  // Safety limit for testing
        while (currentPos < fileData.size() && recordCount < maxRecords) {
            size_t recordStart = currentPos;

            // Parse record header
            parseRecordHeader();

            if (!result.success && result.errors.size() > 10) {
                std::cout << "\nToo many errors. Stopping.\n";
                break;
            }

            // Parse event data
            if (currentPos < fileData.size()) {
                parseEvent();
            }

            // Check if we've processed entire record
            size_t recordSize = currentPos - recordStart;
            if (verbose) {
                std::cout << "\nRecord size: " << recordSize << " bytes\n";
            }
        }

        std::cout << "\n=== Parsing Complete ===\n";
        std::cout << "Total records processed: " << recordCount << "\n";
        std::cout << "File position: " << currentPos << " / " << fileData.size()
                  << " bytes\n";

        if (currentPos < fileData.size()) {
            std::cout << "Remaining data: " << (fileData.size() - currentPos)
                      << " bytes\n";
        }
    }

    const ValidationResult& getResult() const {
        return result;
    }
};

void printHelp(const char* progName) {
    std::cout << "EVIO6 Event Parser and Validator\n";
    std::cout << "=================================\n\n";
    std::cout << "Usage: " << progName << " <evio_file> [OPTIONS]\n\n";
    std::cout << "This program parses and validates EVIO6-format frame-built event files\n";
    std::cout << "produced by coda-fb. It validates the EVIO6 structure, checks headers,\n";
    std::cout << "and can decode FADC250 detector data.\n\n";
    std::cout << "Arguments:\n";
    std::cout << "  <evio_file>     Path to EVIO6 file to parse (required)\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help      Show this help message and exit\n";
    std::cout << "  --verbose       Show detailed EVIO6 structure including:\n";
    std::cout << "                  - File and record headers with all fields\n";
    std::cout << "                  - Bank, segment, and tag information\n";
    std::cout << "                  - Frame numbers, timestamps, and ROC IDs\n";
    std::cout << "                  - Payload sizes and structure details\n";
    std::cout << "  --fadc-verbose  Decode and display FADC250 detector hits:\n";
    std::cout << "                  - Crate: ROC ID from data source\n";
    std::cout << "                  - Slot: Module slot number (0-20)\n";
    std::cout << "                  - Channel: ADC channel (0-15)\n";
    std::cout << "                  - Charge: Integrated pulse charge (13-bit ADC)\n";
    std::cout << "                  - Time: Absolute hit time in nanoseconds\n\n";
    std::cout << "Exit Codes:\n";
    std::cout << "  0  File is valid EVIO6 format\n";
    std::cout << "  1  Validation errors or file cannot be opened\n\n";
    std::cout << "Examples:\n";
    std::cout << "  # Validate file structure only (quiet mode)\n";
    std::cout << "  " << progName << " frames_thread0_file0000.evio\n\n";
    std::cout << "  # Show detailed EVIO6 structure\n";
    std::cout << "  " << progName << " frames_thread0_file0000.evio --verbose\n\n";
    std::cout << "  # Decode and display FADC250 hits\n";
    std::cout << "  " << progName << " frames_thread0_file0000.evio --fadc-verbose\n\n";
    std::cout << "  # Show both structure and FADC250 data\n";
    std::cout << "  " << progName << " frames_thread0_file0000.evio --verbose --fadc-verbose\n\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp(argv[0]);
        return 1;
    }

    std::string filename;
    bool verbose = false;
    bool fadcVerbose = false;

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printHelp(argv[0]);
            return 0;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--fadc-verbose") {
            fadcVerbose = true;
        } else if (arg[0] != '-') {
            // First non-option argument is the filename
            if (filename.empty()) {
                filename = arg;
            } else {
                std::cerr << "ERROR: Multiple input files specified\n";
                std::cerr << "Use '" << argv[0] << " --help' for usage information\n";
                return 1;
            }
        } else {
            std::cerr << "ERROR: Unknown option: " << arg << "\n";
            std::cerr << "Use '" << argv[0] << " --help' for usage information\n";
            return 1;
        }
    }

    if (filename.empty()) {
        std::cerr << "ERROR: No input file specified\n";
        std::cerr << "Use '" << argv[0] << " --help' for usage information\n";
        return 1;
    }

    std::cout << "EVIO6 Event Parser and Validator\n";
    std::cout << "=================================\n";
    std::cout << "File: " << filename << "\n";
    std::cout << "Verbose: " << (verbose ? "enabled" : "disabled") << "\n";
    std::cout << "FADC Verbose: " << (fadcVerbose ? "enabled" : "disabled") << "\n\n";

    EVIO6Parser parser(verbose, fadcVerbose);

    if (!parser.loadFile(filename)) {
        return 1;
    }

    parser.parse();

    const auto& result = parser.getResult();
    result.print();

    return result.success ? 0 : 1;
}
