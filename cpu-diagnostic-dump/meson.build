incdir = include_directories('..')
sources = ['main.cpp']
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
    'cpu-diagnostic-dump',
    sources,
    dependencies: [sdbusplus_dep, fmt_dep, nlohmann_json_dep],
    install: true,
)

