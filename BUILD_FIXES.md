# Build System Fixes for E2SAR Receiver

## Problem 1: Protobuf Library Conflict

### Error Message
```
/usr/bin/ld: warning: libprotobuf.so.32, needed by /usr/local/lib/libgrpc++.so, may conflict with libprotobuf.so.25
/usr/bin/ld: /usr/local/lib64/libe2sar.a(meson-generated_.._loadbalancer.pb.cc.o): undefined reference to symbol '_ZN6google8protobuf5Arena23AllocateAlignedWithHookEmPKSt9type_info'
/usr/bin/ld: /usr/local/lib64/libprotobuf.so.32: error adding symbols: DSO missing from command line
```

## Problem 2: Abseil Library Conflict

### Error Message
```
/usr/bin/ld: /usr/local/lib64/libe2sar.a(meson-generated_.._loadbalancer.grpc.pb.cc.o): undefined reference to symbol '_ZN4absl12lts_202301255MutexD1Ev'
/usr/bin/ld: /usr/local/lib64/libabsl_synchronization.so.2301.0.0: error adding symbols: DSO missing from command line
```

### Root Cause

The system has **multiple library conflicts**:

**Protobuf:**
- **System version**: `libprotobuf.so.25` in `/usr/lib64` (from RHEL9 packages)
- **Local version**: `libprotobuf.so.32` in `/usr/local/lib64` (newer version)

**Abseil (absl):**
- **System version**: Various absl libraries in `/usr/lib64`
- **Local version**: `libabsl_synchronization.so.2301.0.0` and other absl libraries in `/usr/local/lib64`

The **E2SAR library** was compiled against the `/usr/local` versions of both protobuf and Abseil, but the build system was finding and linking against the system versions, causing symbol mismatches.

### Solution

Modified `meson.build` to:

1. **Explicitly search for protobuf in `/usr/local` first**:
   ```meson
   protobuf_lib = compiler.find_library('protobuf',
       dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'],
       required: true)
   ```

2. **Explicitly search for required Abseil libraries in `/usr/local` first**:
   ```meson
   absl_sync_lib = compiler.find_library('absl_synchronization',
       dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'],
       required: true)
   absl_time_lib = compiler.find_library('absl_time',
       dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'],
       required: true)
   ```

3. **Add `/usr/local/lib64` to linker search path and runtime path**:
   ```meson
   linker_flags = ['-L/usr/local/lib64', '-Wl,-rpath,/usr/local/lib64']
   ```

4. **Create explicit dependency declaration including all required libraries**:
   ```meson
   protobuf_dep = declare_dependency(
       include_directories: include_directories('/usr/local/include'),
       dependencies: [protobuf_lib, absl_sync_lib, absl_time_lib]
   )
   ```

### Why This Works

1. **Library search order**: By specifying `/usr/local/lib64` first in the `dirs` parameter, meson finds the newer versions before system versions:
   - `libprotobuf.so.32` (instead of .25)
   - `libabsl_synchronization.so.2301.0.0` (instead of older system version)

2. **Runtime path**: The `-Wl,-rpath,/usr/local/lib64` linker flag ensures the executable looks in `/usr/local/lib64` at runtime

3. **Include path**: Explicitly adding `/usr/local/include` ensures we get the matching header files

4. **Explicit dependencies**: By including Abseil libraries in the protobuf dependency declaration, we ensure they're linked in the correct order

### Verification

After this fix:
- The linker will use `/usr/local/lib64/libprotobuf.so.32`
- The linker will use `/usr/local/lib64/libabsl_synchronization.so.2301.0.0`
- All libraries match the versions E2SAR was built against
- No more symbol conflicts or undefined references

### Build Instructions

After making these changes, you need to reconfigure the build:

```bash
cd /path/to/e2sar-receiver
rm -rf builddir          # Remove old build directory
meson setup builddir     # Reconfigure
meson compile -C builddir
```

Or if using the build script:
```bash
./scripts/build.sh --clean --install
```

## Changes Summary

### Files Modified
- **meson.build**:
  - Changed protobuf dependency detection to prefer `/usr/local`
  - Added explicit library search paths to linker flags
  - Added runtime library path (rpath) for `/usr/local/lib64`

### No Code Changes Required
The C++ source code remains unchanged. This is purely a build system configuration issue.

## General Guidance

When you see linker errors like:
```
warning: libXYZ.so.N, needed by ..., may conflict with libXYZ.so.M
undefined reference to symbol '...'
DSO missing from command line
```

This typically means:
1. Multiple versions of a library exist on the system
2. Different components were built with different versions
3. The linker is finding the wrong version

**Solution approach:**
1. Identify which version was used to build dependencies (E2SAR in this case)
2. Force the build to use the same version
3. Use explicit library paths and rpath to ensure consistency

## Platform-Specific Notes

### RHEL9
- System protobuf is 3.14 (libprotobuf.so.25)
- Newer protobuf in `/usr/local` is typically 3.21+ (libprotobuf.so.32)
- E2SAR requires protobuf 3.21+, so it uses the `/usr/local` version
- Dependents must also use the `/usr/local` version

### Other Distributions
Similar issues may occur on:
- Ubuntu/Debian with old system protobuf
- CentOS 7/8 with older packages
- Any system with multiple protobuf installations

**Always check** which version your dependencies were built against and match it.

