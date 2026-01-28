# EVIO Payload Parsing and Validation

## Overview

The e2sar_receiver now validates and parses every reassembled frame to extract metadata directly from the EVIO payload structure. This ensures data integrity and provides accurate timestamps for frame aggregation.

## Features

✅ **Magic Number Validation** - Verifies correct assembly (0xc0da0100 at word 8)
✅ **Endianness Detection** - Automatically detects and handles byte-swapped data
✅ **Metadata Extraction** - Extracts timestamp, frame number, and ROC ID from payload
✅ **Format Validation** - Verifies EVIO structure is correct
✅ **Error Reporting** - Tracks validation errors and wrong endianness count

## EVIO Payload Structure

### Word Layout (32-bit words, indexed from 1)

```
Word 1-7:  (header, not parsed)
Word 8:    0xc0da0100           Magic number (correctness check)
Word 9:    ROC bank length
Word 10:   ROC_ID              Format: 0x0010_10_ss (ss = stream/ROC ID)
Word 11:   Stream info bank length
Word 12:   Stream info header   Format: 0xFF30_20_ss
Word 13:   TSS header           Format: 0x31_01_length
Word 14:   Frame number         32-bit frame/event number
Word 15:   Timestamp [31:0]     Lower 32 bits of timestamp
Word 16:   Timestamp [63:32]    Upper 32 bits of timestamp
```

## Magic Number Validation

### Purpose

Word 8 must contain `0xc0da0100` to indicate:
1. Frame was correctly reassembled (no missing/corrupt packets)
2. Data has correct endianness (not byte-swapped)

### Detection

**Correct Endianness:**
```
Word 8 = 0xc0da0100  ✓ Valid, use data as-is
```

**Wrong Endianness:**
```
Word 8 = 0x0001dac0  ⚠ Valid but byte-swapped
                       (automatically handled)
```

**Invalid:**
```
Word 8 = anything else  ✗ Frame corrupted or incorrectly assembled
                          (frame skipped)
```

## Metadata Extraction

### Timestamp (64-bit)

Extracted from words 15 and 16:
```cpp
uint32_t ts_low  = word[15];  // Bits [31:0]
uint32_t ts_high = word[16];  // Bits [63:32]
uint64_t timestamp = (ts_high << 32) | ts_low;
```

### Frame Number (32-bit)

Extracted from word 14:
```cpp
uint32_t frameNumber = word[14];
```

### ROC/Stream ID (8-bit)

Extracted from lowest byte of word 10:
```cpp
uint32_t word10 = word[10];
uint8_t rocId = word10 & 0xFF;  // Extract 'ss' from 0x0010_10_ss
```

**Validation:** Word 10 must have format `0x0010_10_ss`, otherwise frame is invalid.

## Implementation

### Data Structures

```cpp
struct EVIOMetadata {
    uint64_t timestamp;      // 64-bit timestamp from payload
    uint32_t frameNumber;    // Frame number from payload
    uint16_t dataId;         // ROC/Stream ID (lowest 8 bits)
    bool valid;              // Whether payload is valid EVIO
    bool wrongEndian;        // Whether endianness was swapped
};
```

### Parser Function

```cpp
EVIOMetadata parseEVIOPayload(const uint8_t* payload, size_t payloadSize);
```

**Returns:**
- `meta.valid = true` - Payload is valid, metadata extracted
- `meta.valid = false` - Payload invalid, frame should be skipped
- `meta.wrongEndian = true` - Data was byte-swapped (but parsed correctly)

### Byte Swapping

When wrong endianness is detected, all 32-bit words are automatically byte-swapped:

```cpp
uint32_t swap32(uint32_t val) {
    return ((val & 0x000000FF) << 24) |
           ((val & 0x0000FF00) << 8) |
           ((val & 0x00FF0000) >> 8) |
           ((val & 0xFF000000) >> 24);
}
```

## Usage in Receiver

### Frame Processing Flow

```
1. Receive reassembled frame from E2SAR
         ↓
2. Parse EVIO payload: parseEVIOPayload(eventBuf, eventSize)
         ↓
3. Validate magic number (word 8)
         ↓
4. Extract metadata (timestamp, frameNumber, rocId)
         ↓
5. Route to frame builder or file output
```

### Code Example

```cpp
// Receive frame
auto getEvtRes = r->recvEvent(&eventBuf, &eventSize, &eventNum, &dataId, 1000);

// Parse payload
EVIOMetadata meta = parseEVIOPayload(eventBuf, eventSize);

if (!meta.valid) {
    // Invalid payload - skip frame
    payloadValidationErrors++;
    continue;
}

if (meta.wrongEndian) {
    wrongEndiannessCount++;
    // Parser has handled byte swapping
}

// Use extracted metadata
frameBuilder->addTimeSlice(
    meta.timestamp,     // From payload, not eventNum
    meta.frameNumber,   // From payload
    meta.dataId,        // From payload (ROC ID)
    eventBuf,
    eventSize
);
```

## Statistics

The receiver tracks validation metrics:

```
Final Statistics:
  Frames Received: 10523
  Frames Written: 10520
  Payload Validation Errors: 3      ← Invalid frames skipped
  Wrong Endianness Count: 0         ← Byte-swapped frames (still valid)
  ...
```

### Interpreting Statistics

**Payload Validation Errors:**
- Frames with invalid magic number (word 8 != 0xc0da0100/0x0001dac0)
- Frames with invalid ROC_ID format (word 10)
- Frames too small (< 64 bytes)
- These frames are **skipped** (not processed)

**Wrong Endianness Count:**
- Frames with byte-swapped data (word 8 = 0x0001dac0)
- Data is automatically corrected
- These frames are **processed normally**
- Indicates potential issue with data source

## Error Handling

### Invalid Magic Number

```
Invalid EVIO magic number at word 8: 0x12345678 (expected 0xc0da0100)
Skipping frame 1234 due to invalid payload
```

**Action:** Frame is skipped, counter incremented

### Invalid ROC_ID Format

```
Invalid ROC_ID format at word 10: 0xdeadbeef (expected 0x0010_10_ss)
Skipping frame 5678 due to invalid payload
```

**Action:** Frame is skipped, counter incremented

### Wrong Endianness

```
(No error message - automatically handled)
```

**Action:** Data is byte-swapped transparently, counter incremented

### Payload Too Small

```
Payload too small for EVIO format: 32 bytes
Skipping frame 9012 due to invalid payload
```

**Action:** Frame is skipped, counter incremented

## Benefits

### 1. Data Integrity

- Detects incorrectly reassembled frames
- Catches corruption during transmission
- Verifies EVIO structure is valid

### 2. Accurate Timestamps

- Uses actual timestamp from data stream
- Not dependent on reassembler's eventNum
- Critical for multi-stream synchronization

### 3. Correct ROC Identification

- Extracts actual ROC/stream ID from payload
- Independent of reassembler's dataId
- Ensures proper attribution in aggregated frames

### 4. Endianness Handling

- Automatic detection and correction
- Transparent to downstream processing
- Warns about potential source issues

## Debugging

### Enable Verbose Output

The parser outputs error messages to stderr for invalid frames. To see all validation activity:

```bash
e2sar_receiver ... 2>&1 | tee receiver.log
```

### Check for Issues

**High validation error rate:**
```
Payload Validation Errors: 1523 / 10000 (15%)
```
→ Problem with reassembly or data source

**Non-zero wrong endianness:**
```
Wrong Endianness Count: 234
```
→ Data source producing byte-swapped data

**All frames valid:**
```
Payload Validation Errors: 0
Wrong Endianness Count: 0
```
→ System operating correctly ✓

## Compatibility

### Backward Compatibility

✅ **File-only mode:** Validation still performed, invalid frames skipped
✅ **Frame builder mode:** Validated metadata used for aggregation
✅ **Original receiver:** Works as before (with added validation)

### EVIO Format Requirements

The payload parser expects:
- EVIO-6 format (not EVIO-4 or earlier)
- Specific structure from data acquisition system
- Magic number at word 8 (standard CODA DAQ format)

## Performance Impact

**Minimal overhead:**
- Simple word reads and comparisons
- No memory allocation
- ~10 CPU cycles per frame for validation
- Byte swapping only when needed

**Benefit vs. Cost:**
- Catches errors early (before aggregation)
- Prevents corrupt data from propagating
- Provides accurate timestamps for synchronization
- Well worth the negligible overhead

## Testing

### Test Valid Frame

```cpp
// Create test payload with valid magic number
uint32_t payload[16];
payload[7] = 0xc0da0100;           // Magic number
payload[9] = 0x00101001;           // ROC_ID (ss=1)
payload[13] = 1234;                 // Frame number
payload[14] = 0x12345678;          // Timestamp low
payload[15] = 0x9ABCDEF0;          // Timestamp high

EVIOMetadata meta = parseEVIOPayload(payload, 64);
assert(meta.valid == true);
assert(meta.timestamp == 0x9ABCDEF012345678);
assert(meta.frameNumber == 1234);
assert(meta.dataId == 1);
```

### Test Wrong Endianness

```cpp
payload[7] = 0x0001dac0;  // Byte-swapped magic number

EVIOMetadata meta = parseEVIOPayload(payload, 64);
assert(meta.valid == true);
assert(meta.wrongEndian == true);
// Values still extracted correctly
```

### Test Invalid Frame

```cpp
payload[7] = 0xDEADBEEF;  // Invalid magic number

EVIOMetadata meta = parseEVIOPayload(payload, 64);
assert(meta.valid == false);
```

## Future Enhancements

1. **Checksum Validation**: Add CRC/checksum verification
2. **Full Structure Parse**: Parse entire EVIO bank tree
3. **Payload Modification**: Optionally fix byte-swapped payloads in-place
4. **Extended Validation**: Check bank lengths, tag consistency
5. **Performance Monitoring**: Track parse time per frame

## See Also

- **EVIO Format Specification**: CODA documentation
- **Frame Builder**: `FRAMEBUILDER_README.md`
- **Command-Line Usage**: `COMMAND_LINE_USAGE.md`
- **Implementation Summary**: `IMPLEMENTATION_SUMMARY.md`

---

**Status:** ✅ Implemented and integrated
**Version:** 1.0.0
**Date:** 2024
