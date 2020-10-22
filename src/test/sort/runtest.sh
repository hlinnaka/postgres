#!/bin/bash

PGDATA=/dev/shm/sorttest-pgdata

PREFIX=/home/hlinnakangas/pgsql-install

EMAIL=hlinnakangas@pivotal.io

set -e

function compile
{
    git checkout -f $1
    git checkout -f sort-balanced-merge2 src/test/sort
    ./configure --enable-debug --enable-depend --prefix=$PREFIX CC="ccache gcc"
    make -j4 -s clean
    make -j4 -s install

    PATH=$PREFIX/bin:$PATH make -C src/test/sort speed generate # build the test suite
}

function runtest
{
    # Kill everything
    pkill -9 postgres || true
    rm -rf $PGDATA

    $PREFIX/bin/initdb -D $PGDATA
    echo "max_parallel_workers_per_gather = 0" >> $PGDATA/postgresql.conf
    echo "shared_buffers = '1 GB' " >> $PGDATA/postgresql.conf

    $PREFIX/bin/pg_ctl -D $PGDATA start -w -l serverlog-$1

    LD_LIBRARY_PATH=$PREFIX/lib PATH=$PREFIX/bin:$PATH make -C src/test/sort generate_testdata

    git rev-parse --short HEAD > results-$1.txt
    
    LD_LIBRARY_PATH=$PREFIX/lib PGDATABASE=sorttest ./src/test/sort/speed >> results-$1.txt
    mail -s "pgperftest-vm: test $1 finished!" $EMAIL < results-$1.txt
}

compile master
runtest master

compile sort-balanced-merge2
runtest sort-balanced-merge2
