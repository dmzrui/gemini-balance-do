#!/usr/bin/env bash
# Run Geekbench 5 on a CPU-quota-limited container (e.g. cgroup cpu.max = 4 vCPU).
#
# Why the shim: Geekbench 5 sizes its multi-core phase from the number of
# logical CPUs it can see. On this host /proc/cpuinfo and
# sysconf(_SC_NPROCESSORS_ONLN) both report the *host* topology (180 threads),
# so GB5 spawns 180 worker threads inside a 4-vCPU cgroup => the multi-core
# phase takes ~45x longer than it should and the score is meaningless.
# cpu_count_shim.c overrides sysconf()/sched_getaffinity()/open() so GB5 sees
# exactly the CPUs the container is actually allowed to use.
#
# Prereqs: gcc, taskset (util-linux), a Geekbench 5.5.1 Linux install in $GB_DIR
# (geekbench5 + geekbench_x86_64 + geekbench.plar).
#
# Usage: GB_DIR=/opt/geekbench CPUS=4 ./run_geekbench5.sh
set -euo pipefail

GB_DIR="${GB_DIR:-/opt/geekbench}"
CPUS="${CPUS:-4}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/gb5_result.log}"

# cgroup quota -> number of CPUs we are actually allowed to burn
if [[ -r /sys/fs/cgroup/cpu.max ]]; then
  read -r quota period < /sys/fs/cgroup/cpu.max
  if [[ "$quota" != "max" ]]; then
    echo "cgroup cpu.max = $quota/$period => $((quota / period)) vCPU (using CPUS=$CPUS)"
  fi
fi

gcc -shared -fPIC -O2 -o "$HERE/cpu_count_shim.so" "$HERE/cpu_count_shim.c" -ldl

# pin to CPUS distinct physical cores (0,1,2,3 are the first SMT thread of
# cores 0-3 on this host, so 0-3 are 4 distinct cores)
cd "$GB_DIR"
LD_PRELOAD="$HERE/cpu_count_shim.so" taskset -c "0-$((CPUS - 1))" \
  ./geekbench5 --cpu 2>&1 | tee "$OUT"

echo
echo "== summary =="
grep -E 'Topology|Upload succeeded|browser.geekbench.com' "$OUT" || true
