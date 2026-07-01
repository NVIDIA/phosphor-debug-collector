incdir = include_directories('..')
sources = ['main.cpp', 'nsm_dump_utils.cpp']
fmt_dep = dependency('fmt', required: false)
if not fmt_dep.found()
    fmt_proj = import('cmake').subproject(
        'fmt',
        cmake_options: [
            '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
            '-DMASTER_PROJECT=OFF',
        ],
        required: false,
    )
    assert(fmt_proj.found(), 'fmtlib is required')
    fmt_dep = fmt_proj.dependency('fmt')
endif

executable(
    'nsm-dump-tool',
    sources,
    dependencies: [sdbusplus_dep, phosphor_logging_dep, fmt_dep],
    # Only install when the feature is explicitly enabled. When built solely
    # because tests are enabled (CI compile coverage), skip installation.
    install: get_option('nsm-dump-tool').allowed(),
)
