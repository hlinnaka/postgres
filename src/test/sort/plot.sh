#!/bin/bash

set -e

PATH=~/pgsql.fsmfork/bin:$PATH

plot_test() {
  local testname=$1;
    
  psql sorttest <<EOF
\copy (select work_mem, avg(time_ms), min(time_ms), max(time_ms) from public.results where testname='$testname' group by work_mem order by 1) to 'data/results-patched.dat'
\copy (select work_mem, avg(time_ms), min(time_ms), max(time_ms) from public.results_unpatched where testname='$testname' group by work_mem order by 1) to 'data/results-unpatched.dat'
EOF

  gnuplot - <<EOF
set terminal png
set output '$1.png'
set title "$testname (1 GB)" noenhanced
set xlabel "work_mem (kB)" noenhanced
set ylabel "time (ms)"
set logscale x
set yrange [32:]
set yrange [0:]
plot "data/results-patched.dat" using 1:2 with lines title "balanced k-way merge", \
     "data/results-unpatched.dat" using 1:2 with lines title "unpatched (commit e7c2b95d37)"
pause mouse close
EOF


}


plot_test medium.ordered_ints
plot_test medium.random_ints
plot_test medium.ordered_text
plot_test medium.random_text
