# ET Connection Parameters Guide

## Quick Reference

The E2SAR frame builder supports flexible ET system connections via three parameters:

| Parameter | Type | Purpose | Examples |
|-----------|------|---------|----------|
| `etFile` | string | ET system file name | `/tmp/et_sys_pagg` |
| `etHost` | string | ET server hostname/IP | `""`, `localhost`, `et-server.jlab.org`, `192.168.1.100` |
| `etPort` | int | ET server TCP port | `0`, `11111`, `23911` |

## Connection Scenarios

### Scenario 1: Local ET System (Broadcast Discovery)

**Use Case:** ET system running on the same machine, or on local network

**Configuration:**
```cpp
FrameBuilder builder(
    "/tmp/et_sys_pagg",  // ET file name
    "",                  // Empty host = use broadcast
    0,                   // Port 0 = use default
    "STATION",
    4                    // threads
);
```

**Command Line:**
```bash
e2sar_receiver \
  --et-file /tmp/et_sys_pagg \
  --et-host "" \
  --et-port 0 \
  --et-station E2SAR_PAGG
```

**How it works:**
- Broadcasts UDP packets on local network
- ET system responds with its location
- Automatic discovery, no configuration needed

**When to use:**
- ET on localhost
- ET on same subnet
- Don't know ET server port
- Simplest setup

---

### Scenario 2: Remote ET System (Direct Connection by Hostname)

**Use Case:** ET system on remote machine with DNS hostname

**Configuration:**
```cpp
FrameBuilder builder(
    "/tmp/et_sys_pagg",
    "et-server.jlab.org",  // Hostname
    11111,                 // Specific port
    "STATION",
    4
);
```

**Command Line:**
```bash
e2sar_receiver \
  --et-file /tmp/et_sys_pagg \
  --et-host et-server.jlab.org \
  --et-port 11111 \
  --et-station E2SAR_PAGG
```

**How it works:**
- Resolves hostname via DNS
- Opens TCP connection directly
- No broadcast, faster connection

**When to use:**
- ET on remote machine
- DNS name available
- Specific port required
- Firewall blocks broadcast

---

### Scenario 3: Remote ET System (Direct Connection by IP)

**Use Case:** ET system on remote machine, connecting by IP address

**Configuration:**
```cpp
FrameBuilder builder(
    "/tmp/et_sys_pagg",
    "192.168.1.100",  // IP address
    11111,
    "STATION",
    4
);
```

**Command Line:**
```bash
e2sar_receiver \
  --et-file /tmp/et_sys_pagg \
  --et-host 192.168.1.100 \
  --et-port 11111 \
  --et-station E2SAR_PAGG
```

**How it works:**
- Connects directly to IP address
- No DNS resolution needed
- Opens TCP connection

**When to use:**
- DNS not available
- Explicit IP control needed
- Testing with specific IP
- Firewall blocks broadcast

---

## ET System Startup Examples

### Local ET System
```bash
# Start ET on default port (broadcasts on local network)
et_start -f /tmp/et_sys_pagg -n 1000 -s 2000000

# Receiver can find it automatically
e2sar_receiver --et-file /tmp/et_sys_pagg --et-host "" --et-port 0
```

### ET System on Specific Port
```bash
# Start ET on port 11111
et_start -f /tmp/et_sys_pagg -p 11111 -n 1000 -s 2000000

# Receiver must specify the port
e2sar_receiver --et-file /tmp/et_sys_pagg --et-port 11111
```

### ET System for Remote Access
```bash
# Start ET on specific port for remote access
et_start -f /tmp/et_sys_pagg -p 11111 -n 1000 -s 2000000

# Remote receiver connects by hostname or IP
e2sar_receiver --et-file /tmp/et_sys_pagg \
               --et-host et-server.jlab.org \
               --et-port 11111
```

---

## Parameter Details

### ET File Name (`etFile`)

**What it is:**
- The name the ET system uses to identify itself
- NOT a path to a physical file on disk
- Used for lookup/matching when connecting

**Examples:**
- `/tmp/et_sys_pagg` - typical format
- `et_clas12` - shorter format
- `/et/run_1234` - run-specific

**Notes:**
- Must match exactly what ET system was started with
- Case-sensitive
- Can include directory-like structure but it's just a name

### ET Host (`etHost`)

**Empty String (`""`)**
- Uses UDP broadcast to find ET system
- Searches local network
- ET system must be on same subnet or localhost
- Automatic discovery

**Hostname (`"localhost"`, `"et-server.jlab.org"`)**
- Direct TCP connection
- Requires DNS resolution
- Can be any valid hostname
- Examples:
  - `"localhost"` - same machine
  - `"et-server"` - short hostname
  - `"et-server.jlab.org"` - fully qualified
  - `"et01.hall-b.jlab.org"` - specific machine

**IP Address (`"192.168.1.100"`)**
- Direct TCP connection by IP
- No DNS needed
- IPv4 address as string
- Examples:
  - `"127.0.0.1"` - localhost
  - `"192.168.1.100"` - local network
  - `"129.57.35.120"` - Jefferson Lab network

### ET Port (`etPort`)

**Port `0` (Default)**
- Uses ET's default port
- Typically port 11111 for broadcast
- Lets ET choose the port

**Specific Port (`11111`, `23911`, etc.)**
- Connects to exact port number
- Must match ET system's port
- Required for remote connections
- Common values:
  - `11111` - typical ET port
  - `23911` - alternative
  - Any port 1024-65535

---

## Network Requirements

### Broadcast Mode (Empty Host)
**Required:**
- UDP port 11111 open (broadcast)
- Multicast enabled on network
- Same subnet or localhost

**Blocked by:**
- Firewalls between machines
- VLANs without multicast
- VPN connections
- Docker containers (usually)

### Direct Mode (Hostname/IP)
**Required:**
- TCP port (specified by `etPort`) open
- Network route to host
- DNS resolution (for hostname)

**Works through:**
- Firewalls (if port open)
- VLANs
- VPNs
- Docker networks (with port mapping)

---

## Troubleshooting

### "Failed to open ET system"

**If using broadcast (empty host):**
1. Check ET system is running: `et_monitor -f /tmp/et_sys_pagg`
2. Verify on same network/subnet
3. Check firewall isn't blocking UDP
4. Try direct connection instead

**If using direct connection:**
1. Verify hostname/IP is correct: `ping et-server.jlab.org`
2. Check port is correct: `telnet et-server.jlab.org 11111`
3. Verify ET is listening on that port
4. Check firewall rules

### Connection Times Out

- Increase timeout (currently 10 seconds hardcoded)
- Check network connectivity
- Verify ET system is running
- Try `et_monitor` command from same machine

### "Station attach failed"

- ET system may be out of attachments
- Station may not exist (will be created automatically)
- Check ET configuration

---

## Command Line Examples

### Minimal (Local, Defaults)
```bash
e2sar_receiver --use-et --et-file /tmp/et_sys_pagg
```

### Explicit Local
```bash
e2sar_receiver --use-et \
  --et-file /tmp/et_sys_pagg \
  --et-host "" \
  --et-port 0
```

### Remote by Hostname
```bash
e2sar_receiver --use-et \
  --et-file /tmp/et_sys_pagg \
  --et-host et-server.jlab.org \
  --et-port 11111
```

### Remote by IP
```bash
e2sar_receiver --use-et \
  --et-file /tmp/et_sys_pagg \
  --et-host 192.168.1.100 \
  --et-port 11111
```

### Full Configuration
```bash
e2sar_receiver --use-et \
  --et-file /tmp/et_sys_pagg \
  --et-host et-server.jlab.org \
  --et-port 11111 \
  --et-station E2SAR_PAGG \
  --et-threads 4 \
  --et-event-size 2097152 \
  --timestamp-slop 100 \
  --frame-timeout 1000
```

---

## Testing Connections

### Test 1: ET System is Running
```bash
et_monitor -f /tmp/et_sys_pagg
```
Should show ET statistics if running.

### Test 2: ET Port is Open
```bash
# For broadcast
sudo tcpdump -i any port 11111

# For direct connection
telnet et-server.jlab.org 11111
# or
nc -zv et-server.jlab.org 11111
```

### Test 3: DNS Resolution
```bash
nslookup et-server.jlab.org
# or
host et-server.jlab.org
```

### Test 4: Network Route
```bash
ping et-server.jlab.org
traceroute et-server.jlab.org
```

---

## Best Practices

1. **Development/Testing**: Use broadcast mode (empty host)
   - Simple setup
   - No configuration needed
   - Quick iteration

2. **Production (Same Machine)**: Use localhost
   - Explicit connection
   - No broadcast needed
   - Slightly faster

3. **Production (Remote)**: Use hostname or IP
   - Required for remote ET
   - More reliable than broadcast
   - Works through firewalls

4. **Port Selection**:
   - Use default (0) if possible
   - Document chosen port
   - Coordinate with ET system administrator

5. **Multi-threading**:
   - Use 2-8 builder threads
   - Match to available CPU cores
   - ET station must be PARALLEL mode

---

## See Also

- `FRAMEBUILDER_README.md` - Full frame builder documentation
- `INTEGRATION_EXAMPLE.md` - Integration with E2SAR receiver
- `MULTITHREADED_DESIGN.md` - Threading architecture details
- ET system documentation: https://coda.jlab.org/drupal/
