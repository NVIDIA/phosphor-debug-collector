# SPDX-License-Identifier: Apache-2.0

# The BMC<->HMC I2C mailbox. The dump client finds its buses in the board's
# /usr/share/hmc-dump/hmc-dump.conf; the dump server finds the HMC's I2C target
# EEPROMs in sysfs. hmc-mailbox publishes the HMC's status to the BMC and
# carries out the BMC's commands on the HMC, among them starting and stopping
# the dump server unit.

if i2c_dump_tools.allowed()
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
endif

# 'auto' also leaves hmc-mailbox out where libgpiod, libubootenv or zlib is
# missing.
libgpiodcxx_dep = dependency('libgpiodcxx', required: hmc_mailbox)
libubootenv_dep = dependency('libubootenv', required: hmc_mailbox)
zlib_dep = dependency('zlib', required: hmc_mailbox)
if hmc_mailbox.allowed() and libgpiodcxx_dep.found() and libubootenv_dep.found() and zlib_dep.found()
    executable(
        'hmc-mailbox',
        'hmc_mailbox.cpp',
        dependencies: [
            sdbusplus_dep,
            phosphor_logging_dep,
            libgpiodcxx_dep,
            libubootenv_dep,
            zlib_dep,
        ],
        include_directories: include_directories('..'),
        install: true,
        install_dir: get_option('bindir'),
    )

    unit_files += {
        'input': 'i2c-dump-tools/hmc-mailbox.service',
        'output': 'hmc-mailbox.service',
    }
endif
