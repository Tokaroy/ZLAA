#!/bin/bash
# thor_perf.sh <out prefix> <start at "frame N presented"> <seconds> [KEY=VALUE...]: thor_run, with simpleperf over that window
# (<prefix>.report.txt: all threads; .threads.txt: per thread; .main.txt: the busiest thread, the game's main one)
. "$(dirname "$0")/env.sh"
out=$1; at=$2; secs=$3; shift 3
T=$(cd "$(dirname "$0")" && pwd); S=${LAA_OUT:-$HOME/laar-build/thor-out}; mkdir -p $S
( sleep 15; for i in $(seq 600); do
    [ "$($W shell "grep -c 'frame $at presented' $A/log.txt 2>/dev/null" | tr -d "\r")" = "1" ] && break; sleep 0.2
  done
  $W shell "simpleperf record --app io.github.zlaa --call-graph fp -f 4000 --duration $secs -o /data/local/tmp/perf.data" > $out.rec.txt 2>&1
  $W shell "simpleperf report -i /data/local/tmp/perf.data --sort dso,symbol -n --percent-limit 0.3" > $out.report.txt 2>&1
  $W shell "simpleperf report -i /data/local/tmp/perf.data --sort tid,comm -n" > $out.threads.txt 2>&1
  main=$(awk '$3 ~ /^[0-9]+$/ {print $3; exit}' $out.threads.txt)   # the busiest thread: the game's main thread
  $W shell "simpleperf report -i /data/local/tmp/perf.data --tids $main --sort dso,symbol -n --percent-limit 0.2" > $out.main.txt 2>&1
) &
sleep 3
$T/thor_run.sh $out.log "$@"
wait
