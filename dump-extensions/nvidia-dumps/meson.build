phosphor_dump_manager_sources += [
    'dump-extensions/nvidia-dumps/dump-extensions.cpp',
    'dump-extensions/nvidia-dumps/dump_manager_system.cpp',
    'dump-extensions/nvidia-dumps/system_dump_entry.cpp',
    'dump-extensions/nvidia-dumps/oem_dump_utils.cpp',
]

if get_option('vhmc-host').enabled()
    phosphor_dump_manager_sources += [
        'dump-extensions/nvidia-dumps/vhmc_bmc_dump.cpp',
    ]
endif

if get_option('retimer-debug-mode').enabled()
    phosphor_dump_manager_sources += [
        'dump-extensions/nvidia-dumps/retimer_debug_mode_state.cpp',
    ]
endif

if get_option('rot-dump-collector').allowed()
    subdir('rot_dump')
endif
