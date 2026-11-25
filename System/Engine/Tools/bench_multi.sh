#!/bin/sh
# N-way benchmark: bench_multi.sh <tag> <runs> <exe1> [exe2 ...] [-- ENV=VAL ...]
# Like bench_ab.sh, but for any number of builds: each round runs every exe once (in order, so drift
# over time hits all builds alike) on the village-walk route (copy of a test save, scripted input),
# 180 s each, with a per-frame log per run in build_work/out/logs/ft_<tag>_<k><n>.txt (k = 1..N = exe
# position, n = round).  Ends with ft_summary.py over the walk window, per build.
# Example (launcher config on a 16-thread PC, 120 fps target):
#   sh Engine/Tools/bench_multi.sh cmp 3 build_work/out/sw_app_old.exe build_work/out/sw_app.exe \
#      -- SW_MULTICORE=1 SW_CORE_PIN=0 SW_CORES=16 SW_FPS=120
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
tag=$1; runs=$2; shift 2
exes=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do exes="$exes $1"; shift; done
[ "$1" = "--" ] && shift
ROUTE="SAVE=user_outdoor_20261107_1502 SW_AUTO_A=20,2.5,55 SW_AUTO_LS=60,170,0,1 SW_AUTO_RS=60,170,0.3,0"
i=1
while [ $i -le $runs ]; do
  k=1
  for exe in $exes; do
    py "$ROOT/Engine/Tools/bench.py" "${tag}_${k}_$i" 180 EXE="$exe" $ROUTE \
       SW_FRAMETIME_LOG="logs/ft_${tag}_${k}_$i.txt" "$@" | tail -1
    k=$((k + 1))
  done
  i=$((i + 1))
done
k=1
for exe in $exes; do
  echo "== build $k: $exe"
  py "$ROOT/Engine/Tools/ft_summary.py" "$ROOT"/build_work/out/logs/ft_${tag}_${k}_*.txt
  k=$((k + 1))
done
