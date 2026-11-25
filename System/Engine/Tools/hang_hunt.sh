#!/bin/sh
# hang_hunt.sh <tag> <runs> <secs> [ENV=VAL ...]: repeats the village-walk bench (multi-core 16, 120 fps target,
# SW_HANG_DUMP=5) until a run logs "=== HANG" or a crash; prints one line per run.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
tag=$1; runs=$2; secs=$3; shift 3
ROUTE="SAVE=user_outdoor_20261107_1502 SW_AUTO_A=20,2.5,55 SW_AUTO_LS=60,170,0,1 SW_AUTO_RS=60,170,0.3,0"
i=1
while [ $i -le $runs ]; do
  py "$ROOT/Engine/Tools/bench.py" "${tag}_$i" "$secs" $ROUTE SW_FPS=120 SW_MULTICORE=1 SW_CORE_PIN=0 SW_CORES=16 \
     SW_THREAD_STATS=1 SW_HANG_DUMP=5 "$@" | tail -1
  log="$ROOT/build_work/out/logs/bench_${tag}_$i.log"
  if grep -a -q "=== HANG\|CRASH HANDLER\|\[trap\]" "$log"; then echo "HANG/CRASH in run $i: $log"; exit 0; fi
  i=$((i + 1))
done
echo "no hang in $runs runs"
