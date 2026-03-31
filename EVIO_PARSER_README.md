# EVIO6 Event Parser and Validator

## Overview

This tool parses and validates EVIO6-format frame-built event files produced by `coda-fb`. It validates the event structure against both:

1. **CODA Online Data Formats specification** (`coda_data_format.pdf`)
2. **Actual coda-fb writer implementation** (`e2sar_reassembler_framebuilder.cpp`)

The parser performs comprehensive structural validation and reports any discrepancies, missing banks/segments, unexpected tags/types, malformed nesting, or length inconsistencies.

## EVIO6 Event Structure (Frame-Built)

Based on analysis of the PDF and coda-fb source code, the expected structure is:

```
EVIO6 File
├── File Header (14 words = 56 bytes)
│   ├── Word 1: File ID (0x4556494F = "EVIO")
│   ├── Word 2: File Number
│   ├── Word 3: Header Length (14)
│   ├── Word 4: Record Count
│   ├── Word 5: Index Array Length
│   ├── Word 6: Bit Info + Version (low 8 bits = 6)
│   ├── Word 7: User Header Length
│   ├── Word 8: Magic Number (0xC0DA0100)
│   ├── Words 9-10: User Register (64-bit)
│   ├── Words 11-12: Trailer Position (64-bit)
│   └── Words 13-14: User Integers
│
└── Records (multiple)
    ├── Record Header (14 words = 56 bytes)
    │   ├── Word 1: Record Length (inclusive, 32-bit words)
    │   ├── Word 2: Record Number
    │   ├── Word 3: Header Length (14)
    │   ├── Word 4: Event Index Count
    │   ├── Word 5: Index Array Length
    │   ├── Word 6: Bit Info + Version
    │   ├── Word 7: User Header Length
    │   ├── Word 8: Magic Number (0xC0DA0100)
    │   ├── Word 9: Uncompressed Data Length
    │   ├── Word 10: Compression Type + Compressed Length
    │   └── Words 11-14: User Registers (2 x 64-bit)
    │
    └── Event Data
        └── Aggregated Frame Bank (tag=0xFF60, type=0x10)
            ├── Stream Info Bank (tag=0xFF31, type=0x20)
            │   ├── Time Slice Segment (tag=0x32, type=0x01, length=3)
            │   │   ├── Frame Number (32-bit)
            │   │   ├── Timestamp Low (32-bit)
            │   │   └── Timestamp High (32-bit)
            │   │
            │   └── Aggregation Info Segment (tag=0x42, type=0x01, length=N)
            │       └── ROC IDs (N words: ROC_ID | reserved | stream_status)
            │
            └── ROC Payload Banks (one per source)
                ├── ROC Bank Length
                ├── ROC Bank Header (tag | type | stream_status)
                └── ROC Payload Data
```

## Key Tags and Types

### From coda-fb Implementation

| Structure | Tag | Type | Description |
|-----------|-----|------|-------------|
| Aggregated Frame Bank | 0xFF60 | 0x10 (BANK) | Top-level container for frame |
| Stream Info Bank | 0xFF31 | 0x20 (SEGMENT) | Frame metadata container |
| Time Slice Segment | 0x32 | 0x01 (INT) | Frame number and timestamp |
| Aggregation Info Segment | 0x42 | 0x01 (INT) | ROC IDs and status |
| ROC Time Slice Bank | 0xFF30 | 0x20 (SEGMENT) | Per-ROC data (variable) |

### EVIO Data Types

- `0x01` = 32-bit signed int
- `0x10` = Bank (contains nested structures)
- `0x20` = Segment (contains data)

## Discrepancies Between PDF and Implementation

The parser documents and validates the **actual implementation** used by coda-fb:

1. **PDF specifies** tag 0xFF60 for streaming physics events
   - **Implementation uses** 0xFF60 for aggregated frame bank ✓ (matches)

2. **PDF specifies** tag 0xFF31 for Time Info Bank
   - **Implementation uses** 0xFF31 for Stream Info Bank ✓ (matches)

3. **PDF specifies** tag 0x31 for Time Slice Segment
   - **Implementation uses** 0x32 for Time Slice Segment (differs)

4. **PDF specifies** tag 0x41 for Aggregation Info Segment
   - **Implementation uses** 0x42 for Aggregation Info Segment (differs)

The parser validates against the **actual implementation** to ensure files written by coda-fb are correctly structured.

## Build Instructions

### Requirements

- C++17 compiler (g++, clang++)
- Standard C++ library (no external dependencies)

### Compilation

```bash
# Using g++
g++ -std=c++17 -O2 -Wall -Wextra evio_event_parser.cpp -o evio_event_parser

# Using clang++
clang++ -std=c++17 -O2 -Wall -Wextra evio_event_parser.cpp -o evio_event_parser
```

### Using a Makefile

Create a `Makefile`:

```makefile
CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
TARGET = evio_event_parser
SRC = evio_event_parser.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)

test: $(TARGET)
	./$(TARGET) frames_thread0_file0000.evio --verbose

.PHONY: all clean test
```

Then build:

```bash
make
```

## Usage

### Basic Usage

```bash
./evio_event_parser frames_thread0_file0000.evio
```

### Verbose Mode

```bash
./evio_event_parser frames_thread0_file0000.evio --verbose
```

Verbose mode prints detailed information about each structure as it's parsed:
- All header fields
- Bank/segment tags, types, and lengths
- Frame numbers and timestamps
- ROC IDs and stream status

### Example Output

#### Normal Mode
```
EVIO6 Event Parser and Validator
=================================
File: frames_thread0_file0000.evio
Verbose: disabled

Loaded file: frames_thread0_file0000.evio (93315232 bytes)

=== Starting EVIO6 File Parsing ===

=== Parsing Complete ===
Total records processed: 100
File position: 5246232 / 93315232 bytes
Remaining data: 88069000 bytes

=== Validation Summary ===
Status: SUCCESS
Errors: 0
Warnings: 0
==========================
```

#### Verbose Mode Output (excerpt)
```
=== EVIO6 File Header ===
  File ID: 0x4556494f
  File Number: 0
  Header Length: 14 (words)
  Record Count: 0
  Index Array Length: 0 (bytes)
  Version: 6
  Bit Info: 0x00000000
  User Header Length: 0 (bytes)
  Magic Number: 0xc0da0100
  ...

=== EVIO6 Record Header #1 ===
  Record Length: 53 (words (inclusive))
  Record Number: 0
  Header Length: 14 (words)
  Event Index Count: 1
  ...

  === Aggregated Frame Bank ===
    Bank Length: 38 (words (exclusive))
    Tag: 0x0000ff60
    Type: 16 (0x10 = BANK)
    Stream Status: 1

    === Stream Info Bank ===
      Bank Length: 7 (words (exclusive))
      Tag: 0x0000ff31
      Type: 32 (0x20 = SEGMENT)
      Stream Status: 1

      === Time Slice Segment (TSS) ===
        Tag: 0x00000032
        Type: 1 (0x01 = INT)
        Length: 3 (words)
        Frame Number: 6
        Timestamp: 25769803782

      === Aggregation Info Segment (AIS) ===
        Tag: 0x00000042
        Type: 1 (0x01 = INT)
        Length: 1 (ROC count)
        ROC 0: ID=0xc, Status=0x0

    === ROC Payload Bank #0 ===
      ROC Bank Length: 28 (words (exclusive))
      Tag: 0x0000ff30
      Type: 32
      Stream Status: 17
      Payload Size: 108 (bytes)
```

## Validation Rules

The parser validates:

### File Header
- ✓ File ID must be 0x4556494F ("EVIO")
- ✓ Header length must be 14 words
- ✓ Magic number must be 0xC0DA0100
- ✓ Version must be 6

### Record Headers
- ✓ Header length must be 14 words
- ✓ Magic number must be 0xC0DA0100
- ✓ Version must be 6
- ✓ Bit info flags are correctly set

### Event Structure
- ✓ Aggregated Frame Bank tag is 0xFF60, type is 0x10 (BANK)
- ✓ Stream Info Bank tag is 0xFF31, type is 0x20 (SEGMENT)
- ✓ Time Slice Segment tag is 0x32, type is 0x01 (INT), length is 3
- ✓ Aggregation Info Segment tag is 0x42, type is 0x01 (INT)
- ✓ ROC count matches number of ROC banks
- ✓ All lengths are consistent
- ✓ No data extends beyond file boundaries

## Exit Codes

- `0` - Validation successful, no errors
- `1` - Validation failed or errors detected

## Assumptions and Limitations

### Assumptions

1. **Big-Endian Format**: The parser assumes all EVIO6 data is in big-endian byte order (as per specification)
2. **No Compression**: Currently assumes uncompressed records (compression type = 0)
3. **No Index Arrays**: Assumes index array length is 0 (not used by coda-fb)
4. **Frame Building Mode**: Validates structure specific to frame-built events, not ROC raw events

### Limitations

1. **First 100 Records**: For safety, the parser limits processing to the first 100 records by default (easily changed)
2. **No Deep Payload Validation**: ROC payload data is not parsed at the ADC module level (FADC250, etc.)
3. **Basic Timestamp Validation**: Does not validate timestamp consistency across frames
4. **No Event Building Logic**: Does not verify event building correctness, only structural integrity

### Where PDF and Code Differ

When the PDF specification and coda-fb implementation differ, the parser:
- **Validates against the implementation** (what coda-fb actually writes)
- **Documents the discrepancy** in this README
- **Reports as expected** if it matches the implementation

Example: The parser expects TSS tag 0x32 (implementation) not 0x31 (PDF).

## Extending the Parser

To add additional validation:

1. **Deep Payload Parsing**: Add methods to parse FADC250 module data structures
2. **Timestamp Consistency**: Track timestamps across frames and validate ordering
3. **ROC-Specific Validation**: Add per-ROC validation rules based on module type
4. **Compression Support**: Add decompression for compressed records

Example extension point in code:

```cpp
void parseROCPayloadBank(int rocIndex) {
    // Current: just reads and skips payload
    // Extension: call parseROCModuleData() here
}

void parseROCModuleData(const uint8_t* data, size_t len) {
    // Parse FADC250 block header, event headers, data words
    // Validate module-specific structure
}
```

## Files

- `evio_event_parser.cpp` - Main parser implementation (standalone, no dependencies)
- `EVIO_PARSER_README.md` - This file
- `coda_data_format.pdf` - CODA specification reference
- `e2sar_reassembler_framebuilder.cpp` - coda-fb writer implementation reference

## Contact and Support

This parser was developed to validate frame-built EVIO6 files from the coda-fb system.

For issues specific to:
- **EVIO format**: Refer to coda_data_format.pdf
- **coda-fb implementation**: Check e2sar_reassembler_framebuilder.cpp
- **This parser**: Review validation output and error messages
