# scx_piano_eco — design notes

Goal: on the 6+2 Snapdragon 8 Elite, keep light/interactive work on the
efficient cluster and only wake the prime cores when there is real work, so
idle/battery time improves without hurting desktop responsiveness.

## Why a custom policy

- The default EEVDF has no cluster-cost model; it balances by load only.
- `schedutil` reacts to utilization, but a task bouncing to a prime core
  raises frequency and power for the whole cluster.
- `scx_lavd` already does most of this (latency-criticality + CPU preference);
  start there, then specialise with `piano_eco` for 6+2 and thermal coupling.

## Data the scheduler needs

| Input | Source |
|---|---|
| CPU capacity | `/sys/devices/system/cpu/cpu*/cpu_capacity` (or DT) |
| Little set | CPU 0-5 |
| Prime set | CPU 6-7 |
| Task weight/nice | `p->scx.weight`, `p->static_prio` |
| Wake flags | `wake_flags` (`SCX_WAKE_*`) |
| Runtime slice | `p->scx.slice`, `scx_bpf_task_runtime()` |

## Policy

1. **Capacity-scaled virtual time.**
   `vtime = now + slice * (CAP_MAX / cap(cpu))`
   A task that ran on a little core accumulates virtual time faster, so the
   fair scheduler naturally migrates it to a prime core when it becomes heavy,
   and keeps short tasks (low runtime) on little cores.

2. **Wake-affine.**
   If `prev_cpu` is idle and allowed, enqueue to its local DSQ. Prevents
   cache-cold migrations on every wakeup (desktop latency).

3. **Idle CPU choice.**
   - light task (`nice >= 0` and runtime < 2 ms): pick an idle little CPU;
   - interactive (`nice <= -5` or `SCX_WAKE_SYNC`): prefer idle prime if the
     little cluster has other runnable work;
   - otherwise let `scx_bpf_select_cpu_dfl()` use the energy model.

4. **Slices.**
   - default 4 ms; scale to `min(8 ms, 4 ms * weight/100)` for background;
   - `SCX_ENQ_PREEMPT` on sync wakeups for input/UI threads.

5. **Frequency hint.**
   `scx_bpf_cpuperf_set(cpu, perf)` to cap prime CPUs at ~70 % unless a heavy
   task is queued; `piano-powerd` owns the hard thermal caps.

6. **Load balance.**
   Keep a per-CPU DSQ and a shared DSQ; `dispatch()` drains local first, then
   shared. Avoid moving tasks between clusters more than once per slice.

## Structure (BPF)

```
maps: cpu_cap[MAX_CPU], little_mask, dsq per CPU
ops:  select_cpu / enqueue / dispatch / running / stopping / init / exit
```

Pseudo-code for `enqueue`:

```c
cap = cpu_cap[cpu_of(p)];
vtime = scx_bpf_now() + slice_ns * (CAP_MAX / cap);
if (affine_ok(p, prev_cpu))
    scx_bpf_dsq_insert_vtime(p, prev_cpu, slice_ns, vtime, enq_flags);
else
    scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, slice_ns, vtime, enq_flags);
```

## Rollout

1. Kernel with `CONFIG_SCHED_CLASS_EXT=y` (GitHub Actions artifact).
2. `scx_lavd --autopilot` on battery, `--performance` on AC; measure idle power
   and UI latency with `scx_lavd --stats`.
3. Ship `piano_eco` behind a systemd unit; A/B on battery drain
   (`powertop`/`upower --dump`) and `perf sched` latency.
