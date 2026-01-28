# E2SAR Library API Changes (v0.1.5)

This document describes the API changes required to adapt to E2SAR library version 0.1.5.

## Summary of Changes

The E2SAR library has undergone API changes that require modifications to client code:

1. **getStats() return type changed** - Now returns a Boost tuple instead of a struct
2. **get_FDStats() method removed** - Port statistics no longer available via this method
3. **Reassembler constructor signature changed** - Now requires explicit `ip::address` parameter
4. **get_dataIP() method removed** - Data IP is no longer accessible after construction
5. **get_recvPorts() method removed** - Receive ports are no longer accessible after construction

## Detailed Changes

### 1. getStats() Return Type Change

**Old API:**
```cpp
auto stats = reassembler->getStats();
std::cout << stats.reassemblyLoss << std::endl;
std::cout << stats.enqueueLoss << std::endl;
std::cout << stats.dataErrCnt << std::endl;
std::cout << stats.grpcErrCnt << std::endl;
std::cout << stats.lastE2SARError << std::endl;
```

**New API:**
```cpp
// getStats() returns: boost::tuples::tuple<eventsRecvd, eventsReassembled, dataErrCnt, reassemblyLoss, enqueueLoss, lastE2SARError>
auto stats = reassembler->getStats();
std::cout << stats.get<0>() << std::endl;  // eventsRecvd (uint64_t)
std::cout << stats.get<1>() << std::endl;  // eventsReassembled (uint64_t)
std::cout << stats.get<2>() << std::endl;  // dataErrCnt (int)
std::cout << stats.get<3>() << std::endl;  // reassemblyLoss (int)
std::cout << stats.get<4>() << std::endl;  // enqueueLoss (int)
std::cout << stats.get<5>() << std::endl;  // lastE2SARError (E2SARErrorc)
```

**Tuple Element Mapping:**
- Index 0: `eventsRecvd` (uint64_t) - Total events received
- Index 1: `eventsReassembled` (uint64_t) - Total events reassembled
- Index 2: `dataErrCnt` (int) - Data error count
- Index 3: `reassemblyLoss` (int) - Events lost during reassembly
- Index 4: `enqueueLoss` (int) - Events lost during enqueue
- Index 5: `lastE2SARError` (E2SARErrorc) - Last error code

### 2. get_FDStats() Method Removed

**Old API:**
```cpp
auto fdStats = reassembler->get_FDStats();
if (!fdStats.has_error()) {
    for (auto fds: fdStats.value()) {
        std::cout << "Port " << fds.first << ": " << fds.second << " fragments" << std::endl;
    }
}
```

**New API:**
```cpp
// This functionality is no longer available
// Per-port fragment statistics cannot be retrieved
```

**Workaround:**
Remove code that depends on per-port statistics, or track manually if needed.

### 3. Reassembler Constructor Changes

**Old API (auto-detect IP):**
```cpp
Reassembler(const EjfatURI &uri, u_int16_t starting_port,
            std::vector<int> cores, const ReassemblerFlags &flags);

Reassembler(const EjfatURI &uri, u_int16_t starting_port,
            size_t num_threads, const ReassemblerFlags &flags);
```

**New API (explicit IP required):**
```cpp
Reassembler(const EjfatURI &uri, ip::address data_ip, u_int16_t starting_port,
            std::vector<int> cores, const ReassemblerFlags &flags);

Reassembler(const EjfatURI &uri, ip::address data_ip, u_int16_t starting_port,
            size_t num_threads, const ReassemblerFlags &flags);
```

**Migration:**
```cpp
// Extract IP from URI when auto-detection is needed
boost::asio::ip::address data_ip;
if (autoIP) {
    // Use IPv6 or IPv4 based on preference
    // Note: get_dataAddrv4/v6 returns Result<std::pair<ip::address, port>>
    if (preferV6) {
        auto data_addr = uri.get_dataAddrv6();
        if (data_addr.has_error()) {
            // Handle error
        }
        data_ip = data_addr.value().first;  // Extract IP from pair
    } else {
        auto data_addr = uri.get_dataAddrv4();
        if (data_addr.has_error()) {
            // Handle error
        }
        data_ip = data_addr.value().first;  // Extract IP from pair
    }
} else {
    data_ip = boost::asio::ip::make_address(recvIP);
}

// Now create reassembler with explicit IP
reasPtr = new Reassembler(uri, data_ip, recvStartPort, coreList, rflags);
```

### 4. get_dataIP() Method Removed

**Old API:**
```cpp
std::cout << "Listening on: " << reasPtr->get_dataIP() << std::endl;
```

**New API:**
```cpp
// Store the IP address locally before passing to constructor
boost::asio::ip::address data_ip = /* obtained earlier */;
reasPtr = new Reassembler(uri, data_ip, recvStartPort, numThreads, rflags);

// Use the locally stored IP for display
std::cout << "Listening on: " << data_ip << std::endl;
```

### 5. get_recvPorts() Method Removed

**Old API:**
```cpp
std::cout << "Ports: " << reasPtr->get_recvPorts().first
          << "-" << reasPtr->get_recvPorts().second << std::endl;
```

**New API:**
```cpp
// Calculate port range manually based on thread count
std::cout << "Ports: " << recvStartPort
          << "-" << (recvStartPort + numThreads - 1) << std::endl;
```

## Complete Migration Example

**Before (Old API):**
```cpp
// Auto-detect IP from URI
reasPtr = new Reassembler(uri, recvStartPort, numThreads, rflags);

std::cout << "Listening on: " << reasPtr->get_dataIP() << ":"
          << reasPtr->get_recvPorts().first << "-"
          << reasPtr->get_recvPorts().second << std::endl;

auto stats = reasPtr->getStats();
std::cout << "Reassembly loss: " << stats.reassemblyLoss << std::endl;
std::cout << "Enqueue loss: " << stats.enqueueLoss << std::endl;

auto fdStats = reasPtr->get_FDStats();
if (!fdStats.has_error()) {
    for (auto fds: fdStats.value()) {
        std::cout << "Port " << fds.first << ": " << fds.second << std::endl;
    }
}
```

**After (New API):**
```cpp
// Extract IP from URI (use v4 or v6 based on preference)
// Note: get_dataAddrv4/v6 returns Result<std::pair<ip::address, port>>
auto data_addr = preferV6 ? uri.get_dataAddrv6() : uri.get_dataAddrv4();
if (data_addr.has_error()) {
    std::cerr << "Failed to extract IP" << std::endl;
    return -1;
}
boost::asio::ip::address data_ip = data_addr.value().first;  // Extract IP from pair

// Create reassembler with explicit IP
reasPtr = new Reassembler(uri, data_ip, recvStartPort, numThreads, rflags);

// Display connection info using local variables
std::cout << "Listening on: " << data_ip << ":"
          << recvStartPort << "-"
          << (recvStartPort + numThreads - 1) << std::endl;

// Access stats via tuple
auto stats = reasPtr->getStats();
std::cout << "Reassembly loss: " << stats.get<3>() << std::endl;
std::cout << "Enqueue loss: " << stats.get<4>() << std::endl;

// Per-port stats no longer available
// Remove or replace this functionality
```

## Compatibility Notes

- These changes are **breaking** and require code modifications
- No backward compatibility mode is provided
- Existing code using the old API will not compile
- The new API provides more explicit control over IP addressing
- Per-port statistics are no longer available from the library

## Build System Impact

No changes to build dependencies are required. The same libraries are used:
- E2SAR library (now v0.1.5)
- Boost (1.83-1.86)
- gRPC++ (≥1.51.1)
- Protocol Buffers
- GLib 2.0

## Testing Recommendations

After migrating to the new API:
1. Verify IP address extraction works correctly
2. Confirm port ranges are calculated correctly
3. Test statistics reporting with tuple access
4. Remove or replace per-port statistics functionality
5. Test with both explicit IP (`--ip`) and auto-detect (`--autoip`) modes
