# piano-kernel-scx

Builds the Xiaomi Pad 8 Pro (`piano`) mainline kernel from
[`yzddmr6/linux-sm8750-piano`](https://github.com/yzddmr6/linux-sm8750-piano)
(`piano/v0.5.1`) with **sched_ext enabled**, on GitHub's arm64 runners.

Output artifact `piano-kernel-sched-ext` contains:

- `Image`
- `sm8750-xiaomi-piano*.dtb`
- `modules/` (stripped, installed layout for `/usr/lib/modules`)
- `config`, `kernel-release`

## Scheduler design (target: battery + responsiveness)

`piano_eco` is a sched_ext policy tuned for the 6+2 Snapdragon cores:

1. **Capacity-aware vtime** — a task's virtual time advances faster on the
   little cluster (CPU 0-5) than on the prime cores (CPU 6-7), so heavy tasks
   naturally migrate up while light tasks stay efficient.
2. **Wake-affine enqueue** — keep a waking task on its previous CPU when that
   CPU is idle; avoids cache-cold migrations.
3. **Idle CPU selection** — pick the least-loaded idle CPU in the same LLC;
   prefer the little cluster unless the task is interactive (`nice <= 0`) or
   its util exceeds a threshold.
4. **Short slices with wakeup preemption** — 3-6 ms slices for desktop latency,
   batch (fair) slice scaling for background tasks.
5. **Frequency hints** — `scx_bpf_cpuperf_set()` to keep the prime cluster at a
   capped perf level unless a heavy task is present, complementing
   `schedutil` and the userspace `piano-powerd` thermal policy.

The userspace thermal/power policy lives in `piano-powerd` (Arch package),
which follows `power-profiles-daemon` (`power-saver` / `balanced` /
`performance`) and applies frequency caps plus thermal fallback.
