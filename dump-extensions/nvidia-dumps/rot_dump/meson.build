# ROT/EROT dump collector (C++). Invoked by dump manager for DiagnosticType EROT/ROT.
incdir = include_directories('..', '../..')
sources = ['main.cpp']

executable(
    'rot_dump',
    sources,
    dependencies: [sdbusplus_dep],
    install: true,
    install_dir: get_option('bindir'),
)
