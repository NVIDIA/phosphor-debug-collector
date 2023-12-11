
# Configuration header file(fdr_dump_config.h) generation
flconf_data = configuration_data()

flconf_data.set_quoted('FDR_DUMP_BIN_PATH', get_option('FDR_DUMP_BIN_PATH'),
                      description : 'The path of FDR dump script'
                    )
flconf_data.set_quoted('FDR_DUMP_OBJPATH', get_option('FDR_DUMP_OBJPATH'),
                      description : 'The FDR dump manager D-Bus object path'
                    )
flconf_data.set_quoted('FDR_DUMP_OBJ_ENTRY', get_option('FDR_DUMP_OBJ_ENTRY'),
                      description : 'The FDR dump entry D-Bus object path'
                    )
flconf_data.set('FDR_DUMP_MAX_LIMIT', get_option('FDR_DUMP_MAX_LIMIT'),
               description : 'Total FDR dumps to be retained on bmc'
             )
flconf_data.set_quoted('FDR_DUMP_PATH', get_option('FDR_DUMP_PATH'),
                     description : 'Directory where FDR dumps are placed'
             )
flconf_data.set('FDR_DUMP_MAX_SIZE', get_option('FDR_DUMP_MAX_SIZE'),
               description : 'Maximum size of one FDR dump in kilo bytes'
             )
flconf_data.set('FDR_DUMP_MIN_SPACE_REQD', get_option('FDR_DUMP_MIN_SPACE_REQD'),
               description : 'Minimum space required for one FDR dump in kilo bytes'
             )
flconf_data.set('FDR_DUMP_TOTAL_SIZE', get_option('FDR_DUMP_TOTAL_SIZE'),
               description : 'Total size of the dump in kilo bytes'
             )

configure_file(configuration : flconf_data,
               output : 'fdr_dump_config.h'
              )

phosphor_dump_manager_sources += [
    'dump-extensions/fdr-dump/fdr-dump-extensions.cpp',
    'dump-extensions/fdr-dump/dump_manager_fdr.cpp',
    'dump-extensions/fdr-dump/fdr_dump_entry.cpp'
]
