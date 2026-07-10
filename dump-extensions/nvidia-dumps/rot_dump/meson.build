# ROT/EROT dump collector (C++). Invoked by dump manager for DiagnosticType EROT/ROT.
incdir = include_directories('..', '../..', meson.project_build_root())
sources = ['main.cpp']

executable(
    'rot_dump',
    sources,
    dependencies: [sdbusplus_dep],
    include_directories: incdir,
    install: true,
    install_dir: get_option('bindir'),
)
