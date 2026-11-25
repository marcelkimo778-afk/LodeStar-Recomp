#!/bin/sh
# A/B benchmark: bench_ab.sh <tag> <exeA> <exeB> <runs> [ENV=VAL ...]
# Alternates A and B on the village-walk route (save copy, scripted input), 180 s each, with a per-frame log
# per run in build_work/out/logs/ft_<tag>_<A|B><n>.txt; prints ft_summary.py over the walk window.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
tag=$1; A=$2; B=$3; runs=$4; shift 4
ROUTE="SAVE=user_outdoor_20261107_1502 SW_AUTO_A=20,2.5,55 SW_AUTO_LS=60,170,0,1 SW_AUTO_RS=60,170,0.3,0"
i=1
while [ $i -le $runs ]; do
  for side in A B; do
    if [ $side = A ]; then exe=$A; else exe=$B; fi
    py "$ROOT/Engine/Tools/bench.py" "${tag}_${side}$i" 180 EXE="$exe" $ROUTE \
       SW_FRAMETIME_LOG="logs/ft_${tag}_${side}$i.txt" "$@" | tail -1
  done
  i=$((i + 1))
done
py "$ROOT/Engine/Tools/ft_summary.py" "$ROOT"/build_work/out/logs/ft_${tag}_*.txt
