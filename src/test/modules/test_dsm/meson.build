# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_dsm_sources = files(
  'test_dsm.c',
)

if host_system == 'windows'
  test_dsm_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_dsm',
    '--FILEDESC', 'test_dsm - test code for dynamic shared memory',])
endif

test_dsm = shared_module('test_dsm',
  test_dsm_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_dsm

test_install_data += files(
  'test_dsm.control',
  'test_dsm--1.0.sql',
)

tests += {
  'name': 'test_dsm',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_dsm_basic.pl',
    ],
  },
}
