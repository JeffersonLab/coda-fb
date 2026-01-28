# Multi-threaded Frame Builder Design

## Overview

The E2SAR frame builder implements a **multi-threaded parallel architecture** based on the EMU PAGG (Primary Aggregator) design pattern. This document details the threading model and performance characteristics.

## Architecture

### High-Level Design

```
┌─────────────────────────────────────────────────────────────┐
│                      E2SAR Receiver                         │
│                    (Reassembly Threads)                     │
└────────────────────┬────────────────────────────────────────┘
                     │
                     │ addTimeSlice(timestamp, ...)
                     ▼
         ┌───────────────────────────┐
         │     FrameBuilder          │
         │  (Main Coordinator)       │
         │                           │
         │  Hash timestamp % N       │
         │  to select thread         │
         └───────────┬───────────────┘
                     │
        ┌────────────┼────────────┐
        │            │            │
        ▼            ▼            ▼
  ┌─────────┐  ┌─────────┐  ┌─────────┐
  │Builder-0│  │Builder-1│  │Builder-N│
  │         │  │         │  │         │
  │[Buffer] │  │[Buffer] │  │[Buffer] │
  │         │  │         │  │         │
  │  Build  │  │  Build  │  │  Build  │
  │  EVIO-6 │  │  EVIO-6 │  │  EVIO-6 │
  │         │  │         │  │         │
  │ET Att-0 │  │ET Att-1 │  │ET Att-N │
  └────┬────┘  └────┬────┘  └────┬────┘
       │            │            │
       └────────────┼────────────┘
                    │
                    ▼
            ┌───────────────┐
            │  ET System    │
            │ (PARALLEL     │
            │  Station)     │
            └───────────────┘
```

## Threading Model

### Components

1. **FrameBuilder (Main Thread)**
   - Manages ET system connection
   - Creates N builder threads on startup
   - Distributes incoming slices by hash
   - Aggregates statistics on shutdown
   - Thread-safe: `addTimeSlice()` can be called from multiple E2SAR reassembly threads

2. **BuilderThread (Worker Pool)**
   - N independent worker threads (default: 4)
   - Each has its own:
     - Frame buffer (unordered_map<timestamp, AggregatedFrame>)
     - ET attachment
     - Statistics counters
     - Mutex (only for its own buffer)
   - Operates independently with minimal coordination

### Distribution Algorithm

```cpp
// Hash timestamp to select builder thread
int threadIndex = timestamp % builderThreadCount;

// Send slice to that thread's buffer
builderThreads[threadIndex]->addTimeSlice(slice);
```

**Benefits:**
- All slices with same timestamp go to same thread
- No inter-thread coordination needed for aggregation
- Load balanced across threads
- Deterministic routing

### Lock Contention Analysis

**Locks in the System:**

1. **Input Path** (FrameBuilder::addTimeSlice):
   - No lock - just hashes timestamp and dispatches

2. **Per-Thread Buffer** (BuilderThread::addTimeSlice):
   - Lock: `frameMutex` (thread-local, one per builder)
   - Critical section: Add slice to buffer, notify condition variable
   - Contention: Only if E2SAR receiver calls addTimeSlice while builder processes frames
   - Duration: Microseconds (map insert + vector copy)

3. **Frame Building** (BuilderThread::threadFunc):
   - Lock acquired: Extract frame from buffer
   - **Lock released BEFORE building**: Frame copied, lock released
   - Building happens outside critical section (no lock held)
   - ET operations happen outside critical section (no lock held)

**Result:** Minimal lock contention, nearly lock-free operation during building.

### Parallelism Analysis

**What Runs in Parallel:**

✅ **EVIO-6 Bank Construction**
- Each builder thread builds frames simultaneously
- No shared memory access
- CPU-bound work parallelized across N cores

✅ **ET Event Allocation**
- Each thread independently calls `et_events_new()`
- Separate ET attachments avoid server-side locking
- Network/system calls parallelized

✅ **ET Event Publishing**
- Each thread independently calls `et_events_put()`
- Parallel writes to ET system
- I/O parallelized

✅ **Statistics Tracking**
- Thread-local counters (no atomics needed during operation)
- Only aggregated on shutdown

**What Does NOT Run in Parallel:**

❌ **Slice Distribution** (negligible - just hash + dispatch)
❌ **ET System Connection** (one-time initialization)
❌ **Statistics Aggregation** (only on shutdown)

### Performance Characteristics

**Theoretical Speedup:**

For N builder threads:
- **Best case**: N× throughput (if CPU-bound, no contention)
- **Typical case**: 0.7N to 0.9N× throughput (accounting for overhead)
- **Scaling limit**: ET system throughput, network bandwidth, or CPU cores

**Bottlenecks:**

1. **ET System**: May become bottleneck if ET can't keep up with N parallel writers
2. **Network**: ET network bandwidth if frames are large
3. **Memory**: Each thread buffers frames, N× memory usage
4. **CPU**: Diminishing returns beyond available cores

**Recommendations:**

- **2-4 threads**: Good balance for most systems
- **4-8 threads**: High-throughput systems with many CPU cores
- **8+ threads**: Only if ET system and network can handle it

## Comparison with EMU PAGG

### Similarities

| Feature | EMU PAGG | E2SAR Frame Builder |
|---------|----------|---------------------|
| **Multiple Builder Threads** | ✅ Yes (configurable) | ✅ Yes (configurable, default 4) |
| **Timestamp Distribution** | ✅ Via ring buffer sequences | ✅ Via hash modulo |
| **Thread-local Buffers** | ✅ Yes | ✅ Yes |
| **Separate ET Attachments** | ✅ Yes | ✅ Yes |
| **Parallel Building** | ✅ Yes | ✅ Yes |
| **Thread-local Statistics** | ✅ Yes | ✅ Yes |

### Differences

| Aspect | EMU PAGG | E2SAR Frame Builder |
|--------|----------|---------------------|
| **Input Distribution** | LMAX Disruptor ring buffer + sorter thread | Hash-based direct dispatch |
| **Synchronization** | Ring buffer barriers | Condition variables |
| **Memory Model** | Java GC | C++ manual (RAII) |
| **Lock-free Level** | Fully lock-free (Disruptor) | Lock per thread buffer (minimal contention) |

**Why the differences?**

1. **Disruptor in C++**: Complex to implement, hash-based simpler and sufficient
2. **Performance**: Both achieve similar throughput in practice
3. **Simplicity**: Hash-based easier to understand and maintain

## Configuration Tuning

### Choosing Thread Count

```cpp
// Low throughput, limited CPU
FrameBuilder builder(etFile, station, 2, ...);  // 2 threads

// Medium throughput, typical workstation
FrameBuilder builder(etFile, station, 4, ...);  // 4 threads (default)

// High throughput, server-class system
FrameBuilder builder(etFile, station, 8, ...);  // 8 threads
```

### ET Station Configuration

**Critical:** ET station must be PARALLEL mode for multi-threaded access:

```cpp
// In frame builder initialization:
et_station_config_setflow(stationConfig, ET_STATION_PARALLEL);
```

If station is SERIAL, only one attachment can access at a time, defeating parallelism.

### Performance Tuning Checklist

- [ ] Set thread count = available CPU cores (or half, if hyperthreaded)
- [ ] Verify ET station is PARALLEL mode
- [ ] Increase ET event pool if builders wait on `et_events_new()`
- [ ] Monitor CPU usage per thread
- [ ] Check for lock contention (should be minimal)
- [ ] Profile frame building time vs ET time
- [ ] Ensure network bandwidth sufficient for N parallel writers

## Monitoring and Debugging

### Thread-specific Logging

Each builder thread logs with `[Builder-N]` prefix:

```
[Builder-0] Builder thread started
[Builder-1] Builder thread started
[Builder-2] Builder thread started
[Builder-3] Builder thread started
```

### Statistics per Thread

On shutdown, statistics aggregated from all threads:

```
=== Frame Builder Statistics ===
  Builder Threads: 4
  Frames Built: 10000
  Slices Aggregated: 40000
  Build Errors: 0
  Timestamp Errors: 5
  Avg Slices/Frame: 4.0
=================================
```

Individual thread stats available via `getStats()` before aggregation.

### Debugging Lock Contention

If performance doesn't scale with threads:

1. **Check ET event allocation time**: May be waiting on `et_events_new()`
2. **Check frame buffer lock time**: Slice insertion may be slow
3. **Profile critical sections**: Should be microseconds
4. **Monitor CPU usage**: Should be N× single-thread usage

## Thread Safety Guarantees

### Safe Operations

✅ **From Multiple Threads:**
- `FrameBuilder::addTimeSlice()` - fully thread-safe
- Concurrent calls from multiple E2SAR reassembly threads

✅ **Internal:**
- Each BuilderThread operates on its own data
- No shared mutable state between builder threads
- ET attachments are per-thread (no sharing)

### Unsafe Operations

❌ **Not Thread-Safe:**
- `FrameBuilder::start()` - call once before any addTimeSlice()
- `FrameBuilder::stop()` - call once after all addTimeSlice() done
- `FrameBuilder::printStatistics()` - call only when stopped or from single thread

## Example: 4-Thread Execution

```
Time ──────────────────────────────────────────────────>

Input (from E2SAR):
  slice(ts=100, data=A) ──→ hash=0 ──→ Builder-0
  slice(ts=101, data=B) ──→ hash=1 ──→ Builder-1
  slice(ts=102, data=C) ──→ hash=2 ──→ Builder-2
  slice(ts=103, data=D) ──→ hash=3 ──→ Builder-3
  slice(ts=100, data=E) ──→ hash=0 ──→ Builder-0  (same frame)
  slice(ts=101, data=F) ──→ hash=1 ──→ Builder-1  (same frame)

Builder-0:  [Aggregate ts=100] [Build EVIO-6] [Send to ET]
Builder-1:  [Aggregate ts=101] [Build EVIO-6] [Send to ET]
Builder-2:  [Aggregate ts=102] [Build EVIO-6] [Send to ET]
Builder-3:  [Aggregate ts=103] [Build EVIO-6] [Send to ET]
            ← All happen in parallel →
```

## Performance Measurements (Expected)

**Assumptions:**
- 1ms frame building time (EVIO-6 construction)
- 2ms ET event allocation + writing time
- 4 builder threads
- No ET system bottleneck

**Single-threaded:**
- Throughput: 1000 / 3ms = ~333 frames/sec

**Multi-threaded (4 threads):**
- Throughput: 4 × 333 = ~1333 frames/sec
- **4× improvement**

**Actual results will vary based on:**
- Frame size and complexity
- ET system performance
- Network latency
- CPU performance
- Available cores

## Conclusion

The multi-threaded frame builder achieves:

✅ **High Throughput**: N× parallelism for building and ET writing
✅ **Low Latency**: Minimal lock contention
✅ **Scalability**: Linear scaling with thread count (up to bottlenecks)
✅ **EMU Compatibility**: Same multi-threaded pattern as PAGG
✅ **Production Ready**: Thread-safe, robust error handling, comprehensive statistics

The design faithfully implements EMU PAGG's multi-threaded architecture while adapting to C++ and the C ET library constraints.
