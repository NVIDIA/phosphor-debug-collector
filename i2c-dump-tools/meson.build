# SPDX-License-Identifier: Apache-2.0

# The two ends of the BMC<->HMC I2C dump mailbox. The client finds its buses in
# the board's /usr/share/hmc-dump/hmc-dump.conf; the server finds the HMC's I2C
# target EEPROMs in sysfs. hmc-mailboxd (platform-init) starts the server unit.

executable(
    'hmc-dump-client',
    'client.cpp',
    dependencies: [sdbusplus_dep, phosphor_logging_dep],
    include_directories: include_directories('..'),
    install: true,
    install_dir: get_option('bindir'),
)

# The i2c-dump-util entry points, now wrappers around hmc-dump-client.
foreach script : ['i2c_dump_util.sh', 'execute_command.sh']
    configure_file(
        input: script + '.in',
        output: script,
        configuration: {
            'bindir': get_option('prefix') / get_option('bindir'),
        },
        install: true,
        install_dir: get_option('datadir') / 'i2c-dump-util',
        install_mode: 'rwxr-xr-x',
    )
endforeach

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
