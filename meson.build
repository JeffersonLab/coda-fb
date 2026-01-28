project('coda-fb', 'cpp',
        version: '1.0.0',
        default_options: ['cpp_std=c++17'],
        license: 'MIT')

# Project information
project_description = 'CODA Frame Builder - Aggregates real-time data frames from EJFAT load balancer for Jefferson Lab DAQ'

# Set default installation directory based on CODA environment variable
# Default: $CODA/Linux-x86_64/bin, fallback to ~/.local/bin if CODA not set
coda_dir = run_command('sh', '-c', 'echo $CODA', check: false).stdout().strip()
home_dir = run_command('sh', '-c', 'echo $HOME', check: false).stdout().strip()

if coda_dir != ''
    message('CODA environment variable detected: ' + coda_dir)
    # Override prefix if not explicitly set by user
    if get_option('prefix') == '/usr/local'
        message('Using CODA-based installation directory')
        # Note: Meson doesn't allow changing prefix after project(),
        # so we'll use this in install_dir instead
    endif
else
    message('CODA environment variable not set, using ~/.local/bin')
endif

# Check for required compiler features
compiler = meson.get_compiler('cpp')

# Find E2SAR library (assuming it's installed)
e2sar_dep = dependency('e2sar', required: true,
    fallback: ['e2sar', 'libe2sar_dep'],
    default_options: ['warning_level=1'])

# System dependencies
thread_dep = dependency('threads', required: true)
glib_dep = dependency('glib-2.0', required: true)

# Protobuf and gRPC dependencies
# Note: E2SAR may be built with a specific protobuf version in /usr/local
# We need to ensure we link against the same version
grpc_dep = dependency('grpc++', version: '>=1.51.1', required: true)

# Find protobuf - prefer /usr/local version to match E2SAR
protobuf_lib = compiler.find_library('protobuf', dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'], required: true)

# Find Abseil libraries needed by E2SAR - prefer /usr/local version
absl_sync_lib = compiler.find_library('absl_synchronization', dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'], required: true)
absl_time_lib = compiler.find_library('absl_time', dirs: ['/usr/local/lib64', '/usr/local/lib', '/usr/lib64', '/usr/lib'], required: true)

protobuf_dep = declare_dependency(
    include_directories: include_directories('/usr/local/include'),
    dependencies: [protobuf_lib, absl_sync_lib, absl_time_lib]
)

# ET library for frame builder (optional)
# ET doesn't typically provide pkg-config, so search manually
et_dep = dependency('et', required: false)
if not et_dep.found()
    message('ET not found via pkg-config, trying manual search...')
    # Try to find ET library manually using compiler.find_library
    # ET may be in standard locations or paths set by LIBRARY_PATH/LD_LIBRARY_PATH
    et_lib = compiler.find_library('et', required: false)
    if et_lib.found()
        message('Found ET library')
        # Also check for ET header
        if compiler.has_header('et.h')
            message('Found ET header et.h')
            et_dep = declare_dependency(
                dependencies: et_lib
            )
        else
            message('WARNING: Found ET library but et.h header not found')
            message('Make sure CPATH includes the ET header directory')
        endif
    else
        message('ET library not found via LIBRARY_PATH')
        message('Set LIBRARY_PATH to include ET library directory if needed')
    endif
endif

# Boost dependencies - via CMake (uses BoostConfig.cmake)
boost_dep = dependency('boost',
    modules: ['system', 'program_options', 'chrono', 'thread', 'filesystem', 'url'],  # <-- add 'url'
    method: 'cmake',
    version: ['>=1.83.0', '<=1.86.0'],
    required: true)

# Boost dependencies - using cmake method since boost doesn't always provide .pc files
#boost_dep = dependency('boost', 
#    modules: ['system', 'program_options', 'chrono', 'thread', 'filesystem'], 
#    method: 'cmake',
#    version: ['>=1.83.0', '<=1.86.0'],
#    required: true)

# Optional dependencies for enhanced features
numa_code = '''
#include <numa.h>
void f() {
  int res = numa_available();
}
'''

linker_flags = ['-L/usr/local/lib64', '-Wl,-rpath,/usr/local/lib64']
if compiler.compiles(numa_code, name: 'NUMA library check')
    add_project_arguments('-DNUMA_AVAILABLE', language: ['cpp'])
    linker_flags += ['-lnuma']
endif

# Check for CPU affinity support
affinity_code = '''
#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>
void f() {
    cpu_set_t set;
    sched_setaffinity(getpid(), sizeof(set), &set); 
}
'''

if compiler.compiles(affinity_code, name: 'CPU Affinity check')
    add_project_arguments('-DAFFINITY_AVAILABLE', language: ['cpp'])
endif

# Check for thread affinity support
thread_affinity_code = '''
#define _GNU_SOURCE
#include <pthread.h>
void f() {
    cpu_set_t set;
    pthread_t thread = pthread_self();
    pthread_setaffinity_np(thread, sizeof(set), &set); 
}
'''

if compiler.compiles(thread_affinity_code, name: 'Thread Affinity check')
    add_project_arguments('-DTHRD_AFFINITY_AVAILABLE', language: ['cpp'])
endif

# Add version information
add_project_arguments('-DCODA_FB_VERSION="' + meson.project_version() + '"',
                     language: ['cpp'])

# Warning level
warning_level = 1

# Source files
receiver_sources = ['src/coda-fb.cpp']

# Add frame builder if ET is available
if et_dep.found()
    receiver_sources += ['src/e2sar_reassembler_framebuilder.cpp']
    add_project_arguments('-DENABLE_FRAME_BUILDER', language: ['cpp'])
endif

# Build the executable
receiver_deps = [e2sar_dep, boost_dep, thread_dep, grpc_dep, protobuf_dep, glib_dep]
if et_dep.found()
    receiver_deps += [et_dep]
endif

# Determine installation directory
# Priority:
#   1. If user specified custom prefix: use <prefix>/bin (highest priority)
#   2. If CODA is set: use $CODA/Linux-x86_64/bin (absolute path, no sudo needed)
#   3. Otherwise: use ~/.local/bin (absolute path, no sudo needed)

# Check if user explicitly set a custom prefix (not the meson default)
user_set_custom_prefix = get_option('prefix') != '/usr/local'

if user_set_custom_prefix
    # User specified custom prefix, use it with default meson behavior
    install_bin_dir = get_option('prefix') / get_option('bindir')
    use_absolute_install = false
    message('Using custom installation prefix: ' + install_bin_dir)
elif coda_dir != ''
    # CODA is set, use absolute CODA directory
    install_bin_dir = coda_dir / 'Linux-x86_64' / 'bin'
    use_absolute_install = true
    message('Installing to CODA directory: ' + install_bin_dir)
else
    # Default: use absolute ~/.local/bin (no sudo needed)
    install_bin_dir = home_dir / '.local' / 'bin'
    use_absolute_install = true
    message('Installing to user directory: ' + install_bin_dir)
endif

if use_absolute_install
    # Use absolute path for CODA or ~/.local installation
    executable('coda-fb',
        receiver_sources,
        dependencies: receiver_deps,
        link_args: linker_flags,
        install: true,
        install_dir: install_bin_dir)
else
    # Use default meson behavior (prefix + bindir)
    executable('coda-fb',
        receiver_sources,
        dependencies: receiver_deps,
        link_args: linker_flags,
        install: true)
endif

# Summary
summary({
    'CODA Frame Builder Version': meson.project_version(),
    'Build Type': get_option('buildtype'),
    'Install Directory': install_bin_dir,
    'C++ Standard': get_option('cpp_std'),
    'NUMA Support': compiler.compiles(numa_code, name: 'NUMA available'),
    'CPU Affinity Support': compiler.compiles(affinity_code, name: 'Affinity available'),
    'Thread Affinity Support': compiler.compiles(thread_affinity_code, name: 'Thread affinity available'),
}, section: 'Configuration')

summary({
    'E2SAR Library': e2sar_dep.found() ? 'Found' : 'Not Found',
    'Boost': boost_dep.found() ? boost_dep.version() : 'Not Found',
    'gRPC++': grpc_dep.found() ? grpc_dep.version() : 'Not Found',
    'Protobuf': protobuf_lib.found() ? 'Found (/usr/local)' : 'Not Found',
    'ET Library': et_dep.found() ? 'Found (frame builder enabled)' : 'Not Found (frame builder disabled)',
}, section: 'Dependencies')
