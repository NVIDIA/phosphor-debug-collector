phosphor_dump_manager_sources += [
    'dump-extensions/nvidia-dumps/dump-extensions.cpp',
    'dump-extensions/nvidia-dumps/dump_manager_system.cpp',
    'dump-extensions/nvidia-dumps/system_dump_entry.cpp',
    'dump-extensions/nvidia-dumps/oem_dump_utils.cpp',
]

if get_option('retimer-debug-mode').enabled()
    phosphor_dump_manager_sources += [
        'dump-extensions/nvidia-dumps/retimer_debug_mode_state.cpp',
    ]
endif

# Per-collector binaries: all built unconditionally. Platform support for each
# dump type is declared by Configuration.Dump records in entity-manager, not a
# build-time flag; an unused collector is never forked at runtime.

nvidia_collectors_incdir = include_directories(
    '.',
    'collectors',
)

nvidia_collector_dep = [
    sdbusplus_dep,
    phosphor_logging_dep,
    nlohmann_json_dep,
]

nvidia_collector_install_dir = get_option('libexecdir') / 'phosphor-debug-collector'

# Shared helper sources built into every collector binary.
nvidia_collector_common_sources = [
    'collectors/common/em_args.cpp',
    'collectors/common/collector_helpers.cpp',
]

nvidia_collector_targets = [
    ['nv-collector-cpld', 'collectors/cpld/main.cpp'],
    ['nv-collector-fpga', 'collectors/fpga/main.cpp'],
    ['nv-collector-erot', 'collectors/erot/main.cpp'],
    ['nv-collector-sma', 'collectors/sma/main.cpp'],
    ['nv-collector-fw-attrs', 'collectors/fw_attrs/main.cpp'],
    ['nv-collector-hw-checkout', 'collectors/hw_checkout/main.cpp'],
    ['nv-collector-cpu-diag', 'collectors/cpu_diag/main.cpp'],
    # NetIR + GPUDeviceDiagnostics dispatch invokes nsm-dump-tool directly
    # from dump_manager_system.cpp (nsmDump); no per-type wrapper binary.
    ['nv-collector-glacier-i2c-log', 'collectors/glacier_i2c_log/main.cpp'],
]

foreach c : nvidia_collector_targets
    executable(
        c[0],
        [c[1]] + nvidia_collector_common_sources,
        dependencies: nvidia_collector_dep,
        include_directories: nvidia_collectors_incdir,
        install: true,
        install_dir: nvidia_collector_install_dir,
    )
endforeach

# TODO: per-collector unit tests under tests/, gated by the `tests` option.

if get_option('rot-dump-collector').allowed()
    subdir('rot_dump')
endif
