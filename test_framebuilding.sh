#!/bin/bash
#
# Test script for verifying frame building modes and multithreading
#
# This script demonstrates and tests:
# 1. Default behavior (reassembly-only)
# 2. Frame building mode (when enabled)
# 3. Multithreading in both modes
#

set -e  # Exit on error

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test configuration
EJFAT_URI='ejfat://test-token@localhost:18347/lb/1?data=192.168.1.100:10000'
DATA_IP='192.168.1.100'
DATA_PORT=10000
TEST_DIR='/tmp/coda-fb-test'
BINARY='./coda-fb'

echo "========================================="
echo "CODA Frame Builder Test Suite"
echo "========================================="
echo ""

# Check if binary exists
if [ ! -f "$BINARY" ]; then
    echo -e "${RED}ERROR: Binary not found: $BINARY${NC}"
    echo "Please build the project first"
    exit 1
fi

# Create test directory
mkdir -p "$TEST_DIR"
echo -e "${GREEN}✓${NC} Created test directory: $TEST_DIR"
echo ""

#
# TEST 1: Verify Default Behavior (Reassembly-Only)
#
echo "========================================="
echo "TEST 1: Default Behavior (Reassembly-Only)"
echo "========================================="
echo "Verifying that frame building is disabled by default"
echo ""
echo "Command:"
echo "$BINARY \\"
echo "  -u '$EJFAT_URI' \\"
echo "  --ip $DATA_IP --port $DATA_PORT \\"
echo "  --output-dir $TEST_DIR/test1 \\"
echo "  --prefix events --threads 2 \\"
echo "  --withcp=0 2>&1 | head -20"
echo ""

mkdir -p "$TEST_DIR/test1"

# Run for a few seconds and capture output
timeout 3 $BINARY \
  -u "$EJFAT_URI" \
  --ip $DATA_IP --port $DATA_PORT \
  --output-dir "$TEST_DIR/test1" \
  --prefix events --threads 2 \
  --withcp=0 2>&1 | tee "$TEST_DIR/test1_output.log" || true

echo ""
echo "Verification:"
if grep -q "Frame builder: DISABLED (reassembly-only mode)" "$TEST_DIR/test1_output.log"; then
    echo -e "${GREEN}✓ PASS${NC}: Frame builder is disabled by default"
else
    echo -e "${RED}✗ FAIL${NC}: Frame builder should be disabled by default"
fi

if grep -q "Reassembly threads: 2" "$TEST_DIR/test1_output.log"; then
    echo -e "${GREEN}✓ PASS${NC}: Reassembly is using 2 threads as requested"
else
    echo -e "${RED}✗ FAIL${NC}: Reassembly should be using 2 threads"
fi

echo ""

#
# TEST 2: Verify Frame Building Mode (When Enabled)
#
echo "========================================="
echo "TEST 2: Frame Building Mode (Enabled)"
echo "========================================="
echo "Verifying that frame building works when explicitly enabled"
echo ""
echo "Command:"
echo "$BINARY \\"
echo "  -u '$EJFAT_URI' \\"
echo "  --ip $DATA_IP --port $DATA_PORT \\"
echo "  --enable-framebuild=1 \\"
echo "  --fb-output-dir $TEST_DIR/test2 \\"
echo "  --fb-threads 3 --threads 2 \\"
echo "  --withcp=0 2>&1 | head -30"
echo ""

mkdir -p "$TEST_DIR/test2"

# Run for a few seconds and capture output
timeout 3 $BINARY \
  -u "$EJFAT_URI" \
  --ip $DATA_IP --port $DATA_PORT \
  --enable-framebuild=1 \
  --fb-output-dir "$TEST_DIR/test2" \
  --fb-threads 3 --threads 2 \
  --withcp=0 2>&1 | tee "$TEST_DIR/test2_output.log" || true

echo ""
echo "Verification:"
if grep -q "Frame builder: ENABLED" "$TEST_DIR/test2_output.log"; then
    echo -e "${GREEN}✓ PASS${NC}: Frame builder is enabled when requested"
else
    echo -e "${RED}✗ FAIL${NC}: Frame builder should be enabled"
fi

if grep -q "Builder Threads: 3" "$TEST_DIR/test2_output.log"; then
    echo -e "${GREEN}✓ PASS${NC}: Frame builder is using 3 threads as requested"
else
    echo -e "${RED}✗ FAIL${NC}: Frame builder should be using 3 threads"
fi

if grep -q "Reassembly threads: 2" "$TEST_DIR/test2_output.log"; then
    echo -e "${GREEN}✓ PASS${NC}: Reassembly is still using 2 threads"
else
    echo -e "${RED}✗ FAIL${NC}: Reassembly should be using 2 threads"
fi

echo ""

#
# TEST 3: Verify Thread Count
#
echo "========================================="
echo "TEST 3: Multithreading Verification"
echo "========================================="
echo "Verifying that multiple threads are actually running"
echo ""
echo "This test requires actual network traffic to verify threading"
echo "When running with live data, you can verify with:"
echo "  ps -eLf | grep coda-fb | wc -l"
echo ""
echo "Expected thread counts:"
echo "  - Reassembly-only with --threads 4: ~6+ threads"
echo "    (4 receivers + main + stats)"
echo "  - Frame building with --threads 4 --fb-threads 2: ~8+ threads"
echo "    (4 receivers + 2 builders + main + stats)"
echo ""
echo -e "${YELLOW}MANUAL TEST${NC}: Run the following and monitor with 'htop' or 'ps':"
echo ""
echo "# Terminal 1: Run with high thread count"
echo "$BINARY \\"
echo "  -u '$EJFAT_URI' \\"
echo "  --ip $DATA_IP --port $DATA_PORT \\"
echo "  --threads 8 --enable-framebuild=1 \\"
echo "  --fb-output-dir $TEST_DIR/stress \\"
echo "  --fb-threads 4 --withcp=0"
echo ""
echo "# Terminal 2: Monitor threads"
echo "watch -n 1 'ps -eLf | grep coda-fb | grep -v grep'"
echo ""

#
# TEST 4: Verify No Framebuilding in Default Mode
#
echo "========================================="
echo "TEST 4: Verify No Framebuilding Overhead in Default Mode"
echo "========================================="
echo "Ensuring that default mode does not allocate framebuilding resources"
echo ""
echo "When framebuilding is disabled:"
echo "  - No ET connections should be made"
echo "  - No frame builder threads should be created"
echo "  - No aggregation should occur"
echo ""

timeout 3 $BINARY \
  -u "$EJFAT_URI" \
  --ip $DATA_IP --port $DATA_PORT \
  --output-dir "$TEST_DIR/test4" \
  --prefix events --threads 1 \
  --withcp=0 2>&1 | tee "$TEST_DIR/test4_output.log" || true

echo ""
echo "Verification:"
if ! grep -q "Initializing frame builder" "$TEST_DIR/test4_output.log"; then
    echo -e "${GREEN}✓ PASS${NC}: No frame builder initialization in default mode"
else
    echo -e "${RED}✗ FAIL${NC}: Frame builder should not initialize in default mode"
fi

if ! grep -q "Builder Threads" "$TEST_DIR/test4_output.log"; then
    echo -e "${GREEN}✓ PASS${NC}: No builder threads in default mode"
else
    echo -e "${RED}✗ FAIL${NC}: Builder threads should not be created in default mode"
fi

echo ""

#
# TEST 5: Error Handling
#
echo "========================================="
echo "TEST 5: Error Handling"
echo "========================================="
echo ""

echo "Test 5a: Missing output directory in default mode"
$BINARY -u "$EJFAT_URI" --ip $DATA_IP --port $DATA_PORT --withcp=0 2>&1 | \
  grep -q "Reassembly-only mode requires --output-dir" && \
  echo -e "${GREEN}✓ PASS${NC}: Correctly requires --output-dir in default mode" || \
  echo -e "${RED}✗ FAIL${NC}: Should require --output-dir in default mode"

echo ""
echo "Test 5b: Missing output in framebuilding mode"
$BINARY -u "$EJFAT_URI" --ip $DATA_IP --port $DATA_PORT \
  --enable-framebuild=1 --withcp=0 2>&1 | \
  grep -q "Frame builder mode requires at least one output" && \
  echo -e "${GREEN}✓ PASS${NC}: Correctly requires output in framebuilding mode" || \
  echo -e "${RED}✗ FAIL${NC}: Should require output in framebuilding mode"

echo ""

#
# Summary
#
echo "========================================="
echo "TEST SUMMARY"
echo "========================================="
echo ""
echo "Test logs saved to: $TEST_DIR/"
echo ""
echo "To test with live data:"
echo "  1. Set up EJFAT control plane"
echo "  2. Configure data source to send to $DATA_IP:$DATA_PORT"
echo "  3. Run one of the example commands above"
echo "  4. Monitor statistics output for frame counts"
echo "  5. Verify output files are created"
echo ""
echo -e "${GREEN}All automated tests completed!${NC}"
echo ""
echo "Additional manual tests recommended:"
echo "  - Test with actual network traffic"
echo "  - Verify EVIO-6 output file format"
echo "  - Test ET system integration"
echo "  - Stress test with high data rates"
echo "  - Verify clean shutdown with Ctrl+C"
echo ""
