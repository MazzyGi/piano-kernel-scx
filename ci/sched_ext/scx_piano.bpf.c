/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Piano: power-aware sched_ext scheduler.
 *
 * Modes (cfg[CFG_MODE]):
 *   0 = power   : keep tasks on their previous CPU and never hunt for idle
 *                 CPUs, packing work so other cores can stay idle.
 *   1 = balanced: default wakeup placement, modest interactive boost.
 *   2 = performance: same placement, larger wakeup boost.
 *
 * cfg[CFG_BOOST] is a vtime bonus (ns) applied to wakeups.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

#define SHARED_DSQ	0
#define CFG_MODE	0
#define CFG_BOOST	1

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 2);
} cfg SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 3);		/* [local, global, boost] */
} stats SEC(".maps");

static u64 vtime_now;
UEI_DEFINE(uei);

static u64 cfg_val(u32 idx)
{
	u64 *v = bpf_map_lookup_elem(&cfg, &idx);
	return v ? *v : 0;
}

static void stat_inc(u32 idx)
{
	u64 *cnt = bpf_map_lookup_elem(&stats, &idx);

	if (cnt)
		(*cnt)++;
}

s32 BPF_STRUCT_OPS(piano_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	u64 mode = cfg_val(CFG_MODE);
	bool is_idle = false;
	s32 cpu;

	if (mode == 0) {
		stat_inc(0);
		return prev_cpu;
	}

	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle) {
		stat_inc(0);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
	}
	return cpu;
}

void BPF_STRUCT_OPS(piano_enqueue, struct task_struct *p, u64 enq_flags)
{
	u64 vtime = p->scx.dsq_vtime;
	u64 boost = cfg_val(CFG_BOOST);
	u64 floor;

	stat_inc(1);

	if ((enq_flags & SCX_ENQ_WAKEUP) && boost) {
		floor = vtime_now - SCX_SLICE_DFL;
		if (time_before(floor, vtime)) {
			if (vtime - floor > boost)
				vtime -= boost;
			else
				vtime = floor;
			stat_inc(2);
		}
	}

	if (time_before(vtime, vtime_now - SCX_SLICE_DFL))
		vtime = vtime_now - SCX_SLICE_DFL;

	scx_bpf_dsq_insert_vtime(p, SHARED_DSQ, SCX_SLICE_DFL, vtime,
				 enq_flags);
}

void BPF_STRUCT_OPS(piano_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_dsq_move_to_local(SHARED_DSQ, 0);
}

void BPF_STRUCT_OPS(piano_running, struct task_struct *p)
{
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
}

void BPF_STRUCT_OPS(piano_stopping, struct task_struct *p, bool runnable)
{
	u64 delta = scale_by_task_weight_inverse(p, SCX_SLICE_DFL - p->scx.slice);

	scx_bpf_task_set_dsq_vtime(p, p->scx.dsq_vtime + delta);
}

void BPF_STRUCT_OPS(piano_enable, struct task_struct *p)
{
	scx_bpf_task_set_dsq_vtime(p, vtime_now);
}

s32 BPF_STRUCT_OPS_SLEEPABLE(piano_init)
{
	int ret;

	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret) {
		scx_bpf_error("failed to create DSQ %d (%d)", SHARED_DSQ, ret);
		return ret;
	}
	return 0;
}

void BPF_STRUCT_OPS(piano_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(piano_ops,
	       .select_cpu	= (void *)piano_select_cpu,
	       .enqueue		= (void *)piano_enqueue,
	       .dispatch	= (void *)piano_dispatch,
	       .running		= (void *)piano_running,
	       .stopping	= (void *)piano_stopping,
	       .enable		= (void *)piano_enable,
	       .init		= (void *)piano_init,
	       .exit		= (void *)piano_exit,
	       .name		= "piano");
