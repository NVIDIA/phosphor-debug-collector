gtest_dep = dependency('gtest', main: true, disabler: true, required: false)
gmock_dep = dependency('gmock', disabler: true, required: false)
if not gtest_dep.found() or not gmock_dep.found()
    gtest_proj = import('cmake').subproject('googletest', required: false)
    if gtest_proj.found()
        gtest_dep = declare_dependency(
            dependencies: [
                dependency('threads'),
                gtest_proj.dependency('gtest'),
                gtest_proj.dependency('gtest_main'),
            ],
        )
        gmock_dep = gtest_proj.dependency('gmock')
    else
        assert(
            not get_option('tests').allowed(),
            'Googletest is required if tests are enabled',
        )
    endif
endif

dump = declare_dependency(sources: ['../dump_serialize.cpp'])

tests = [
    'debug_inif_test',
    'tar_compress_lock_test',
    'nsm_device_utils_test',
]

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            include_directories: ['.', '../'],
            implicit_include_directories: false,
            dependencies: [
                gtest_dep,
                gmock_dep,
                dump,
                phosphor_logging_dep,
                cereal_dep,
            ],
        ),
        workdir: meson.current_source_dir(),
    )
endforeach

ramoops_manager = declare_dependency(
    sources: [dump_types_hpp, '../ramoops_manager.cpp'],
)

test(
    'ramoops_manager_test',
    executable(
        'ramoops_manager_test',
        'ramoops_manager_test.cpp',
        include_directories: ['.', '../'],
        implicit_include_directories: false,
        dependencies: [
            gtest_dep,
            gmock_dep,
            ramoops_manager,
            phosphor_dbus_interfaces_dep,
            phosphor_logging_dep,
            sdbusplus_dep,
            sdeventplus_dep,
        ],
    ),
    workdir: meson.current_source_dir(),
)

# lookupCreateParam() and the BMC (Manager) OEM allowable values live in the
# NVIDIA dump extension; the allowable-values case only runs with vhmc-host.
if not get_option('openpower-dumps-extension').allowed()
    test(
        'oem_dump_utils_test',
        executable(
            'oem_dump_utils_test',
            'oem_dump_utils_test.cpp',
            '../dump-extensions/nvidia-dumps/oem_dump_utils.cpp',
            dump_types_hpp,
            include_directories: ['.', '../'],
            implicit_include_directories: false,
            dependencies: [
                gtest_dep,
                gmock_dep,
                phosphor_dbus_interfaces_dep,
                phosphor_logging_dep,
                sdbusplus_dep,
                sdeventplus_dep,
            ],
        ),
        workdir: meson.current_source_dir(),
    )
endif

# PCore selector handling is header-only and shared by the dump manager and
# cpu-diagnostic-dump, so it is tested without either of their dependencies.
# Built whenever tests are enabled, not only under -Dpcore-dump=enabled, so CI
# keeps compiling it on platforms that ship the feature disabled.
test(
    'pcore_dump_test',
    executable(
        'pcore_dump_test',
        'pcore_dump_test.cpp',
        include_directories: ['.', '../'],
        implicit_include_directories: false,
        dependencies: [gtest_dep, gmock_dep],
    ),
    workdir: meson.current_source_dir(),
)

# The hmc-dump-server pieces that need neither D-Bus nor the HMC mailbox.
test(
    'hmc_dump_server_test',
    executable(
        'hmc_dump_server_test',
        'hmc_dump_server_test.cpp',
        include_directories: ['.', '../'],
        implicit_include_directories: false,
        dependencies: [gtest_dep, gmock_dep],
    ),
    workdir: meson.current_source_dir(),
)

# The I2C mailbox tools' configuration file reader.
test(
    'conf_file_test',
    executable(
        'conf_file_test',
        'conf_file_test.cpp',
        include_directories: ['.', '../'],
        implicit_include_directories: false,
        dependencies: [gtest_dep, gmock_dep],
    ),
    workdir: meson.current_source_dir(),
)

# hmc-mailbox's configuration file and runtime detection.
test(
    'hmc_mailbox_config_test',
    executable(
        'hmc_mailbox_config_test',
        'hmc_mailbox_config_test.cpp',
        include_directories: ['.', '../'],
        implicit_include_directories: false,
        dependencies: [gtest_dep, gmock_dep, phosphor_logging_dep],
    ),
    workdir: meson.current_source_dir(),
)
