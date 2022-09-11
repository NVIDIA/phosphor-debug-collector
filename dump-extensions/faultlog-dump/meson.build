
# Configuration header file(faultlog_dump_config.h) generation
flconf_data = configuration_data()

flconf_data.set_quoted('CPER_DUMP_BIN_PATH', get_option('CPER_DUMP_BIN_PATH'),
                      description : 'The path of cper dump script'
                    )
flconf_data.set_quoted('FAULTLOG_DUMP_OBJPATH', get_option('FAULTLOG_DUMP_OBJPATH'),
                      description : 'The fault log dump manager D-Bus object path'
                    )
flconf_data.set_quoted('FAULTLOG_DUMP_OBJ_ENTRY', get_option('FAULTLOG_DUMP_OBJ_ENTRY'),
                      description : 'The fault log dump entry D-Bus object path'
                    )
flconf_data.set('FAULTLOG_DUMP_MAX_LIMIT', get_option('FAULTLOG_DUMP_MAX_LIMIT'),
               description : 'Total fault log dumps to be retained on bmc'
             )
flconf_data.set_quoted('FAULTLOG_DUMP_PATH', get_option('FAULTLOG_DUMP_PATH'),
                     description : 'Directory where fault log dumps are placed'
             )
flconf_data.set('FAULTLOG_DUMP_MAX_SIZE', get_option('FAULTLOG_DUMP_MAX_SIZE'),
               description : 'Maximum size of one fault log dump in kilo bytes'
             )
flconf_data.set('FAULTLOG_DUMP_MIN_SPACE_REQD', get_option('FAULTLOG_DUMP_MIN_SPACE_REQD'),
               description : 'Minimum space required for one fault log dump in kilo bytes'
             )
flconf_data.set('FAULTLOG_DUMP_TOTAL_SIZE', get_option('FAULTLOG_DUMP_TOTAL_SIZE'),
               description : 'Total size of the dump in kilo bytes'
             )
flconf_data.set('FAULTLOG_DUMP_ROTATION', get_option('faultlog-dump-rotation').enabled(),
               description : 'Rotate dump when total size of dumps exceed the quota'
             )

configure_file(configuration : flconf_data,
               output : 'faultlog_dump_config.h'
              )

phosphor_dump_manager_sources += [
    'dump-extensions/faultlog-dump/faultlog-dump-extensions.cpp',
    'dump-extensions/faultlog-dump/dump_manager_faultlog.cpp',
    'dump-extensions/faultlog-dump/faultlog_dump_entry.cpp'
]
