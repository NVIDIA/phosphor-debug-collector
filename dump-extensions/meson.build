# SPDX-License-Identifier: Apache-2.0

if get_option('openpower-dumps-extension').allowed()
    subdir('openpower-dumps')
else
    subdir('nvidia-dumps')
endif

if get_option('faultlog-dump-extension').enabled()
    subdir('faultlog-dump')
endif

if get_option('fdr-dump-extension').enabled()
    subdir('fdr-dump')
endif
