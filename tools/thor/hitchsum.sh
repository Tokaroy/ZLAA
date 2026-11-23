#!/bin/bash
# hitchsum.sh log...: a summary of LAA_HITCH_LOG=1 runs (thor_run.sh logs): frames over 20 ms and over 33 ms, draws
# skipped while their pipelines were made, and the main thread's waits for them; split into start-up (frames before
# 1500), the save loading (1500-1899) and play (1900 on), as thor_run's default script loads File 2 at ~1600.
# Pipelines made to warm up shaders (pipeline.c) and their CPU time below each.
for f in "$@"; do
  grep '^hitch:' "$f" | awk -v f="$(basename "$f" .txt)" '
    { fr = $3; ms = $4 + 0; sk = 0; w = 0
      for (i = 1; i <= NF; i++) { if ($i == "draws") sk = $(i - 1); if ($i == "waited") w = $(i + 1) + 0 }
      seg = fr < 1500 ? "boot" : fr < 1900 ? "load" : "play"
      if (ms > 20) { slow[seg]++; all++ }
      if (ms > 33.4) big[seg]++
      skip[seg] += sk; wait += w }
    END { printf "%-24s over 20 ms %3d (start %3d, load %3d, play %3d) | over 33 ms: load %3d, play %3d | skipped: load %5d, play %4d | waited %4.0f ms\n",
          f, all, slow["boot"], slow["load"], slow["play"], big["load"], big["play"], skip["load"], skip["play"], wait }'
  grep "warm up" "$f" | awk '{ n += $2; ms += $9 } END { if (n) printf "%24s %d warm-up pipelines, %.0f ms CPU\n", "", n, ms }'
done
