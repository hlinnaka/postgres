# Copyright (c) 2022-2024, PostgreSQL Global Development Group

readbufferbench_sources = files(
  'readbufferbench.c',
)

if host_system == 'windows'
  readbufferbench_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'readbufferbench',
    '--FILEDESC', 'readbufferbench - micro-benchmarking code ReadBuffer and friends',])
endif

readbufferbench = shared_module('readbufferbench',
  readbufferbench_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += readbufferbench

test_install_data += files(
  'readbufferbench.control',
  'readbufferbench--1.0.sql',
)

tests += {
  'name': 'readbufferbench',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
}
