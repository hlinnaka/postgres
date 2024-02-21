# Copyright (c) 2022-2024, PostgreSQL Global Development Group

backend_sources += files(
  'barrier.c',
  'dsm.c',
  'dsm_impl.c',
  'dsm_registry.c',
  'ipc.c',
  'ipci.c',
  'latch.c',
  'pmsignal.c',
  'procarray.c',
  'procsignal.c',
  'shm_mq.c',
  'shm_toc.c',
  'shmem.c',
  'signalfuncs.c',
  'sinval.c',
  'sinvaladt.c',
  'standby.c',

)

if host_system == 'windows'
  backend_sources += files(
    'dsm_impl_windows.c',
  )
else
  backend_sources += files(
    'dsm_impl_posix.c',
    'dsm_impl_sysv.c',
    'dsm_impl_mmap.c',
  )
endif
