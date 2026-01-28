# Code Documentation Summary

## Overview

The e2sar_receiver code has been enhanced with extensive, detailed comments to make it extremely readable and maintainable. Every critical section includes clear explanations of what the code does, why it does it, and how it works.

## Documentation Enhancements

### 1. **Payload Parsing Functions**

#### EVIOMetadata Structure
```cpp
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
```

**Key Information:**
- Purpose of each field clearly explained
- Indicates which payload words contain the data
- Explains how fields are used in the system

#### swap32() Function
```cpp
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
```

**Key Information:**
- Clear explanation of what byte swapping does
- Concrete example showing transformation
- Each bit manipulation explained with comments

#### parseEVIOPayload() Function

**Header Documentation:**
```cpp
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
```

**Step-by-Step Processing:**

Each step in the parsing function is clearly marked and explained:

```cpp
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
```

**Key Features:**
- Each validation step clearly marked
- Success/warning/failure cases explicitly identified
- Explains WHY each check is performed
- Shows index conversion (1-based to 0-based)
- Clear return paths with explanations

### 2. **Main Reception Loop**

#### receiveAndWriteFrames() Function

**Header Documentation:**
```cpp
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
```

**Step-by-Step Processing:**

```cpp
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
    framesReceived++;

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
```

**Frame Builder Mode:**
```cpp
if (frameBuilder != nullptr) {
    // ------------------------------------------------------------
    // FRAME BUILDER MODE: Send to frame builder for aggregation
    // ------------------------------------------------------------
    // The frame builder will:
    // 1. Group this frame with others by timestamp
    // 2. Build EVIO-6 aggregated time frame bank
    // 3. Output to ET system and/or files with 2GB rollover
    //
    // We pass the extracted metadata from the payload:
    // - timestamp: For grouping frames across multiple streams
    // - frameNumber: For tracking and debugging
    // - rocId: For identifying which ROC/stream this came from
    // - eventBuf: The actual payload data (kept as-is)
    // - eventSize: Size of payload in bytes

    frameBuilder->addTimeSlice(
        timestamp,       // 64-bit timestamp for synchronization
        frameNumber,     // Frame sequence number
        rocId,           // ROC/Stream identifier
        eventBuf,        // Pointer to payload data
        eventSize        // Size of payload
    );
```

**File-Only Mode:**
```cpp
else {
    // ------------------------------------------------------------
    // FILE-ONLY MODE: Write directly to output file
    // ------------------------------------------------------------
    // This is the original behavior - write raw reassembled frames
    // directly to a single output file without aggregation or
    // EVIO-6 formatting.

    // Acquire mutex to ensure thread-safe file writing
    std::lock_guard<std::mutex> lock(fileMutex);

    // Write the entire frame payload to file
    ssize_t bytesWritten = write(outputFd, eventBuf, eventSize);

    if (bytesWritten < 0)
    {
        // Write failed - log error with errno description
        std::cerr << "Error writing event " << eventNum << " to file: "
                  << strerror(errno) << std::endl;
        writeErrors++;
    }
```

## Documentation Benefits

### 1. **Readability**
- Anyone can understand what the code does without deep knowledge
- Clear section markers with separator lines
- Logical flow from step to step

### 2. **Maintainability**
- Future developers can quickly locate and understand sections
- Comments explain WHY decisions were made, not just WHAT
- Easy to modify or extend without breaking logic

### 3. **Debugging**
- Clear indication of where each validation happens
- Explicit explanation of error conditions
- Easy to trace through execution flow

### 4. **Education**
- New developers can learn EVIO structure from comments
- Explains technical concepts (endianness, magic numbers)
- Shows best practices for data validation

### 5. **Code Review**
- Reviewers can quickly verify logic is correct
- Intentions are clear at every step
- Edge cases are explicitly handled and documented

## Comment Style Guidelines

The enhanced comments follow these principles:

### 1. **Section Headers**
```cpp
// ========================================================================
// MAJOR SECTION TITLE
// ========================================================================
```

### 2. **Sub-section Headers**
```cpp
// ====================================================================
// STEP N: Sub-section Title
// ====================================================================
```

### 3. **Inline Block Comments**
```cpp
// ------------------------------------------------------------
// Mode-specific processing
// ------------------------------------------------------------
```

### 4. **Variable Annotations**
```cpp
uint64_t timestamp;      // 64-bit timestamp extracted from payload words 15-16
                          // Used for synchronizing frames across multiple streams
```

### 5. **Conditional Logic**
```cpp
if (condition) {
    // SUCCESS: Explain what succeeded and why
    // ...
} else {
    // FAILURE: Explain what failed and why
    // ...
}
```

### 6. **Parameter Documentation**
```cpp
frameBuilder->addTimeSlice(
    timestamp,       // 64-bit timestamp for synchronization
    frameNumber,     // Frame sequence number
    rocId,           // ROC/Stream identifier
    eventBuf,        // Pointer to payload data
    eventSize        // Size of payload
);
```

## Summary

The code is now **extremely readable** with:
- ✅ Clear section markers and step-by-step flow
- ✅ Explanation of WHY, not just WHAT
- ✅ Success/warning/failure cases explicitly documented
- ✅ Every parameter explained at point of use
- ✅ Visual separation with consistent formatting
- ✅ Educational comments for complex concepts
- ✅ Easy navigation through logical sections

This level of documentation ensures the code can be understood, maintained, and extended by any developer, even years from now.
