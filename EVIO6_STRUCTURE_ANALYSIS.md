# EVIO6 Structure Analysis: PDF vs coda-fb Implementation

## Executive Summary

This document provides a detailed comparison between:
1. The CODA Online Data Formats specification (`coda_data_format.pdf`)
2. The actual coda-fb implementation (`e2sar_reassembler_framebuilder.cpp`)
3. The observed structure in `frames_thread0_file0000.evio`

The parser (`evio_event_parser.cpp`) validates against the **actual implementation** to ensure correctness.

## File-Level Structure

### EVIO6 File Header (14 words = 56 bytes)

| Word | Field | PDF Spec | coda-fb Implementation | Validation |
|------|-------|----------|------------------------|------------|
| 1 | File ID | 0x4556494F ("EVIO") | 0x4556494F | ✓ Match |
| 2 | File Number | Split number (1+) | 0 (unused) | ✓ Implementation |
| 3 | Header Length | 14 words | 14 words | ✓ Match |
| 4 | Record Count | Number of records | 0 (unknown) | ⚠ Implementation uses 0 |
| 5 | Index Array Length | Bytes | 0 (no index) | ✓ Implementation |
| 6 | Bit Info + Version | Version in low 8 bits | Version = 6 | ✓ Match |
| 7 | User Header Length | Bytes | 0 (no user header) | ✓ Implementation |
| 8 | Magic Number | 0xC0DA0100 | 0xC0DA0100 | ✓ Match |
| 9-10 | User Register | 64-bit user data | 0x0000000000000000 | ✓ Implementation |
| 11-12 | Trailer Position | Bytes from start | 0x0000000000000000 (no trailer) | ✓ Implementation |
| 13-14 | User Integers | Two 32-bit values | 0x00000000 each | ✓ Implementation |

**Implementation Notes:**
- coda-fb writes file header with minimal metadata
- No index array, no trailer, no user header
- Record count is 0 (unknown at file creation time)

### EVIO6 Record Header (14 words = 56 bytes)

| Word | Field | PDF Spec | coda-fb Implementation | Validation |
|------|-------|----------|------------------------|------------|
| 1 | Record Length | 32-bit words (inclusive) | Calculated length | ✓ Match |
| 2 | Record Number | Starting at 1 | 0, 1, 2, ... | ✓ Implementation |
| 3 | Header Length | 14 words | 14 words | ✓ Match |
| 4 | Event Index Count | Number of events | 1 (one event per record) | ✓ Implementation |
| 5 | Index Array Length | Bytes | 0 (no index) | ✓ Implementation |
| 6 | Bit Info + Version | Complex bit field | See below | ✓ Implementation |
| 7 | User Header Length | Bytes | 0 (no user header) | ✓ Implementation |
| 8 | Magic Number | 0xC0DA0100 | 0xC0DA0100 | ✓ Match |
| 9 | Uncompressed Length | Words after header | Calculated | ✓ Implementation |
| 10 | Compression Info | Type (4 bits) + Length (28 bits) | 0 (no compression) | ✓ Implementation |
| 11-14 | User Registers | 2 x 64-bit values | 0x0000000000000000 | ✓ Implementation |

**Word 6 Bit Info Details (from coda-fb, line 415-418):**
```
Bits 0-7:   Version = 6
Bit 9:      Last block flag (1 = is last)
Bit 14:     Header type = EVIO record
Bit 31:     Byte order (1 = BIG endian)
```

Value: `0x80004206` = `10000000 00000000 01000010 00000110`
- Version: 0x06 (6)
- Bit 9 set: Last block
- Bit 14 set: Header type 0 (EVIO record)
- Bit 31 set: BIG endian

## Event Structure: Frame-Built Format

### Top-Level: Aggregated Frame Bank

| Component | PDF Spec | coda-fb Implementation | Parser Validation |
|-----------|----------|------------------------|-------------------|
| **Tag** | 0xFF60 (Streaming ROC Raw) | 0xFF60 | ✓ Tag checked |
| **Type** | 0x10 (Bank) | 0x10 (Bank) | ✓ Type checked |
| **Num Field** | Stream Status | Stream Status (error bit + count) | ✓ Validated |
| **Structure** | Bank of banks | Bank of banks | ✓ Nesting validated |

**Code Reference:** `e2sar_reassembler_framebuilder.cpp` line 443-444
```cpp
uint32_t aggBankHeader = (0xFF60 << 16) | (0x10 << 8) | streamStatus;
```

**Stream Status Encoding (line 365):**
```cpp
int streamStatus = ((hasError ? 1 : 0) << 7) | (sliceCount & 0x7F);
```
- Bit 7: Error flag (1 = error detected)
- Bits 0-6: Slice count (number of ROC sources)

### Level 2: Stream Info Bank

| Component | PDF Spec | coda-fb Implementation | Parser Validation |
|-----------|----------|------------------------|-------------------|
| **Tag** | 0xFF31 (Time Info Bank) | 0xFF31 (Stream Info Bank) | ✓ Tag checked |
| **Type** | 0x20 (Segment) | 0x20 (Segment) | ✓ Type checked |
| **Num Field** | Stream Status | Stream Status | ✓ Validated |
| **Structure** | Bank of segments | Bank of segments | ✓ Nesting validated |

**Code Reference:** Line 453-454
```cpp
uint32_t streamInfoHeader = (0xFF31 << 16) | (0x20 << 8) | streamStatus;
```

### Level 3a: Time Slice Segment (TSS)

| Component | PDF Spec | coda-fb Implementation | Parser Validation |
|-----------|----------|------------------------|-------------------|
| **Tag** | 0x31 | **0x32** | ⚠ **DISCREPANCY** |
| **Type** | 0x01 (INT) | 0x01 (INT) | ✓ Type checked |
| **Length** | 3 words | 3 words | ✓ Length checked |
| **Data Word 1** | Frame Number | Frame Number (32-bit) | ✓ Validated |
| **Data Word 2** | Timestamp Low | Timestamp Low (32-bit) | ✓ Validated |
| **Data Word 3** | Timestamp High | Timestamp High (32-bit) | ✓ Validated |

**Code Reference:** Line 459
```cpp
uint32_t tssHeader = (0x32 << 24) | (0x01 << 16) | 3;  // Tag 0x32, not 0x31!
```

**🔴 KEY DISCREPANCY:**
- **PDF Specification:** Tag 0x31 for Time Slice Segment
- **coda-fb Implementation:** Tag **0x32** for Time Slice Segment
- **Parser Validates:** Tag **0x32** (matches implementation)

**Timestamp Encoding (line 464-465):**
```cpp
eventWords.push_back(static_cast<uint32_t>(tsAvg & 0xFFFFFFFF));        // low 32 bits
eventWords.push_back(static_cast<uint32_t>((tsAvg >> 32) & 0xFFFFFFFF)); // high 32 bits
```
- 64-bit timestamp split into two 32-bit words
- Low word first, high word second (regardless of endianness)

### Level 3b: Aggregation Info Segment (AIS)

| Component | PDF Spec | coda-fb Implementation | Parser Validation |
|-----------|----------|------------------------|-------------------|
| **Tag** | 0x41 | **0x42** | ⚠ **DISCREPANCY** |
| **Type** | 0x01 (INT) | 0x01 (INT) | ✓ Type checked |
| **Length** | N (ROC count) | N (ROC count) | ✓ Length validated |
| **Data Format** | ROC_ID \| reserved \| status | Same | ✓ Format validated |

**Code Reference:** Line 469
```cpp
uint32_t aisHeader = (0x42 << 24) | (0x01 << 16) | sliceCount;  // Tag 0x42, not 0x41!
```

**🔴 KEY DISCREPANCY:**
- **PDF Specification:** Tag 0x41 for Aggregation Info Segment
- **coda-fb Implementation:** Tag **0x42** for Aggregation Info Segment
- **Parser Validates:** Tag **0x42** (matches implementation)

**ROC Entry Format (line 475):**
```cpp
uint32_t aisEntry = (slice.dataId << 16) | slice.streamStatus;
```
- Bits 31-16: ROC ID / Data Source ID
- Bits 15-8: Reserved (unused)
- Bits 7-0: Stream Status

### Level 2: ROC Payload Banks

| Component | PDF Spec | coda-fb Implementation | Parser Validation |
|-----------|----------|------------------------|-------------------|
| **Tag** | 0xFF30 | Variable (from input) | ⚠ Not strictly validated |
| **Type** | 0x20 (Segment) | Variable (from input) | ⚠ Not strictly validated |
| **Structure** | ROC Time Slice Bank | ROC payload (passed through) | ✓ Length validated |
| **Data** | Module data | Module data (CODA header stripped) | ✓ Boundary checked |

**Code Reference:** Line 394
```cpp
std::vector<uint8_t> rocBank(slice.payload.begin() + 32, slice.payload.end());
```

**IMPORTANT:** coda-fb strips the first 32 bytes (8 words) of CODA block header before writing ROC banks:
- Input: CODA block header (8 words) + ROC bank data
- Output: ROC bank data only (header stripped)
- Word 8 validation: Must be 0xC0DA0100 (line 382-391)

## Byte Order and Endianness

### PDF Specification
- EVIO6 uses **BIG endian** byte order
- Magic number 0xC0DA0100 is stored in big-endian format
- Bit 31 of Word 6 indicates endianness (1 = big-endian)

### coda-fb Implementation
- **File/Record Headers:** BIG endian (line 523-527)
- **ROC Payload Data:** Original endianness preserved (line 534-544)

**Code Reference - Header Byte Swap (line 523-527):**
```cpp
for (size_t i = 0; i < eventWords.size(); i++) {
    uint32_t val = eventWords[i];
    outputWords[i] = ((val & 0x000000FF) << 24) |
                     ((val & 0x0000FF00) << 8) |
                     ((val & 0x00FF0000) >> 8) |
                     ((val & 0xFF000000) >> 24);  // Convert to BIG endian
}
```

**Code Reference - Payload Preservation (line 537-539):**
```cpp
std::memcpy(output.data() + currentSize,
            rocBank.data(),
            rocBank.size());  // ROC data copied as-is, no byte swap
```

## Observed File Structure

From `hexdump` of `frames_thread0_file0000.evio`:

```
Offset   Data                                     Interpretation
------   ---------------------------------------- --------------
0x0000   45 56 49 4f                              File ID: "EVIO"
0x0004   00 00 00 00                              File Number: 0
0x0008   00 00 00 0e                              Header Length: 14
0x000c   00 00 00 00                              Record Count: 0
0x0010   00 00 00 00                              Index Array Length: 0
0x0014   00 00 00 06                              Bit Info + Version: 0x06
0x0018   00 00 00 00                              User Header Length: 0
0x001c   c0 da 01 00                              Magic: 0xC0DA0100 ✓
...

0x0038   00 00 00 35                              Record Length: 53 words
0x003c   00 00 00 00                              Record Number: 0
0x0040   00 00 00 0e                              Header Length: 14
0x0044   00 00 00 01                              Event Count: 1
0x0048   00 00 00 00                              Index Length: 0
0x004c   80 00 42 06                              Bit Info: 0x80004206
0x0050   00 00 00 00                              User Header Length: 0
0x0054   c0 da 01 00                              Magic: 0xC0DA0100 ✓
...

0x0070   00 00 00 26                              Agg Bank Length: 38 words
0x0074   ff 60 10 01                              Tag=0xFF60, Type=0x10, Status=0x01 ✓
0x0078   00 00 00 07                              Stream Info Length: 7 words
0x007c   ff 31 20 01                              Tag=0xFF31, Type=0x20, Status=0x01 ✓
0x0080   32 01 00 03                              TSS: Tag=0x32, Type=0x01, Len=3 ✓
0x0084   00 00 00 06                              Frame Number: 6
0x0088   00 06 00 00                              Timestamp Low: 0x00060000
0x008c   00 00 00 00                              Timestamp High: 0x00000000
0x0090   42 01 00 01                              AIS: Tag=0x42, Type=0x01, Len=1 ✓
0x0094   00 0c 00 00                              ROC ID: 0x000C
```

**✓ Confirms Implementation:**
- TSS tag is **0x32** (not 0x31)
- AIS tag is **0x42** (not 0x41)
- All magic numbers correct
- Big-endian byte order throughout

## Validation Strategy

The parser validates against the **actual implementation**, not the PDF:

### ✅ Strict Validation (Must Match)
1. File ID = 0x4556494F
2. Header lengths = 14 words
3. Magic numbers = 0xC0DA0100
4. Version = 6
5. Aggregated Frame tag = 0xFF60, type = 0x10
6. Stream Info tag = 0xFF31, type = 0x20
7. **TSS tag = 0x32** (implementation), type = 0x01, length = 3
8. **AIS tag = 0x42** (implementation), type = 0x01
9. Length consistency (no overruns)

### ⚠ Warnings (Non-Critical)
1. Unexpected TSS length (not 3)
2. Missing expected ROC banks
3. Timestamp inconsistencies

### ❌ Errors (Critical Failures)
1. Wrong file ID
2. Wrong magic number
3. Wrong version
4. Wrong tags/types in frame structure
5. Length fields causing buffer overruns
6. Malformed nesting

## Key Findings Summary

### Discrepancies: PDF vs Implementation

| Item | PDF Specification | coda-fb Implementation | Impact |
|------|-------------------|------------------------|--------|
| TSS Tag | 0x31 | **0x32** | 🔴 Files fail PDF validation |
| AIS Tag | 0x41 | **0x42** | 🔴 Files fail PDF validation |
| Record Count (file hdr) | Actual count | 0 (unknown) | ⚠ Informational only |
| ROC Bank Tags | 0xFF30 | Variable (preserved) | ⚠ Not enforced |

### Why Discrepancies Exist

1. **Different Tag Namespace:**
   - PDF uses tags 0x31, 0x41 for segments
   - Implementation uses 0x32, 0x42 (offset by 1)
   - Possible reason: Avoiding collision with other CODA formats

2. **Implementation Flexibility:**
   - ROC payload banks preserve original tags
   - Not all fields strictly defined in code
   - Evolution of format between PDF and implementation

3. **Version Evolution:**
   - PDF may document earlier version
   - Implementation may have newer conventions
   - Both claim EVIO version 6

### Recommendations

1. **For Validation:**
   - Use `evio_event_parser` which validates against **actual implementation**
   - Do not strictly enforce PDF tag values (0x31, 0x41)
   - Focus on structural integrity and length consistency

2. **For Documentation:**
   - Update PDF to reflect actual tags (0x32, 0x42)
   - Or document implementation as "extended EVIO6"
   - Clearly mark differences between spec and practice

3. **For Future Development:**
   - Codify the actual format in a machine-readable schema
   - Auto-generate validators from schema
   - Maintain backward compatibility with existing files

## Parser Exit Codes

- **0**: File is valid according to coda-fb implementation
- **1**: File has errors (structural, length, magic number, etc.)

Use `--verbose` flag to see detailed structure during parsing.
