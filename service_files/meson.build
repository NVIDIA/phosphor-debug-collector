systemd = dependency('systemd')
systemd_system_unit_dir = systemd.get_variable(
    pkgconfig: 'systemd_system_unit_dir',
)

install_data(
    [
        'obmc-dump-monitor.service',
        'ramoops-monitor.service',
        'xyz.openbmc_project.Dump.Manager.service',
    ],
    install_dir: systemd_system_unit_dir,
)

kdump_unit_subs = configuration_data()
kdump_unit_subs.set('KDUMP_DUMP_PATH', get_option('KDUMP_DUMP_PATH'))

configure_file(
    input: 'kdump-monitor.service.in',
    output: 'kdump-monitor.service',
    configuration: kdump_unit_subs,
    install: true,
    install_dir: systemd_system_unit_dir,
)
