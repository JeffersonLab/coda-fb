# coda-fb tests. Not built by default; 'meson test -C builddir' builds and runs
# them. See test/README.md for what each one covers.

test_inc = include_directories('.', '../src')

# Pure C++ (no E2SAR): statistics primitives and the frame sender.
test_pipeline_stats = executable('test_pipeline_stats',
    'test_pipeline_stats.cpp',
    include_directories: test_inc,
    dependencies: [thread_dep],
    build_by_default: false)
test('pipeline_stats', test_pipeline_stats, timeout: 120)

test_frame_sender = executable('test_frame_sender',
    ['test_frame_sender.cpp', '../src/frame_sender.cpp'],
    include_directories: test_inc,
    dependencies: [thread_dep],
    build_by_default: false)
test('frame_sender', test_frame_sender, timeout: 120)

# Prometheus exposition and the embedded HTTP endpoint (Boost.Beast, no E2SAR).
test_metrics_render = executable('test_metrics_render',
    ['test_metrics_render.cpp', '../src/metrics_render.cpp'],
    include_directories: test_inc,
    dependencies: [thread_dep],
    build_by_default: false)
test('metrics_render', test_metrics_render, timeout: 120)

test_metrics_endpoint = executable('test_metrics_endpoint',
    ['test_metrics_endpoint.cpp', '../src/metrics_endpoint.cpp', '../src/metrics_render.cpp'],
    include_directories: test_inc,
    dependencies: [boost_dep, thread_dep],
    build_by_default: false)
test('metrics_endpoint', test_metrics_endpoint, timeout: 120, is_parallel: false)

# Against the real E2SAR EjfatURI.
test_uri_config = executable('test_uri_config',
    ['test_uri_config.cpp', '../src/ejfat_uri_config.cpp'],
    include_directories: test_inc,
    dependencies: receiver_deps,
    link_args: linker_flags,
    build_by_default: false)
test('uri_config', test_uri_config, timeout: 60)

# Not a test: loopback traffic for demonstrating the monitoring stack without
# EJFAT (doc/monitoring.md, section 6). Build with
#   meson compile -C builddir loopback_traffic
loopback_traffic = executable('loopback_traffic',
    'loopback_traffic.cpp',
    dependencies: receiver_deps,
    link_args: linker_flags,
    build_by_default: false)

# The real coda-fb binary. Forwarding needs the frame builder, i.e. ET.
if et_dep.found()
    test('cli_startup', find_program('bash'),
        args: [files('cli_startup_test.sh'), coda_fb_exe.full_path()],
        depends: coda_fb_exe,
        is_parallel: false,
        timeout: 600)

    e2e_driver = executable('e2e_forwarding_driver',
        'e2e_forwarding_driver.cpp',
        dependencies: receiver_deps,
        link_args: linker_flags,
        build_by_default: false)
    foreach scenario : ['incomplete', 'partial-skip']
        test('forwarding_e2e_' + scenario, e2e_driver,
            args: [coda_fb_exe.full_path(), meson.current_build_dir() / 'e2e-' + scenario, scenario],
            depends: coda_fb_exe,
            is_parallel: false,
            timeout: 300)
    endforeach
endif
