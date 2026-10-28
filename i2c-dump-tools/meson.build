# SPDX-License-Identifier: Apache-2.0

executable(
    'hmc-dump-server',
    'server.cpp',
    dependencies: sdbusplus_dep,
    install: true,
    install_dir: get_option('bindir'),
)

unit_files += {
    'input': 'i2c-dump-tools/hmc-dump-server.service',
    'output': 'hmc-dump-server.service',
}
