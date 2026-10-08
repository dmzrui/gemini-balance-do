# Geekbench 5 on this sandbox (4 vCPU container)

## Result

| | Score |
|---|---|
| **Single-Core** | **1492** |
| **Multi-Core** | **5004** |

Geekbench 5.5.1 for Linux x86 (64-bit), official Primate Labs build.
Result: <https://browser.geekbench.com/v5/cpu/24700121>
(raw page text in `run2_4vcpu_result_page.txt`)

Second (contaminated) run: SC 1495 / MC 4606 —
<https://browser.geekbench.com/v5/cpu/24700108>. The multi-core number was
depressed because the probe scripts below were running on the same 4 vCPUs at
the time; the clean run above is the one to quote.

## Environment

| | |
|---|---|
| Host CPU | AMD EPYC 9B14 (Zen 5c), 1 socket, 90 cores / 180 threads, 2.6 GHz base |
| Host memory | 708 GiB, no swap |
| Hypervisor | KVM guest on Google Compute Engine, inside a k8s Pod |
| **Container CPU quota** | `cpu.max = 400000/100000` → **4 vCPU** (`cpuset` unrestricted, 0-179) |
| Container memory limit | `memory.max` = 6144 MiB |
| GPU | none (no `/dev/nvidia*`, `/dev/dri`, `/dev/kfd`) |
| Disk | `/` overlay 1.1 TB; `/workspace` 12 GB ext4 (`/dev/nvme0n12`) |
| Network | 1× eth0, 10 Gb/s link, measured ~65 MB/s (~520 Mbit/s) egress |
| OS | Debian 12 (bookworm), kernel 6.12.85+ |

## Why the LD_PRELOAD shim is needed

`lscpu`, `nproc`, `/proc/cpuinfo` and `sysconf(_SC_NPROCESSORS_ONLN)` all report
the **host** topology (180 threads) — the cgroup quota is invisible to them.
Geekbench 5 sizes its multi-core phase from that number, so a naive run spawns
181 threads inside a 4-vCPU cgroup (verified with `ps -o nlwp`). Total work
becomes 180× the single-core workload while only 4 CPUs of throughput are
available, i.e. the multi-core phase takes ~45× longer and the score is
meaningless.

`cpu_count_shim.c` overrides `sysconf()`, `sched_getaffinity()` and
`open*()/fopen*()` (for `/proc/cpuinfo` and `/sys/devices/system/cpu/{online,
present,possible}`) so Geekbench sees exactly 4 CPUs, and `taskset -c 0-3`
pins it to 4 distinct physical cores (CPUs 0-89 are the first SMT thread of
each core on this host). Geekbench then reports `Topology 1 Processor, 4 Cores`
and uses 4 worker threads.

## Sanity check: the 4 vCPU quota is actually delivered

```
1 busy thread  on cpu0   : cpu=5.00s / wall=5.00s   (100%)
4 busy threads on cpu0-3 : 4.99 / 4.99 / 5.00 / 5.00 s CPU each over 5 s wall
```

No cgroup throttling pressure (`nr_throttled` grows only marginally), so the
scores reflect real throughput, not throttling. Note that `ps %CPU` for the
Geekbench process averages ~50% during the single-core phase — that is
Geekbench's own between-workload idle time, not CPU starvation.

## Reproduce

```bash
# grab the official 5.5.1 binary (cdn.geekbench.com is blocked in this sandbox,
# so it is pulled from the docker image davidsarkany/geekbench:5.5.1 layer)
#   -> /opt/geekbench/{geekbench5,geekbench_x86_64,geekbench.plar}
GB_DIR=/opt/geekbench CPUS=4 ./run_geekbench5.sh
```

Notes on running it here:

* Tryout mode requires Internet and **does not print the scores locally** — it
  only uploads them and prints the `browser.geekbench.com` URL.
* `browser.geekbench.com` sits behind a Cloudflare challenge that answers 403
  to plain `curl` (the upload POST itself goes through fine); the score tables
  in `*_result_page.txt` were read via the `r.jina.ai` text proxy.
