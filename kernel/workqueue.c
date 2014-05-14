/*
 * kernel/workqueue.c - generic async execution with shared worker pool
 *
 * Copyright (C) 2002		Ingo Molnar
 *
 *   Derived from the taskqueue/keventd code by:
 *     David Woodhouse <dwmw2@infradead.org>
 *     Andrew Morton
 *     Kai Petzke <wpp@marie.physik.tu-berlin.de>
 *     Theodore Ts'o <tytso@mit.edu>
 *
 * Made to use alloc_percpu by Christoph Lameter.
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * This is the generic async execution mechanism.  Work items as are
 * executed in process context.  The worker pool is shared and
 * automatically managed.  There is one worker pool for each CPU and
 * one extra for works which are better served by workers which are
 * not bound to any specific CPU.
 *
 * Please read Documentation/workqueue.txt for details.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/signal.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/kthread.h>
#include <linux/hardirq.h>
#include <linux/mempolicy.h>
#include <linux/freezer.h>
#include <linux/kallsyms.h>
#include <linux/debug_locks.h>
#include <linux/lockdep.h>
#include <linux/idr.h>
#include <linux/bug.h>

#include "workqueue_sched.h"
#ifdef CONFIG_SEC_DEBUG
#include <mach/sec_debug.h>
#endif

enum {
	/*
	 * global_cwq flags
	 *
	 * A bound gcwq is either associated or disassociated with its CPU.
	 * While associated (!DISASSOCIATED), all workers are bound to the
	 * CPU and none has %WORKER_UNBOUND set and concurrency management
	 * is in effect.
	 *
	 * While DISASSOCIATED, the cpu may be offline and all workers have
	 * %WORKER_UNBOUND set and concurrency management disabled, and may
	 * be executing on any CPU.  The gcwq behaves as an unbound one.
	 *
	 * Note that DISASSOCIATED can be flipped only while holding
	 * managership of all pools on the gcwq to avoid changing binding
	 * state while create_worker() is in progress.
	 */
	GCWQ_DISASSOCIATED	= 1 << 0,	/* cpu can't serve workers */
	GCWQ_FREEZING		= 1 << 1,	/* freeze in progress */

	/* pool flags */
	POOL_MANAGE_WORKERS	= 1 << 0,	/* need to manage workers */

	/* worker flags */
	WORKER_STARTED		= 1 << 0,	/* started */
	WORKER_DIE		= 1 << 1,	/* die die die */
	WORKER_IDLE		= 1 << 2,	/* is idle */
	WORKER_PREP		= 1 << 3,	/* preparing to run works */
	WORKER_CPU_INTENSIVE	= 1 << 6,	/* cpu intensive */
	WORKER_UNBOUND		= 1 << 7,	/* worker is unbound */

	WORKER_NOT_RUNNING	= WORKER_PREP | WORKER_UNBOUND |
				  WORKER_CPU_INTENSIVE,

	NR_WORKER_POOLS		= 2,		/* # worker pools per gcwq */

	BUSY_WORKER_HASH_ORDER	= 6,		/* 64 pointers */
	BUSY_WORKER_HASH_SIZE	= 1 << BUSY_WORKER_HASH_ORDER,
	BUSY_WORKER_HASH_MASK	= BUSY_WORKER_HASH_SIZE - 1,

	MAX_IDLE_WORKERS_RATIO	= 4,		/* 1/4 of busy can be idle */
	IDLE_WORKER_TIMEOUT	= 300 * HZ,	/* keep idle ones for 5 mins */

	MAYDAY_INITIAL_TIMEOUT  = HZ / 100 >= 2 ? HZ / 100 : 2,
						/* call for help after 10ms
						   (min two ticks) */
	MAYDAY_INTERVAL		= HZ / 10,	/* and then every 100ms */
	CREATE_COOLDOWN		= HZ,		/* time to breath after fail */

	/*
	 * Rescue workers are used only on emergencies and shared by
	 * all cpus.  Give -20.
	 */
	RESCUER_NICE_LEVEL	= -20,
	HIGHPRI_NICE_LEVEL	= -20,
};

/*
 * Structure fields follow one of the following exclusion rules.
 *
 * I: Modifiable by initialization/destruction paths and read-only for
 *    everyone else.
 *
 * P: Preemption protected.  Disabling preemption is enough and should
 *    only be modified and accessed from the local cpu.
 *
 * L: gcwq->lock protected.  Access with gcwq->lock held.
 *
 * X: During normal operation, modification requires gcwq->lock and
 *    should be done only from local cpu.  Either disabling preemption
 *    on local cpu or grabbing gcwq->lock is enough for read access.
 *    If GCWQ_DISASSOCIATED is set, it's identical to L.
 *
 * F: wq->flush_mutex protected.
 *
 * W: workqueue_lock protected.
 */

struct global_cwq;
struct worker_pool;

/*
 * The poor guys doing the actual heavy lifting.  All on-duty workers
 * are either serving the manager role, on idle list or on busy hash.
 */
struct worker {
	/* on idle list while idle, on busy hash table while busy */
	union {
		struct list_head	entry;	/* L: while idle */
		struct hlist_node	hentry;	/* L: while busy */
	};

	struct work_struct	*current_work;	/* L: work being processed */
	struct cpu_workqueue_struct *current_cwq; /* L: current_work's cwq */
	struct list_head	scheduled;	/* L: scheduled works */
	struct task_struct	*task;		/* I: worker task */
	struct worker_pool	*pool;		/* I: the associated pool */
	/* 64 bytes boundary on 64bit, 32 on 32bit */
	unsigned long		last_active;	/* L: last active timestamp */
	unsigned int		flags;		/* X: flags */
	int			id;		/* I: worker id */

	/* for rebinding worker to CPU */
	struct work_struct	rebind_work;	/* L: for busy worker */
};

struct worker_pool {
	struct global_cwq	*gcwq;		/* I: the owning gcwq */
	unsigned int		flags;		/* X: flags */

	struct list_head	worklist;	/* L: list of pending works */
	int			nr_workers;	/* L: total number of workers */
	
	/* nr_idle includes the ones off idle_list for rebinding */
	int			nr_idle;	/* L: currently idle ones */

	struct list_head	idle_list;	/* X: list of idle workers */
	struct timer_list	idle_timer;	/* L: worker idle timeout */
	struct timer_list	mayday_timer;	/* L: SOS timer for workers */

	struct mutex		manager_mutex;	/* mutex manager should hold */
	struct ida		worker_ida;	/* L: for worker IDs */
};

/*
 * Global per-cpu workqueue.  There's one and only one for each cpu
 * and all works are queued and processed here regardless of their
 * target workqueues.
 */
struct global_cwq {
	spinlock_t		lock;		/* the gcwq lock */
	unsigned int		cpu;		/* I: the associated cpu */
	unsigned int		flags;		/* L: GCWQ_* flags */

	/* workers are chained either in busy_hash or pool idle_list */
	struct hlist_head	busy_hash[BUSY_WORKER_HASH_SIZE];
						/* L: hash of busy workers */

    struct worker_pool	pools[NR_WORKER_POOLS];
						/* normal and highpri pools */
} ____cacheline_aligned_in_smp;

/*
 * The per-CPU workqueue.  The lower WORK_STRUCT_FLAG_BITS of
 * work_struct->data are used for flags and thus cwqs need to be
 * aligned at two's power of the number of flag bits.
 */
struct cpu_workqueue_struct {
	struct worker_pool	*pool;		/* I: the associated pool */
	struct workqueue_struct *wq;		/* I: the owning workqueue */
	int			work_color;	/* L: current color */
	int			flush_color;	/* L: flushing color */
	int			nr_in_flight[WORK_NR_COLORS];
						/* L: nr of in_flight works */
	int			nr_active;	/* L: nr of active works */
	int			max_active;	/* L: max active works */
	struct list_head	delayed_works;	/* L: delayed works */
};

/*
 * Structure used to wait for workqueue flush.
 */
struct wq_flusher {
	struct list_head	list;		/* F: list of flushers */
	int			flush_color;	/* F: flush color waiting for */
	struct completion	done;		/* flush completion */
};

/*
 * All cpumasks are assumed to be always set on UP and thus can't be
 * used to determine whether there's something to be done.
 */
#ifdef CONFIG_SMP
typedef cpumask_var_t mayday_mask_t;
#define mayday_test_and_set_cpu(cpu, mask)	\
	cpumask_test_and_set_cpu((cpu), (mask))
#define mayday_clear_cpu(cpu, mask)		cpumask_clear_cpu((cpu), (mask))
#define for_each_mayday_cpu(cpu, mask)		for_each_cpu((cpu), (mask))
#define alloc_mayday_mask(maskp, gfp)		zalloc_cpumask_var((maskp), (gfp))
#define free_mayday_mask(mask)			free_cpumask_var((mask))
#else
typedef unsigned long mayday_mask_t;
#define mayday_test_and_set_cpu(cpu, mask)	test_and_set_bit(0, &(mask))
#define mayday_clear_cpu(cpu, mask)		clear_bit(0, &(mask))
#define for_each_mayday_cpu(cpu, mask)		if ((cpu) = 0, (mask))
#define alloc_mayday_mask(maskp, gfp)		true
#define free_mayday_mask(mask)			do { } while (0)
#endif

/*
 * The externally visible workqueue abstraction is an array of
 * per-CPU workqueues:
 */
struct workqueue_struct {
	unsigned int		flags;		/* W: WQ_* flags */
	union {
		struct cpu_workqueue_struct __percpu	*pcpu;
		struct cpu_workqueue_struct		*single;
		unsigned long				v;
	} cpu_wq;				/* I: cwq's */
	struct list_head	list;		/* W: list of all workqueues */

	struct mutex		flush_mutex;	/* protects wq flushing */
	int			work_color;	/* F: current work color */
	int			flush_color;	/* F: current flush color */
	atomic_t		nr_cwqs_to_flush; /* flush in progress */
	struct wq_flusher	*first_flusher;	/* F: first flusher */
	struct list_head	flusher_queue;	/* F: flush waiters */
	struct list_head	flusher_overflow; /* F: flush overflow list */

	mayday_mask_t		mayday_mask;	/* cpus requesting rescue */
	struct worker		*rescuer;	/* I: rescue worker */

	int			nr_drainers;	/* W: drain in progress */
	int			saved_max_active; /* W: saved cwq max_active */
#ifdef CONFIG_LOCKDEP
	struct lockdep_map	lockdep_map;
#endif
	char			name[];		/* I: workqueue name */
};

struct workqueue_struct *system_wq __read_mostly;
struct workqueue_struct *system_long_wq __read_mostly;
struct workqueue_struct *system_nrt_wq __read_mostly;
struct workqueue_struct *system_unbound_wq __read_mostly;
struct workqueue_struct *system_freezable_wq __read_mostly;
struct workqueue_struct *system_nrt_freezable_wq __read_mostly;
EXPORT_SYMBOL_GPL(system_wq);
EXPORT_SYMBOL_GPL(system_long_wq);
EXPORT_SYMBOL_GPL(system_nrt_wq);
EXPORT_SYMBOL_GPL(system_unbound_wq);
EXPORT_SYMBOL_GPL(system_freezable_wq);
EXPORT_SYMBOL_GPL(system_nrt_freezable_wq);

#define CREATE_TRACE_POINTS
#include <trace/events/workqueue.h>

#define for_each_worker_pool(pool, gcwq)				\
	for ((pool) = &(gcwq)->pools[0];				\
	     (pool) < &(gcwq)->pools[NR_WORKER_POOLS]; (pool)++)

#define for_each_busy_worker(worker, i, pos, gcwq)			\
	for (i = 0; i < BUSY_WORKER_HASH_SIZE; i++)			\
		hlist_for_each_entry(worker, pos, &gcwq->busy_hash[i], hentry)

static inline int __next_gcwq_cpu(int cpu, const struct cpumask *mask,
				  unsigned int sw)
{
	if (cpu < nr_cpu_ids) {
		if (sw & 1) {
			cpu = cpumask_next(cpu, mask);
			if (cpu < nr_cpu_ids)
				return cpu;
		}
		if (sw & 2)
			return WORK_CPU_UNBOUND;
	}
	return WORK_CPU_NONE;
}

static inline int __next_wq_cpu(int cpu, const struct cpumask *mask,
				struct workqueue_struct *wq)
{
	return __next_gcwq_cpu(cpu, mask, !(wq->flags & WQ_UNBOUND) ? 1 : 2);
}

/*
 * CPU iterators
 *
 * An extra gcwq is defined for an invalid cpu number
 * (WORK_CPU_UNBOUND) to host workqueues which are not bound to any
 * specific CPU.  The following iterators are similar to
 * for_each_*_cpu() iterators but also considers the unbound gcwq.
 *
 * for_each_gcwq_cpu()		: possible CPUs + WORK_CPU_UNBOUND
 * for_each_online_gcwq_cpu()	: online CPUs + WORK_CPU_UNBOUND
 * for_each_cwq_cpu()		: possible CPUs for bound workqueues,
 *				  WORK_CPU_UNBOUND for unbound workqueues
 */
#define for_each_gcwq_cpu(cpu)						\
	for ((cpu) = __next_gcwq_cpu(-1, cpu_possible_mask, 3);		\
	     (cpu) < WORK_CPU_NONE;					\
	     (cpu) = __next_gcwq_cpu((cpu), cpu_possible_mask, 3))

#define for_each_online_gcwq_cpu(cpu)					\
	for ((cpu) = __next_gcwq_cpu(-1, cpu_online_mask, 3);		\
	     (cpu) < WORK_CPU_NONE;					\
	     (cpu) = __next_gcwq_cpu((cpu), cpu_online_mask, 3))

#define for_each_cwq_cpu(cpu, wq)					\
	for ((cpu) = __next_wq_cpu(-1, cpu_possible_mask, (wq));	\
	     (cpu) < WORK_CPU_NONE;					\
	     (cpu) = __next_wq_cpu((cpu), cpu_possible_mask, (wq)))

#ifdef CONFIG_DEBUG_OBJECTS_WORK

static struct debug_obj_descr work_debug_descr;

static void *work_debug_hint(void *addr)
{
	return ((struct work_struct *) addr)->func;
}

/*
 * fixup_init is called when:
 * - an active object is initialized
 */
static int work_fixup_init(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		cancel_work_sync(work);
		debug_object_init(work, &work_debug_descr);
		return 1;
	default:
		return 0;
	}
}

/*
 * fixup_activate is called when:
 * - an active object is activated
 * - an unknown object is activated (might be a statically initialized object)
 */
static int work_fixup_activate(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {

	case ODEBUG_STATE_NOTAVAILABLE:
		/*
		 * This is not really a fixup. The work struct was
		 * statically initialized. We just make sure that it
		 * is tracked in the object tracker.
		 */
		if (test_bit(WORK_STRUCT_STATIC_BIT, work_data_bits(work))) {
			debug_object_init(work, &work_debug_descr);
			debug_object_activate(work, &work_debug_descr);
			return 0;
		}
		WARN_ON_ONCE(1);
		return 0;

	case ODEBUG_STATE_ACTIVE:
		WARN_ON(1);

	default:
		return 0;
	}
}

/*
 * fixup_free is called when:
 * - an active object is freed
 */
static int work_fixup_free(void *addr, enum debug_obj_state state)
{
	struct work_struct *work = addr;

	switch (state) {
	case ODEBUG_STATE_ACTIVE:
		cancel_work_sync(work);
		debug_object_free(work, &work_debug_descr);
		return 1;
	default:
		return 0;
	}
}

static struct debug_obj_descr work_debug_descr = {
	.name		= "work_struct",
	.debug_hint	= work_debug_hint,
	.fixup_init	= work_fixup_init,
	.fixup_activate	= work_fixup_activate,
	.fixup_free	= work_fixup_free,
};

static inline void debug_work_activate(struct work_struct *work)
{
	debug_object_activate(work, &work_debug_descr);
}

static inline void debug_work_deactivate(struct work_struct *work)
{
	debug_object_deactivate(work, &work_debug_descr);
}

void __init_work(struct work_struct *work, int onstack)
{
	if (onstack)
		debug_object_init_on_stack(work, &work_debug_descr);
	else
		debug_object_init(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(__init_work);

void destroy_work_on_stack(struct work_struct *work)
{
	debug_object_free(work, &work_debug_descr);
}
EXPORT_SYMBOL_GPL(destroy_work_on_stack);

#else
static inline void debug_work_activate(struct work_struct *work) { }
static inline void debug_work_deactivate(struct work_struct *work) { }
#endif

/* Serializes the accesses to the list of workqueues. */
static DEFINE_SPINLOCK(workqueue_lock);
static LIST_HEAD(workqueues);
static bool workqueue_freezing;		/* W: have wqs started freezing? */

/*
 * The almighty global cpu workqueues.  nr_running is the only field
 * which is expected to be used frequently by other cpus via
 * try_to_wake_up().  Put it in a separate cacheline.
 */
static DEFINE_PER_CPU(struct global_cwq, global_cwq);
static DEFINE_PER_CPU_SHARED_ALIGNED(atomic_t, pool_nr_running[NR_WORKER_POOLS]);

/*
 * Global cpu workqueue and nr_running counter for unbound gcwq.  The
 * gcwq is always online, has GCWQ_DISASSOCIATED set, and all its
 * workers have WORKER_UNBOUND set.
 */
static struct global_cwq unbound_global_cwq;
static atomic_t unbound_pool_nr_running[NR_WORKER_POOLS] = {
	[0 ... NR_WORKER_POOLS - 1]	= ATOMIC_INIT(0),	/* always 0 */
};

static int worker_thread(void *__worker);

static int worker_pool_pri(struct worker_pool *pool)
{
	return pool - pool->gcwq->pools;
}

static struct global_cwq *get_gcwq(unsigned int cpu)
{
	if (cpu != WORK_CPU_UNBOUND)
		return &per_cpu(global_cwq, cpu);
	else
		return &unbound_global_cwq;
}

static atomic_t *get_pool_nr_running(struct worker_pool *pool)
{
	int cpu = pool->gcwq->cpu;
	int idx = worker_pool_pri(pool);

	if (cpu != WORK_CPU_UNBOUND)
		return &per_cpu(pool_nr_running, cpu)[idx];
	else
		return &unbound_pool_nr_running[idx];
}

static struct cpu_workqueue_struct *get_cwq(unsigned int cpu,
					    struct workqueue_struct *wq)
{
	if (!(wq->flags & WQ_UNBOUND)) {
		if (likely(cpu < nr_cpu_ids))
			return per_cpu_ptr(wq->cpu_wq.pcpu, cpu);
	} else if (likely(cpu == WORK_CPU_UNBOUND))
		return wq->cpu_wq.single;
	return NULL;
}

static unsigned int work_color_to_flags(int color)
{
	return color << WORK_STRUCT_COLOR_SHIFT;
}

static int get_work_color(struct work_struct *work)
{
	return (*work_data_bits(work) >> WORK_STRUCT_COLOR_SHIFT) &
		((1 << WORK_STRUCT_COLOR_BITS) - 1);
}

static int work_next_color(int color)
{
	return (color + 1) % WORK_NR_COLORS;
}

/*
 * A work's data points to the cwq with WORK_STRUCT_CWQ set while the
 * work is on queue.  Once execution starts, WORK_STRUCT_CWQ is
 * cleared and the work data contains the cpu number it was last on.
 *
 * set_work_{cwq|cpu}() and clear_work_data() can be used to set the
 * cwq, cpu or clear work->data.  These functions should only be
 * called while the work is owned - ie. while the PENDING bit is set.
 *
 * get_work_[g]cwq() can be used to obtain the gcwq or cwq
 * corresponding to a work.  gcwq is available once the work has been
 * queued anywhere after initialization.  cwq is available only from
 * queueing until execution starts.
 */
static inline void set_work_data(struct work_struct *work, unsigned long data,
				 unsigned long flags)
{
	BUG_ON(!work_pending(work));
	atomic_long_set(&work->data, data | flags | work_static(work));
}

static void set_work_cwq(struct work_struct *work,
			 struct cpu_workqueue_struct *cwq,
			 unsigned long extra_flags)
{
	set_work_data(work, (unsigned long)cwq,
		      WORK_STRUCT_PENDING | WORK_STRUCT_CWQ | extra_flags);
}

static void set_work_cpu(struct work_struct *work, unsigned int cpu)
{
	set_work_data(work, cpu << WORK_STRUCT_FLAG_BITS, WORK_STRUCT_PENDING);
}

static void clear_work_data(struct work_struct *work)
{
	set_work_data(work, WORK_STRUCT_NO_CPU, 0);
}

static struct cpu_workqueue_struct *get_work_cwq(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);

	if (data & WORK_STRUCT_CWQ)
		return (void *)(data & WORK_STRUCT_WQ_DATA_MASK);
	else
		return NULL;
}

static struct global_cwq *get_work_gcwq(struct work_struct *work)
{
	unsigned long data = atomic_long_read(&work->data);
	unsigned int cpu;

	if (data & WORK_STRUCT_CWQ)
		return ((struct cpu_workqueue_struct *)
			(data & WORK_STRUCT_WQ_DATA_MASK))->pool->gcwq;

	cpu = data >> WORK_STRUCT_FLAG_BITS;
	if (cpu == WORK_CPU_NONE)
		return NULL;

	BUG_ON(cpu >= nr_cpu_ids && cpu != WORK_CPU_UNBOUND);
	return get_gcwq(cpu);
}

/*
 * Policy functions.  These define the policies on how the global worker
 * pools are managed.  Unless noted otherwise, these functions assume that
 * they're being called with gcwq->lock held.
 */

static bool __need_more_worker(struct worker_pool *pool)
{
	return !atomic_read(get_pool_nr_running(pool));
}

/*
 * Need to wake up a worker?  Called from anything but currently
 * running workers.
 *
 * Note that, because unbound workers never contribute to nr_running, this
 * function will always return %true for unbound gcwq as long as the
 * worklist isn't empty.
 */
static bool need_more_worker(struct worker_pool *pool)
{
	return !list_empty(&pool->worklist) && __need_more_worker(pool);
}

/* Can I start working?  Called from busy but !running workers. */
static bool may_start_working(struct worker_pool *pool)
{
	return pool->nr_idle;
}

/* Do I need to keep working?  Called from currently running workers. */
static bool keep_working(struct worker_pool *pool)
{
	atomic_t *nr_running = get_pool_nr_running(pool);

	return !list_empty(&pool->worklist) && atomic_read(nr_running) <= 1;
}

/* Do we need a new worker?  Called from manager. */
static bool need_to_create_worker(struct worker_pool *pool)
{
	return need_more_worker(pool) && !may_start_working(pool);
}

/* Do I need to be the manager? */
static bool need_to_manage_workers(struct worker_pool *pool)
{
	return need_to_create_worker(pool) ||
		(pool->flags & POOL_MANAGE_WORKERS);
}

/* Do we have too many workers and should some go away? */
static bool too_many_workers(struct worker_pool *pool)
{
	bool managing = mutex_is_locked(&pool->manager_mutex);
	int nr_idle = pool->nr_idle + managing; /* manager is considered idle */
	int nr_busy = pool->nr_workers - nr_idle;

/*
	 * nr_idle and idle_list may disagree if idle rebinding is in
	 * progress.  Never return %true if idle_list is empty.
	 */
	if (list_empty(&pool->idle_list))
		return false;

	return nr_idle > 2 && (nr_idle - 2) * MAX_IDLE_WORKERS_RATIO >= nr_busy;
}

/*
 * Wake up functions.
 */

/* Return the first worker.  Safe with preemption disabled */
static struct worker *first_worker(struct worker_pool *pool)
{
	if (unlikely(list_empty(&pool->idle_list)))
		return NULL;

	return list_first_entry(&pool->idle_list, struct worker, entry);
}

/**
 * wake_up_worker - wake up an idle worker
 * @pool: worker pool to wake worker from
 *
 * Wake up the first idle worker of @pool.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock).
 */
static void wake_up_worker(struct worker_pool *pool)
{
	struct worker *worker = first_worker(pool);

	if (likely(worker))
		wake_up_process(worker->task);
}

/**
 * wq_worker_waking_up - a worker is waking up
 * @task: task waking up
 * @cpu: CPU @task is waking up to
 *
 * This function is called during try_to_wake_up() when a worker is
 * being awoken.
 *
 * CONTEXT:
 * spin_lock_irq(rq->lock)
 */
void wq_worker_waking_up(struct task_struct *task, unsigned int cpu)
{
	struct worker *worker = kthread_data(task);

	if (!(worker->flags & WORKER_NOT_RUNNING))
		atomic_inc(get_pool_nr_running(worker->pool));
}

/**
 * wq_worker_sleeping - a worker is going to sleep
 * @task: task going to sleep
 * @cpu: CPU in question, must be the current CPU number
 *
 * This function is called during schedule() when a busy worker is
 * going to sleep.  Worker on the same cpu can be woken up by
 * returning pointer to its task.
 *
 * CONTEXT:
 * spin_lock_irq(rq->lock)
 *
 * RETURNS:
 * Worker task on @cpu to wake up, %NULL if none.
 */
struct task_struct *wq_worker_sleeping(struct task_struct *task,
				       unsigned int cpu)
{
	struct worker *worker = kthread_data(task), *to_wakeup = NULL;
	struct worker_pool *pool = worker->pool;
	atomic_t *nr_running = get_pool_nr_running(pool);

	if (worker->flags & WORKER_NOT_RUNNING)
		return NULL;

	/* this can only happen on the local cpu */
	BUG_ON(cpu != raw_smp_processor_id());

	/*
	 * The counterpart of the following dec_and_test, implied mb,
	 * worklist not empty test sequence is in insert_work().
	 * Please read comment there.
	 *
	 * NOT_RUNNING is clear.  This means that we're bound to and
	 * running on the local cpu w/ rq lock held and preemption
	 * disabled, which in turn means that none else could be
	 * manipulating idle_list, so dereferencing idle_list without gcwq
	 * lock is safe.
	 */
	if (atomic_dec_and_test(nr_running) && !list_empty(&pool->worklist))
		to_wakeup = first_worker(pool);
	return to_wakeup ? to_wakeup->task : NULL;
}

/**
 * worker_set_flags - set worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to set
 * @wakeup: wakeup an idle worker if necessary
 *
 * Set @flags in @worker->flags and adjust nr_running accordingly.  If
 * nr_running becomes zero and @wakeup is %true, an idle worker is
 * woken up.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock)
 */
static inline void worker_set_flags(struct worker *worker, unsigned int flags,
				    bool wakeup)
{
	struct worker_pool *pool = worker->pool;

	WARN_ON_ONCE(worker->task != current);

	/*
	 * If transitioning into NOT_RUNNING, adjust nr_running and
	 * wake up an idle worker as necessary if requested by
	 * @wakeup.
	 */
	if ((flags & WORKER_NOT_RUNNING) &&
	    !(worker->flags & WORKER_NOT_RUNNING)) {
		atomic_t *nr_running = get_pool_nr_running(pool);

		if (wakeup) {
			if (atomic_dec_and_test(nr_running) &&
			    !list_empty(&pool->worklist))
				wake_up_worker(pool);
		} else
			atomic_dec(nr_running);
	}

	worker->flags |= flags;
}

/**
 * worker_clr_flags - clear worker flags and adjust nr_running accordingly
 * @worker: self
 * @flags: flags to clear
 *
 * Clear @flags in @worker->flags and adjust nr_running accordingly.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock)
 */
static inline void worker_clr_flags(struct worker *worker, unsigned int flags)
{
	struct worker_pool *pool = worker->pool;
	unsigned int oflags = worker->flags;

	WARN_ON_ONCE(worker->task != current);

	worker->flags &= ~flags;

	/*
	 * If transitioning out of NOT_RUNNING, increment nr_running.  Note
	 * that the nested NOT_RUNNING is not a noop.  NOT_RUNNING is mask
	 * of multiple flags, not a single flag.
	 */
	if ((flags & WORKER_NOT_RUNNING) && (oflags & WORKER_NOT_RUNNING))
		if (!(worker->flags & WORKER_NOT_RUNNING))
			atomic_inc(get_pool_nr_running(pool));
}

/**
 * busy_worker_head - return the busy hash head for a work
 * @gcwq: gcwq of interest
 * @work: work to be hashed
 *
 * Return hash head of @gcwq for @work.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock).
 *
 * RETURNS:
 * Pointer to the hash head.
 */
static struct hlist_head *busy_worker_head(struct global_cwq *gcwq,
					   struct work_struct *work)
{
	const int base_shift = ilog2(sizeof(struct work_struct));
	unsigned long v = (unsigned long)work;

	/* simple shift and fold hash, do we need something better? */
	v >>= base_shift;
	v += v >> BUSY_WORKER_HASH_ORDER;
	v &= BUSY_WORKER_HASH_MASK;

	return &gcwq->busy_hash[v];
}

/**
 * __find_worker_executing_work - find worker which is executing a work
 * @gcwq: gcwq of interest
 * @bwh: hash head as returned by busy_worker_head()
 * @work: work to find worker for
 *
 * Find a worker which is executing @work on @gcwq.  @bwh should be
 * the hash head obtained by calling busy_worker_head() with the same
 * work.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock).
 *
 * RETURNS:
 * Pointer to worker which is executing @work if found, NULL
 * otherwise.
 */
static struct worker *__find_worker_executing_work(struct global_cwq *gcwq,
						   struct hlist_head *bwh,
						   struct work_struct *work)
{
	struct worker *worker;
	struct hlist_node *tmp;

	hlist_for_each_entry(worker, tmp, bwh, hentry)
		if (worker->current_work == work)
			return worker;
	return NULL;
}

/**
 * find_worker_executing_work - find worker which is executing a work
 * @gcwq: gcwq of interest
 * @work: work to find worker for
 *
 * Find a worker which is executing @work on @gcwq.  This function is
 * identical to __find_worker_executing_work() except that this
 * function calculates @bwh itself.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock).
 *
 * RETURNS:
 * Pointer to worker which is executing @work if found, NULL
 * otherwise.
 */
static struct worker *find_worker_executing_work(struct global_cwq *gcwq,
						 struct work_struct *work)
{
	return __find_worker_executing_work(gcwq, busy_worker_head(gcwq, work),
					    work);
}

/**
 * insert_work - insert a work into gcwq
 * @cwq: cwq @work belongs to
 * @work: work to insert
 * @head: insertion point
 * @extra_flags: extra WORK_STRUCT_* flags to set
 *
 * Insert @work which belongs to @cwq into @gcwq after @head.
 * @extra_flags is or'd to work_struct flags.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock).
 */
static void insert_work(struct cpu_workqueue_struct *cwq,
			struct work_struct *work, struct list_head *head,
			unsigned int extra_flags)
{
	struct worker_pool *pool = cwq->pool;

	/* we own @work, set data and link */
	set_work_cwq(work, cwq, extra_flags);

	/*
	 * Ensure that we get the right work->data if we see the
	 * result of list_add() below, see try_to_grab_pending().
	 */
	smp_wmb();

	list_add_tail(&work->entry, head);

	/*
	 * Ensure either worker_sched_deactivated() sees the above
	 * list_add_tail() or we see zero nr_running to avoid workers
	 * lying around lazily while there are works to be processed.
	 */
	smp_mb();

	if (__need_more_worker(pool))
		wake_up_worker(pool);
}

/*
 * Test whether @work is being queued from another work executing on the
 * same workqueue.  This is rather expensive and should only be used from
 * cold paths.
 */
static bool is_chained_work(struct workqueue_struct *wq)
{
	unsigned long flags;
	unsigned int cpu;

	for_each_gcwq_cpu(cpu) {
		struct global_cwq *gcwq = get_gcwq(cpu);
		struct worker *worker;
		struct hlist_node *pos;
		int i;

		spin_lock_irqsave(&gcwq->lock, flags);
		for_each_busy_worker(worker, i, pos, gcwq) {
			if (worker->task != current)
				continue;
			spin_unlock_irqrestore(&gcwq->lock, flags);
			/*
			 * I'm @worker, no locking necessary.  See if @work
			 * is headed to the same workqueue.
			 */
			return worker->current_cwq->wq == wq;
		}
		spin_unlock_irqrestore(&gcwq->lock, flags);
	}
	return false;
}

static void __queue_work(unsigned int cpu, struct workqueue_struct *wq,
			 struct work_struct *work)
{
	struct global_cwq *gcwq;
	struct cpu_workqueue_struct *cwq;
	struct list_head *worklist;
	unsigned int work_flags;
	unsigned long flags;

	debug_work_activate(work);

	/* if dying, only works from the same workqueue are allowed */
	if (unlikely(wq->flags & WQ_DRAINING) &&
	    WARN_ON_ONCE(!is_chained_work(wq)))
		return;

	/* determine gcwq to use */
	if (!(wq->flags & WQ_UNBOUND)) {
		struct global_cwq *last_gcwq;

		if (unlikely(cpu == WORK_CPU_UNBOUND))
			cpu = raw_smp_processor_id();

		/*
		 * It's multi cpu.  If @wq is non-reentrant and @work
		 * was previously on a different cpu, it might still
		 * be running there, in which case the work needs to
		 * be queued on that cpu to guarantee non-reentrance.
		 */
		gcwq = get_gcwq(cpu);
		if (wq->flags & WQ_NON_REENTRANT &&
		    (last_gcwq = get_work_gcwq(work)) && last_gcwq != gcwq) {
			struct worker *worker;

			spin_lock_irqsave(&last_gcwq->lock, flags);

			worker = find_worker_executing_work(last_gcwq, work);

			if (worker && worker->current_cwq->wq == wq)
				gcwq = last_gcwq;
			else {
				/* meh... not running there, queue here */
				spin_unlock_irqrestore(&last_gcwq->lock, flags);
				spin_lock_irqsave(&gcwq->lock, flags);
			}
		} else
			spin_lock_irqsave(&gcwq->lock, flags);
	} else {
		gcwq = get_gcwq(WORK_CPU_UNBOUND);
		spin_lock_irqsave(&gcwq->lock, flags);
	}

	/* gcwq determined, get cwq and queue */
	cwq = get_cwq(gcwq->cpu, wq);
	trace_workqueue_queue_work(cpu, cwq, work);

	BUG_ON(!list_empty(&work->entry));

	cwq->nr_in_flight[cwq->work_color]++;
	work_flags = work_color_to_flags(cwq->work_color);

	if (likely(cwq->nr_active < cwq->max_active)) {
		trace_workqueue_activate_work(work);
		cwq->nr_active++;
		worklist = &cwq->pool->worklist;
	} else {
		work_flags |= WORK_STRUCT_DELAYED;
		worklist = &cwq->delayed_works;
	}

	insert_work(cwq, work, worklist, work_flags);

	spin_unlock_irqrestore(&gcwq->lock, flags);
}

/**
 * queue_work - queue work on a workqueue
 * @wq: workqueue to use
 * @work: work to queue
 *
 * Returns 0 if @work was already on a queue, non-zero otherwise.
 *
 * We queue the work to the CPU on which it was submitted, but if the CPU dies
 * it can be processed by another CPU.
 */
int queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	int ret;

	ret = queue_work_on(get_cpu(), wq, work);
	put_cpu();

	return ret;
}
EXPORT_SYMBOL_GPL(queue_work);

/**
 * queue_work_on - queue work on specific cpu
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @work: work to queue
 *
 * Returns 0 if @work was already on a queue, non-zero otherwise.
 *
 * We queue the work to a specific CPU, the caller must ensure it
 * can't go away.
 */
int
queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
	int ret = 0;

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		__queue_work(cpu, wq, work);
		ret = 1;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(queue_work_on);

static void delayed_work_timer_fn(unsigned long __data)
{
	struct delayed_work *dwork = (struct delayed_work *)__data;
	struct cpu_workqueue_struct *cwq = get_work_cwq(&dwork->work);

	__queue_work(smp_processor_id(), cwq->wq, &dwork->work);
}

/**
 * queue_delayed_work - queue work on a workqueue after delay
 * @wq: workqueue to use
 * @dwork: delayable work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * Returns 0 if @work was already on a queue, non-zero otherwise.
 */
int queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *dwork, unsigned long delay)
{
	if (delay == 0)
		return queue_work(wq, &dwork->work);

	return queue_delayed_work_on(-1, wq, dwork, delay);
}
EXPORT_SYMBOL_GPL(queue_delayed_work);

/**
 * queue_delayed_work_on - queue work on specific CPU after delay
 * @cpu: CPU number to execute work on
 * @wq: workqueue to use
 * @dwork: work to queue
 * @delay: number of jiffies to wait before queueing
 *
 * Returns 0 if @work was already on a queue, non-zero otherwise.
 */
int queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
			struct delayed_work *dwork, unsigned long delay)
{
	int ret = 0;
	struct timer_list *timer = &dwork->timer;
	struct work_struct *work = &dwork->work;

	if (!test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))) {
		unsigned int lcpu;

		BUG_ON(timer_pending(timer));
		BUG_ON(!list_empty(&work->entry));

		timer_stats_timer_set_start_info(&dwork->timer);

		/*
		 * This stores cwq for the moment, for the timer_fn.
		 * Note that the work's gcwq is preserved to allow
		 * reentrance detection for delayed works.
		 */
		if (!(wq->flags & WQ_UNBOUND)) {
			struct global_cwq *gcwq = get_work_gcwq(work);

			if (gcwq && gcwq->cpu != WORK_CPU_UNBOUND)
				lcpu = gcwq->cpu;
			else
				lcpu = raw_smp_processor_id();
		} else
			lcpu = WORK_CPU_UNBOUND;

		set_work_cwq(work, get_cwq(lcpu, wq), 0);

		timer->expires = jiffies + delay;
		timer->data = (unsigned long)dwork;
		timer->function = delayed_work_timer_fn;

		if (unlikely(cpu >= 0))
			add_timer_on(timer, cpu);
		else
			add_timer(timer);
		ret = 1;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(queue_delayed_work_on);

/**
 * worker_enter_idle - enter idle state
 * @worker: worker which is entering idle state
 *
 * @worker is entering idle state.  Update stats and idle timer if
 * necessary.
 *
 * LOCKING:
 * spin_lock_irq(gcwq->lock).
 */
static void worker_enter_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;
	struct global_cwq *gcwq = pool->gcwq;

	BUG_ON(worker->flags & WORKER_IDLE);
	BUG_ON(!list_empty(&worker->entry) &&
	       (worker->hentry.next || worker->hentry.pprev));

	/* can't use worker_set_flags(), also called from start_worker() */
	worker->flags |= WORKER_IDLE;
	pool->nr_idle++;
	worker->last_active = jiffies;

	/* idle_list is LIFO */
	list_add(&worker->entry, &pool->idle_list);

	if (too_many_workers(pool) && !timer_pending(&pool->idle_timer))
		mod_timer(&pool->idle_timer, jiffies + IDLE_WORKER_TIMEOUT);

	/*
	 * Sanity check nr_running.  Because gcwq_unbind_fn() releases
	 * gcwq->lock between setting %WORKER_UNBOUND and zapping
	 * nr_running, the warning may trigger spuriously.  Check iff
	 * unbind is not in progress.
	 */
	WARN_ON_ONCE(!(gcwq->flags & GCWQ_DISASSOCIATED) &&
		     pool->nr_workers == pool->nr_idle &&
		     atomic_read(get_pool_nr_running(pool)));
}

/**
 * worker_leave_idle - leave idle state
 * @worker: worker which is leaving idle state
 *
 * @worker is leaving idle state.  Update stats.
 *
 * LOCKING:
 * spin_lock_irq(gcwq->lock).
 */
static void worker_leave_idle(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;

	BUG_ON(!(worker->flags & WORKER_IDLE));
	worker_clr_flags(worker, WORKER_IDLE);
	pool->nr_idle--;
	list_del_init(&worker->entry);
}

/**
 * worker_maybe_bind_and_lock - bind worker to its cpu if possible and lock gcwq
 * @worker: self
 *
 * Works which are scheduled while the cpu is online must at least be
 * scheduled to a worker which is bound to the cpu so that if they are
 * flushed from cpu callbacks while cpu is going down, they are
 * guaranteed to execute on the cpu.
 *
 * This function is to be used by rogue workers and rescuers to bind
 * themselves to the target cpu and may race with cpu going down or
 * coming online.  kthread_bind() can't be used because it may put the
 * worker to already dead cpu and set_cpus_allowed_ptr() can't be used
 * verbatim as it's best effort and blocking and gcwq may be
 * [dis]associated in the meantime.
 *
 * This function tries set_cpus_allowed() and locks gcwq and verifies the
 * binding against %GCWQ_DISASSOCIATED which is set during
 * %CPU_DOWN_PREPARE and cleared during %CPU_ONLINE, so if the worker
 * enters idle state or fetches works without dropping lock, it can
 * guarantee the scheduling requirement described in the first paragraph.
 *
 * CONTEXT:
 * Might sleep.  Called without any lock but returns with gcwq->lock
 * held.
 *
 * RETURNS:
 * %true if the associated gcwq is online (@worker is successfully
 * bound), %false if offline.
 */
static bool worker_maybe_bind_and_lock(struct worker *worker)
__acquires(&gcwq->lock)
{
	struct global_cwq *gcwq = worker->pool->gcwq;
	struct task_struct *task = worker->task;

	while (true) {
		/*
		 * The following call may fail, succeed or succeed
		 * without actually migrating the task to the cpu if
		 * it races with cpu hotunplug operation.  Verify
		 * against GCWQ_DISASSOCIATED.
		 */
		if (!(gcwq->flags & GCWQ_DISASSOCIATED))
			set_cpus_allowed_ptr(task, get_cpu_mask(gcwq->cpu));

		spin_lock_irq(&gcwq->lock);
		if (gcwq->flags & GCWQ_DISASSOCIATED)
			return false;
		if (task_cpu(task) == gcwq->cpu &&
		    cpumask_equal(&current->cpus_allowed,
				  get_cpu_mask(gcwq->cpu)))
			return true;
		spin_unlock_irq(&gcwq->lock);

		/*
		 * We've raced with CPU hot[un]plug.  Give it a breather
		 * and retry migration.  cond_resched() is required here;
		 * otherwise, we might deadlock against cpu_stop trying to
		 * bring down the CPU on non-preemptive kernel.
		 */
		cpu_relax();
		cond_resched();
	}
}

/*
 * Rebind an idle @worker to its CPU.  worker_thread() will test
 * list_empty(@worker->entry) before leaving idle and call this function.
 */
static void idle_worker_rebind(struct worker *worker)
{
	struct global_cwq *gcwq = worker->pool->gcwq;

	/* CPU may go down again inbetween, clear UNBOUND only on success */
	if (worker_maybe_bind_and_lock(worker))
		worker_clr_flags(worker, WORKER_UNBOUND);
	
	/* rebind complete, become available again */
	list_add(&worker->entry, &worker->pool->idle_list);
	spin_unlock_irq(&gcwq->lock);
}

/*
 * Function for @worker->rebind.work used to rebind unbound busy workers to
 * the associated cpu which is coming back online.  This is scheduled by
 * cpu up but can race with other cpu hotplug operations and may be
 * executed twice without intervening cpu down.
 */
static void busy_worker_rebind_fn(struct work_struct *work)
{
	struct worker *worker = container_of(work, struct worker, rebind_work);
	struct global_cwq *gcwq = worker->pool->gcwq;

	if (worker_maybe_bind_and_lock(worker))
		worker_clr_flags(worker, WORKER_REBIND);

	spin_unlock_irq(&gcwq->lock);
}

/**
 * rebind_workers - rebind all workers of a gcwq to the associated CPU
 * @gcwq: gcwq of interest
 *
 * @gcwq->cpu is coming online.  Rebind all workers to the CPU.  Rebinding
 * is different for idle and busy ones.
 *
 * Idle ones will be removed from the idle_list and woken up.  They will
 * add themselves back after completing rebind.  This ensures that the
 * idle_list doesn't contain any unbound workers when re-bound busy workers
 * try to perform local wake-ups for concurrency management.
 *
 * Busy workers can rebind after they finish their current work items.
 * Queueing the rebind work item at the head of the scheduled list is
 * enough.  Note that nr_running will be properly bumped as busy workers
 * rebind.
 *
 *On return, all non-manager workers are scheduled for rebind - see
 * manage_workers() for the manager special case.  Any idle worker
 * including the manager will not appear on @idle_list until rebind is
 * complete, making local wake-ups safe.
 */
static void rebind_workers(struct global_cwq *gcwq)
{
	struct worker_pool *pool;
	struct worker *worker, *n;
	struct hlist_node *pos;
	int i;

	lockdep_assert_held(&gcwq->lock);

	for_each_worker_pool(pool, gcwq)
		lockdep_assert_held(&pool->manager_mutex);

	/* dequeue and kick idle ones */
	for_each_worker_pool(pool, gcwq) {
		list_for_each_entry_safe(worker, n, &pool->idle_list, entry) {
			/*
			 * idle workers should be off @pool->idle_list
			 * until rebind is complete to avoid receiving
			 * premature local wake-ups.
			 */
			list_del_init(&worker->entry);
			
            /*
			 * worker_thread() will see the above dequeuing
			 * and call idle_worker_rebind().
			 */			wake_up_process(worker->task);
		}
	}
    
    /* rebind busy workers */
	for_each_worker_pool(pool, gcwq)
		list_for_each_entry(worker, &pool->idle_list, entry)
			worker->flags &= ~WORKER_REBIND;

	wake_up_all(&gcwq->rebind_hold);

	/* rebind busy workers */
	for_each_busy_worker(worker, i, pos, gcwq) {
		struct work_struct *rebind_work = &worker->rebind_work;

		/* morph UNBOUND to REBIND */
		worker->flags &= ~WORKER_UNBOUND;
		worker->flags |= WORKER_REBIND;

		if (test_and_set_bit(WORK_STRUCT_PENDING_BIT,
				     work_data_bits(rebind_work)))
			continue;

		/* wq doesn't matter, use the default one */
		debug_work_activate(rebind_work);
		insert_work(get_cwq(gcwq->cpu, system_wq), rebind_work,
			    worker->scheduled.next,
			    work_color_to_flags(WORK_NO_COLOR));
	}
}

static struct worker *alloc_worker(void)
{
	struct worker *worker;

	worker = kzalloc(sizeof(*worker), GFP_KERNEL);
	if (worker) {
		INIT_LIST_HEAD(&worker->entry);
		INIT_LIST_HEAD(&worker->scheduled);
		INIT_WORK(&worker->rebind_work, busy_worker_rebind_fn);
		/* on creation a worker is in !idle && prep state */
		worker->flags = WORKER_PREP;
	}
	return worker;
}

/**
 * create_worker - create a new workqueue worker
 * @pool: pool the new worker will belong to
 *
 * Create a new worker which is bound to @pool.  The returned worker
 * can be started by calling start_worker() or destroyed using
 * destroy_worker().
 *
 * CONTEXT:
 * Might sleep.  Does GFP_KERNEL allocations.
 *
 * RETURNS:
 * Pointer to the newly created worker.
 */
static struct worker *create_worker(struct worker_pool *pool)
{
	struct global_cwq *gcwq = pool->gcwq;
	const char *pri = worker_pool_pri(pool) ? "H" : "";
	struct worker *worker = NULL;
	int id = -1;

	spin_lock_irq(&gcwq->lock);
	while (ida_get_new(&pool->worker_ida, &id)) {
		spin_unlock_irq(&gcwq->lock);
		if (!ida_pre_get(&pool->worker_ida, GFP_KERNEL))
			goto fail;
		spin_lock_irq(&gcwq->lock);
	}
	spin_unlock_irq(&gcwq->lock);

	worker = alloc_worker();
	if (!worker)
		goto fail;

	worker->pool = pool;
	worker->id = id;

	if (gcwq->cpu != WORK_CPU_UNBOUND)
		worker->task = kthread_create_on_node(worker_thread,
					worker, cpu_to_node(gcwq->cpu),
					"kworker/%u:%d%s", gcwq->cpu, id, pri);
	else
		worker->task = kthread_create(worker_thread, worker,
					      "kworker/u:%d%s", id, pri);
	if (IS_ERR(worker->task))
		goto fail;

	if (worker_pool_pri(pool))
		set_user_nice(worker->task, HIGHPRI_NICE_LEVEL);

	/*
	 * Determine CPU binding of the new worker depending on
	 * %GCWQ_DISASSOCIATED.  The caller is responsible for ensuring the
	 * flag remains stable across this function.  See the comments
	 * above the flag definition for details.
	 *
	 * As an unbound worker may later become a regular one if CPU comes
	 * online, make sure every worker has %PF_THREAD_BOUND set.
	 */
	if (!(gcwq->flags & GCWQ_DISASSOCIATED)) {
		kthread_bind(worker->task, gcwq->cpu);
	} else {
		worker->task->flags |= PF_THREAD_BOUND;
		worker->flags |= WORKER_UNBOUND;
	}

	return worker;
fail:
	if (id >= 0) {
		spin_lock_irq(&gcwq->lock);
		ida_remove(&pool->worker_ida, id);
		spin_unlock_irq(&gcwq->lock);
	}
	kfree(worker);
	return NULL;
}

/**
 * start_worker - start a newly created worker
 * @worker: worker to start
 *
 * Make the gcwq aware of @worker and start it.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock).
 */
static void start_worker(struct worker *worker)
{
	worker->flags |= WORKER_STARTED;
	worker->pool->nr_workers++;
	worker_enter_idle(worker);
	wake_up_process(worker->task);
}

/**
 * destroy_worker - destroy a workqueue worker
 * @worker: worker to be destroyed
 *
 * Destroy @worker and adjust @gcwq stats accordingly.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock) which is released and regrabbed.
 */
static void destroy_worker(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;
	struct global_cwq *gcwq = pool->gcwq;
	int id = worker->id;

	/* sanity check frenzy */
	BUG_ON(worker->current_work);
	BUG_ON(!list_empty(&worker->scheduled));

	if (worker->flags & WORKER_STARTED)
		pool->nr_workers--;
	if (worker->flags & WORKER_IDLE)
		pool->nr_idle--;

	/*
	 * Once WORKER_DIE is set, the kworker may destroy itself at any
	 * point.  Pin to ensure the task stays until we're done with it.
	 */
	get_task_struct(worker->task);

	list_del_init(&worker->entry);
	worker->flags |= WORKER_DIE;

	spin_unlock_irq(&gcwq->lock);

	kthread_stop(worker->task);
	put_task_struct(worker->task);
	kfree(worker);

	spin_lock_irq(&gcwq->lock);
	ida_remove(&pool->worker_ida, id);
}

static void idle_worker_timeout(unsigned long __pool)
{
	struct worker_pool *pool = (void *)__pool;
	struct global_cwq *gcwq = pool->gcwq;

	spin_lock_irq(&gcwq->lock);

	if (too_many_workers(pool)) {
		struct worker *worker;
		unsigned long expires;

		/* idle_list is kept in LIFO order, check the last one */
		worker = list_entry(pool->idle_list.prev, struct worker, entry);
		expires = worker->last_active + IDLE_WORKER_TIMEOUT;

		if (time_before(jiffies, expires))
			mod_timer(&pool->idle_timer, expires);
		else {
			/* it's been idle for too long, wake up manager */
			pool->flags |= POOL_MANAGE_WORKERS;
			wake_up_worker(pool);
		}
	}

	spin_unlock_irq(&gcwq->lock);
}

static bool send_mayday(struct work_struct *work)
{
	struct cpu_workqueue_struct *cwq = get_work_cwq(work);
	struct workqueue_struct *wq = cwq->wq;
	unsigned int cpu;

	if (!(wq->flags & WQ_RESCUER))
		return false;

	/* mayday mayday mayday */
	cpu = cwq->pool->gcwq->cpu;
	/* WORK_CPU_UNBOUND can't be set in cpumask, use cpu 0 instead */
	if (cpu == WORK_CPU_UNBOUND)
		cpu = 0;
	if (!mayday_test_and_set_cpu(cpu, wq->mayday_mask))
		wake_up_process(wq->rescuer->task);
	return true;
}

static void gcwq_mayday_timeout(unsigned long __pool)
{
	struct worker_pool *pool = (void *)__pool;
	struct global_cwq *gcwq = pool->gcwq;
	struct work_struct *work;

	spin_lock_irq(&gcwq->lock);

	if (need_to_create_worker(pool)) {
		/*
		 * We've been trying to create a new worker but
		 * haven't been successful.  We might be hitting an
		 * allocation deadlock.  Send distress signals to
		 * rescuers.
		 */
		list_for_each_entry(work, &pool->worklist, entry)
			send_mayday(work);
	}

	spin_unlock_irq(&gcwq->lock);

	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INTERVAL);
}

/**
 * maybe_create_worker - create a new worker if necessary
 * @pool: pool to create a new worker for
 *
 * Create a new worker for @pool if necessary.  @pool is guaranteed to
 * have at least one idle worker on return from this function.  If
 * creating a new worker takes longer than MAYDAY_INTERVAL, mayday is
 * sent to all rescuers with works scheduled on @pool to resolve
 * possible allocation deadlock.
 *
 * On return, need_to_create_worker() is guaranteed to be false and
 * may_start_working() true.
 *
 * LOCKING:
 * spin_lock_irq(gcwq->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.  Called only from
 * manager.
 *
 * RETURNS:
 * false if no action was taken and gcwq->lock stayed locked, true
 * otherwise.
 */
static bool maybe_create_worker(struct worker_pool *pool)
__releases(&gcwq->lock)
__acquires(&gcwq->lock)
{
	struct global_cwq *gcwq = pool->gcwq;

	if (!need_to_create_worker(pool))
		return false;
restart:
	spin_unlock_irq(&gcwq->lock);

	/* if we don't make progress in MAYDAY_INITIAL_TIMEOUT, call for help */
	mod_timer(&pool->mayday_timer, jiffies + MAYDAY_INITIAL_TIMEOUT);

	while (true) {
		struct worker *worker;

		worker = create_worker(pool);
		if (worker) {
			del_timer_sync(&pool->mayday_timer);
			spin_lock_irq(&gcwq->lock);
			start_worker(worker);
			BUG_ON(need_to_create_worker(pool));
			return true;
		}

		if (!need_to_create_worker(pool))
			break;

		__set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(CREATE_COOLDOWN);

		if (!need_to_create_worker(pool))
			break;
	}

	del_timer_sync(&pool->mayday_timer);
	spin_lock_irq(&gcwq->lock);
	if (need_to_create_worker(pool))
		goto restart;
	return true;
}

/**
 * maybe_destroy_worker - destroy workers which have been idle for a while
 * @pool: pool to destroy workers for
 *
 * Destroy @pool workers which have been idle for longer than
 * IDLE_WORKER_TIMEOUT.
 *
 * LOCKING:
 * spin_lock_irq(gcwq->lock) which may be released and regrabbed
 * multiple times.  Called only from manager.
 *
 * RETURNS:
 * false if no action was taken and gcwq->lock stayed locked, true
 * otherwise.
 */
static bool maybe_destroy_workers(struct worker_pool *pool)
{
	bool ret = false;

	while (too_many_workers(pool)) {
		struct worker *worker;
		unsigned long expires;

		worker = list_entry(pool->idle_list.prev, struct worker, entry);
		expires = worker->last_active + IDLE_WORKER_TIMEOUT;

		if (time_before(jiffies, expires)) {
			mod_timer(&pool->idle_timer, expires);
			break;
		}

		destroy_worker(worker);
		ret = true;
	}

	return ret;
}

/**
 * manage_workers - manage worker pool
 * @worker: self
 *
 * Assume the manager role and manage gcwq worker pool @worker belongs
 * to.  At any given time, there can be only zero or one manager per
 * gcwq.  The exclusion is handled automatically by this function.
 *
 * The caller can safely start processing works on false return.  On
 * true return, it's guaranteed that need_to_create_worker() is false
 * and may_start_working() is true.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock) which may be released and regrabbed
 * multiple times.  Does GFP_KERNEL allocations.
 *
 * RETURNS:
 * false if no action was taken and gcwq->lock stayed locked, true if
 * some action was taken.
 */
static bool manage_workers(struct worker *worker)
{
	struct worker_pool *pool = worker->pool;
	bool ret = false;

	if (!mutex_trylock(&pool->manager_mutex))
		return ret;

	pool->flags &= ~POOL_MANAGE_WORKERS;

	/*
	 * Destroy and then create so that may_start_working() is true
	 * on return.
	 */
	ret |= maybe_destroy_workers(pool);
	ret |= maybe_create_worker(pool);

	mutex_unlock(&pool->manager_mutex);
	return ret;
}

/**
 * move_linked_works - move linked works to a list
 * @work: start of series of works to be scheduled
 * @head: target list to append @work to
 * @nextp: out paramter for nested worklist walking
 *
 * Schedule linked works starting from @work to @head.  Work series to
 * be scheduled starts at @work and includes any consecutive work with
 * WORK_STRUCT_LINKED set in its predecessor.
 *
 * If @nextp is not NULL, it's updated to point to the next work of
 * the last scheduled work.  This allows move_linked_works() to be
 * nested inside outer list_for_each_entry_safe().
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock).
 */
static void move_linked_works(struct work_struct *work, struct list_head *head,
			      struct work_struct **nextp)
{
	struct work_struct *n;

	/*
	 * Linked worklist will always end before the end of the list,
	 * use NULL for list head.
	 */
	list_for_each_entry_safe_from(work, n, NULL, entry) {
		list_move_tail(&work->entry, head);
		if (!(*work_data_bits(work) & WORK_STRUCT_LINKED))
			break;
	}

	/*
	 * If we're already inside safe list traversal and have moved
	 * multiple works to the scheduled queue, the next position
	 * needs to be updated.
	 */
	if (nextp)
		*nextp = n;
}

static void cwq_activate_first_delayed(struct cpu_workqueue_struct *cwq)
{
	struct work_struct *work = list_first_entry(&cwq->delayed_works,
						    struct work_struct, entry);

	trace_workqueue_activate_work(work);
	move_linked_works(work, &cwq->pool->worklist, NULL);
	__clear_bit(WORK_STRUCT_DELAYED_BIT, work_data_bits(work));
	cwq->nr_active++;
}

/**
 * cwq_dec_nr_in_flight - decrement cwq's nr_in_flight
 * @cwq: cwq of interest
 * @color: color of work which left the queue
 * @delayed: for a delayed work
 *
 * A work either has completed or is removed from pending queue,
 * decrement nr_in_flight of its cwq and handle workqueue flushing.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock).
 */
static void cwq_dec_nr_in_flight(struct cpu_workqueue_struct *cwq, int color,
				 bool delayed)
{
	/* ignore uncolored works */
	if (color == WORK_NO_COLOR)
		return;

	cwq->nr_in_flight[color]--;

	if (!delayed) {
		cwq->nr_active--;
		if (!list_empty(&cwq->delayed_works)) {
			/* one down, submit a delayed one */
			if (cwq->nr_active < cwq->max_active)
				cwq_activate_first_delayed(cwq);
		}
	}

	/* is flush in progress and are we at the flushing tip? */
	if (likely(cwq->flush_color != color))
		return;

	/* are there still in-flight works? */
	if (cwq->nr_in_flight[color])
		return;

	/* this cwq is done, clear flush_color */
	cwq->flush_color = -1;

	/*
	 * If this was the last cwq, wake up the first flusher.  It
	 * will handle the rest.
	 */
	if (atomic_dec_and_test(&cwq->wq->nr_cwqs_to_flush))
		complete(&cwq->wq->first_flusher->done);
}

/**
 * process_one_work - process single work
 * @worker: self
 * @work: work to process
 *
 * Process @work.  This function contains all the logics necessary to
 * process a single work including synchronization against and
 * interaction with other workers on the same cpu, queueing and
 * flushing.  As long as context requirement is met, any worker can
 * call this function to process a work.
 *
 * CONTEXT:
 * spin_lock_irq(gcwq->lock) which is released and regrabbed.
 */
static void process_one_work(struct worker *worker, struct work_struct *work)
__releases(&gcwq->lock)
__acquires(&gcwq->lock)
{
	struct cpu_workqueue_struct *cwq = get_work_cwq(work);
	struct worker_pool *pool = worker->pool;
	struct global_cwq *gcwq = pool->gcwq;
	struct hlist_head *bwh = busy_worker_head(gcwq, work);
	bool cpu_intensive = cwq->wq->flags & WQ_CPU_INTENSIVE;
	work_func_t f = work->func;
	int work_color;
	struct worker *collision;
#ifdef CONFIG_LOCKDEP
	/*
	 * It is permissible to free the struct work_struct from
	 * inside the function that is called from it, this we need to
	 * take into account for lockdep too.  To avoid bogus "held
	 * lock freed" warnings as well as pro
