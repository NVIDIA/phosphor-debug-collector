incdir = include_directories('..')
sources = ['main.cpp', 'create_dump_dbus.cpp']
fmt_dep = dependency('fmt', required: false)
if not fmt_dep.found()
  fmt_proj = import('cmake').subproject(
    'fmt',
    cmake_options: [
      '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
      '-DMASTER_PROJECT=OFF'
    ],
    required: false)
  assert(fmt_proj.found(), 'fmtlib is required')
  fmt_dep = fmt_proj.dependency('fmt')
endif

executable('create-dump-dbus', sources, dependencies: [sdbusplus_dep, fmt_dep], install: true)
