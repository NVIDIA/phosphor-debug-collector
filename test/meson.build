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
