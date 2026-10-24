// File:	thread-worker.c

// List all group member's name: Yug Shah
// username of iLab: ys991
// iLab Server:

#include "thread-worker.h"

//Global counter for total context switches and 
//average turn around and response time
long tot_cntx_switches=0;
double avg_turn_time=0;
double avg_resp_time=0;


// INITAILIZE ALL YOUR OTHER VARIABLES HERE

static int         lib_init   = 0;     /* has the library been set up yet? */
static ucontext_t  sched_ctx;          /* the scheduler runs on this context */
static char       *sched_stack = NULL; /* and on its own stack */
static tcb        *current    = NULL;  /* the thread on the CPU right now */
static tcb        *main_tcb   = NULL;  /* TCB for the benchmark's main thread */
static tcb        *all_threads = NULL; /* every live TCB, linked by all_next */
static worker_t    next_tid   = 1;     /* id 0 is reserved for main */

/* Runqueues.  PSJF only ever uses runq[0]; MLFQ uses all NUM_LEVELS of them,
 * with runq[0] the highest priority. */
static tcb_queue   runq[NUM_LEVELS];

/* Threads that called worker_exit().  Their stacks cannot be freed by the
 * exiting thread itself (it is still standing on one), so the scheduler
 * reclaims them on its next pass. */
static tcb_queue   finished_q;

static sigset_t    prof_set;           /* just { SIGPROF } */
static int         timer_armed  = 0;
static long        dispatch_us  = 0;   /* when `current` was given the CPU */
static long        last_boost_us = 0;  /* MLFQ rule 5 bookkeeping */

/* Accumulators behind avg_turn_time / avg_resp_time.  Only worker threads
 * created through worker_create() are counted; the main thread is not a
 * worker and would otherwise skew both averages. */
static long        completed  = 0;
static double      total_turn = 0;
static double      total_resp = 0;

static void schedule();
#ifndef MLFQ
static void sched_psjf();
#else
static void sched_mlfq();
#endif


/* ----------------------------------------------------------------------- *
 * Small helpers                                                           *
 * ----------------------------------------------------------------------- */

/* Monotonic clocks.  clock_gettime() is async-signal-safe, so both are also
 * legal to call from the timer handler.
 *
 * Scheduling runs on the microsecond clock -- quanta are whole milliseconds,
 * so there is nothing finer to resolve.  The reported metrics use the
 * nanosecond clock instead: a thread can easily be created and finish inside
 * one microsecond tick, and on the coarser clock its turnaround would truncate
 * to exactly 0. */
static long now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static long now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

/* The runqueues, the TCB lists and the mutex wait lists are shared between
 * ordinary library code and the SIGPROF handler, so every library entry point
 * blocks SIGPROF while it touches them.  That is the user-level equivalent of
 * disabling interrupts around a kernel critical section. */
static void disable_preempt(void) {
	sigprocmask(SIG_BLOCK, &prof_set, NULL);
}

static void enable_preempt(void) {
	sigprocmask(SIG_UNBLOCK, &prof_set, NULL);
}

/* prof_set has to be valid before the first library call, because mutexes are
 * initialized before the first worker_create(). */
__attribute__((constructor))
static void worker_lib_ctor(void) {
	sigemptyset(&prof_set);
	sigaddset(&prof_set, SIGPROF);
}


/* ----------------------------------------------------------------------- *
 * Queue operations (shared by the runqueues and the mutex wait lists)     *
 * ----------------------------------------------------------------------- */

static void enqueue(tcb_queue *q, tcb *t) {
	t->qnext = NULL;
	if (q->tail)
		q->tail->qnext = t;
	else
		q->head = t;
	q->tail = t;
	q->count++;
}

static tcb *dequeue(tcb_queue *q) {
	tcb *t = q->head;
	if (!t)
		return NULL;
	q->head = t->qnext;
	if (!q->head)
		q->tail = NULL;
	q->count--;
	t->qnext = NULL;
	return t;
}

/* Remove a specific thread from the middle of a queue.  PSJF needs this
 * because it picks the minimum rather than the head. */
static void queue_remove(tcb_queue *q, tcb *t) {
	tcb *prev = NULL;
	tcb *cur  = q->head;

	while (cur && cur != t) {
		prev = cur;
		cur  = cur->qnext;
	}
	if (!cur)
		return;

	if (prev)
		prev->qnext = cur->qnext;
	else
		q->head = cur->qnext;
	if (q->tail == cur)
		q->tail = prev;
	q->count--;
	cur->qnext = NULL;
}

/* How many threads are sitting on a runqueue waiting for the CPU. */
static int runnable_count(void) {
	int i, n = 0;
	for (i = 0; i < NUM_LEVELS; i++)
		n += runq[i].count;
	return n;
}

static tcb *find_tcb(worker_t id) {
	tcb *t;
	for (t = all_threads; t; t = t->all_next)
		if (t->id == id)
			return t;
	return NULL;
}

static void remove_from_all(tcb *target) {
	tcb *prev = NULL;
	tcb *t    = all_threads;

	while (t && t != target) {
		prev = t;
		t    = t->all_next;
	}
	if (!t)
		return;
	if (prev)
		prev->all_next = t->all_next;
	else
		all_threads = t->all_next;
	t->all_next = NULL;
}


/* ----------------------------------------------------------------------- *
 * Timer                                                                   *
 * ----------------------------------------------------------------------- *
 * One-shot ITIMER_PROF timers, re-armed on every dispatch.  A one-shot timer
 * is used instead of a periodic one so that each MLFQ level can have its own
 * time slice, and so that a thread which gives up the CPU early is not
 * immediately interrupted by the leftover of someone else's quantum.        */

static void arm_timer(long usec) {
	struct itimerval it;

	it.it_interval.tv_sec  = 0;
	it.it_interval.tv_usec = 0;
	it.it_value.tv_sec     = usec / 1000000L;
	it.it_value.tv_usec    = usec % 1000000L;

	timer_armed = 1;
	setitimer(ITIMER_PROF, &it, NULL);
}

static void disarm_timer(void) {
	struct itimerval it;

	memset(&it, 0, sizeof it);
	timer_armed = 0;
	setitimer(ITIMER_PROF, &it, NULL);
}

/* Time slice this thread gets.  Under MLFQ the higher the priority, the
 * shorter the slice (OSTEP section 8.5). */
static long slice_for(tcb *t) {
#ifndef MLFQ
	(void) t;
	return (long) QUANTUM * 1000L;
#else
	return (long) QUANTUM * 1000L * (t->priority + 1);
#endif
}

/* Put a thread on the runqueue it belongs to and mark it READY. */
static void make_runnable(tcb *t) {
	t->status = READY;

#ifdef MLFQ
	if (t->priority < 0)
		t->priority = 0;
	if (t->priority > NUM_LEVELS - 1)
		t->priority = NUM_LEVELS - 1;
	enqueue(&runq[t->priority], t);
#else
	enqueue(&runq[0], t);
#endif

	/* The timer is only armed while somebody is actually waiting for the CPU
	 * (see the dispatch path in schedule()).  Waking a thread up from a mutex
	 * or from worker_exit() creates such a waiter, so if the running thread is
	 * currently un-preemptible it has to be put back on the clock -- otherwise
	 * a thread that never yields would starve the one it just woke. */
	if (!timer_armed && current && current->status == SCHEDULED)
		arm_timer(slice_for(current));
}

#ifdef MLFQ
/* Charge `ran` microseconds of CPU time to a thread's allotment at its current
 * level, and demote it once that allotment is used up (MLFQ rules 2-4).
 *
 * Charging elapsed time rather than counting timer interrupts is what makes a
 * thread unable to game the scheduler: giving the CPU up early preserves the
 * rest of its allotment, but it does not refund it, so a thread that yields
 * just before every slice expires still sinks at the same rate as one that
 * runs flat out.  Only the periodic boost (rule 5) refunds an allotment. */
static void mlfq_charge(tcb *t, long ran) {
	long slice = slice_for(t);

	t->allot_us += ran;
	if (t->allot_us < slice)
		return;

	if (t->priority < NUM_LEVELS - 1) {
		t->priority++;
		t->allot_us = 0;
	} else {
		t->allot_us = slice;      /* already at the bottom; stay there */
	}
}
#endif

/* Reclaim the stacks of threads that have exited.  Safe here and only here,
 * because the scheduler runs on its own stack.  The TCB itself is kept alive
 * so that worker_join() can still read the return value. */
static void reap_finished(void) {
	tcb *t;
	while ((t = dequeue(&finished_q)) != NULL) {
		free(t->stack);
		t->stack = NULL;
	}
}


/* ----------------------------------------------------------------------- *
 * Preemption                                                              *
 * ----------------------------------------------------------------------- */

/* SIGPROF handler: the quantum expired, so hand the CPU to the scheduler.
 *
 * swapcontext() saves and restores the signal mask along with the registers,
 * which is what makes switching out of a signal handler work here:
 *   - sched_ctx was captured with SIGPROF blocked, so the scheduler itself can
 *     never be interrupted by the quantum timer;
 *   - the context saved into current->ctx has SIGPROF blocked (the kernel
 *     blocks it for the duration of the handler), so when this thread is
 *     resumed it comes back here, returns from the handler, and the normal
 *     signal return path unblocks SIGPROF again. */
static void timer_handler(int signum) {
	(void) signum;

	if (!current || current->status != SCHEDULED)
		return;

	timer_armed     = 0;      /* one-shot: it has just expired */
	current->status = READY;
	swapcontext(&current->ctx, &sched_ctx);
}


/* ----------------------------------------------------------------------- *
 * One-time library setup                                                  *
 * ----------------------------------------------------------------------- *
 * Called from the first worker_create().  Returns with SIGPROF blocked, so the
 * caller must not disable preemption again.                                 */

static void init_library(void) {
	struct sigaction sa;
	int i;

	sigemptyset(&prof_set);
	sigaddset(&prof_set, SIGPROF);
	disable_preempt();

	for (i = 0; i < NUM_LEVELS; i++)
		memset(&runq[i], 0, sizeof runq[i]);
	memset(&finished_q, 0, sizeof finished_q);

	/* The thread that is already running -- the benchmark's main -- needs a TCB
	 * of its own, otherwise it could not be descheduled while it waits in
	 * worker_join() or on a mutex.  Its "stack" is the real process stack, so
	 * there is nothing to allocate and nothing to free. */
	main_tcb = (tcb *) calloc(1, sizeof(tcb));
	if (!main_tcb) {
		fprintf(stderr, "thread-worker: out of memory\n");
		exit(1);
	}
	main_tcb->id           = 0;
	main_tcb->status       = SCHEDULED;
	main_tcb->priority     = 0;
	main_tcb->is_main      = 1;
	main_tcb->started      = 1;
	main_tcb->arrival_ns   = now_ns();
	main_tcb->first_run_ns = main_tcb->arrival_ns;
	main_tcb->all_next     = all_threads;
	all_threads            = main_tcb;
	current                = main_tcb;

	/* The scheduler gets a context and a stack of its own. */
	sched_stack = (char *) malloc(STACK_SIZE);
	if (!sched_stack) {
		fprintf(stderr, "thread-worker: out of memory\n");
		exit(1);
	}
	if (getcontext(&sched_ctx) < 0) {
		perror("getcontext");
		exit(1);
	}
	sched_ctx.uc_link           = NULL;
	sched_ctx.uc_stack.ss_sp    = sched_stack;
	sched_ctx.uc_stack.ss_size  = STACK_SIZE;
	sched_ctx.uc_stack.ss_flags = 0;
	sigaddset(&sched_ctx.uc_sigmask, SIGPROF);   /* never preempt the scheduler */
	makecontext(&sched_ctx, (void (*)(void)) schedule, 0);

	/* sigaction() rather than signal(): signal() does not guarantee the
	 * handler stays installed.  SA_RESTART so that a blocking read inside a
	 * benchmark (external_cal's fscanf) is resumed rather than failing with
	 * EINTR once the thread is scheduled again. */
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = &timer_handler;
	sa.sa_flags   = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGPROF, &sa, NULL) < 0) {
		perror("sigaction");
		exit(1);
	}

	dispatch_us   = now_us();
	last_boost_us = dispatch_us;
	timer_armed   = 0;   /* armed as soon as a second thread becomes runnable */
	lib_init      = 1;
}

/* Every worker thread starts here instead of jumping straight into its body.
 * The wrapper exists so that a thread which simply returns -- rather than
 * calling worker_exit() -- is still cleaned up properly. */
static void worker_stub(void) {
	tcb  *me  = current;     /* the scheduler set this just before swapping in */
	void *ret = NULL;

	enable_preempt();
	if (me && me->function)
		ret = me->function(me->arg);
	worker_exit(ret);
}


/* create a new thread */
int worker_create(worker_t * thread, pthread_attr_t * attr, 
                      void *(*function)(void*), void * arg) {

       // - create Thread Control Block (TCB)
       // - create and initialize the context of this worker thread
       // - allocate space of stack for this thread to run
       // after everything is set, push this thread into run queue and 
       // - make it ready for the execution.

	tcb *t;

	(void) attr;                  /* ignored, as the write-up allows */

	if (!lib_init)
		init_library();           /* returns with SIGPROF already blocked */
	else
		disable_preempt();

	t = (tcb *) calloc(1, sizeof(tcb));
	if (!t) {
		enable_preempt();
		return -1;
	}

	t->stack = malloc(STACK_SIZE);
	if (!t->stack) {
		free(t);
		enable_preempt();
		return -1;
	}

	t->id       = next_tid++;
	t->function = function;
	t->arg      = arg;
	t->priority = 0;              /* new threads enter at the top MLFQ level */
	t->status   = READY;

	/* Build the context this thread will run in. */
	if (getcontext(&t->ctx) < 0) {
		free(t->stack);
		free(t);
		enable_preempt();
		return -1;
	}
	t->ctx.uc_link           = &sched_ctx;   /* fallback if the body returns */
	t->ctx.uc_stack.ss_sp    = t->stack;
	t->ctx.uc_stack.ss_size  = STACK_SIZE;
	t->ctx.uc_stack.ss_flags = 0;
	/* getcontext() copied the caller's signal mask, which currently has
	 * SIGPROF blocked.  A worker has to start out preemptible. */
	sigdelset(&t->ctx.uc_sigmask, SIGPROF);
	makecontext(&t->ctx, (void (*)(void)) worker_stub, 0);

	/* Response and turnaround times are both measured from here: the moment
	 * the thread is first put on a runqueue. */
	t->arrival_ns = now_ns();

	t->all_next = all_threads;
	all_threads = t;

	make_runnable(t);

	if (thread)
		*thread = t->id;

	enable_preempt();
	return 0;
};

/* give CPU possession to other user-level worker threads voluntarily */
int worker_yield() {
	
	// - change worker thread's state from Running to Ready
	// - save context of this thread to its thread control block
	// - switch from thread context to scheduler context

	disable_preempt();

	if (!lib_init || !current) {
		enable_preempt();
		return 0;
	}

	current->status = READY;

	disarm_timer();
	swapcontext(&current->ctx, &sched_ctx);

	/* Back on the CPU.  SIGPROF is still blocked (it was blocked when the
	 * context above was saved), so unblock it before returning. */
	enable_preempt();
	return 0;
};

/* terminate a thread */
void worker_exit(void *value_ptr) {
	// - de-allocate any dynamic memory created when starting this thread

	tcb *me;

	disable_preempt();

	if (!lib_init || !current) {
		enable_preempt();
		return;
	}

	me            = current;
	me->retval    = value_ptr;
	me->status    = FINISHED;
	me->finish_ns = now_ns();
	me->cpu_us   += now_us() - dispatch_us;

	if (!me->is_main) {
		/* Turnaround: creation to completion.  Response: creation to the first
		 * time this thread actually got the CPU (recorded at dispatch).  Both
		 * are folded into the running averages here, when the thread is really
		 * done, so that threads still in flight do not skew them. */
		total_turn += (double) (me->finish_ns - me->arrival_ns) / STAT_DIVISOR;
		total_resp += me->resp_time;
		completed++;
		avg_turn_time = total_turn / (double) completed;
		avg_resp_time = total_resp / (double) completed;
	}

	/* Let whoever is waiting for us in worker_join() run again. */
	if (me->joiner) {
		make_runnable(me->joiner);
		me->joiner = NULL;
	}

	/* This thread is standing on the stack it would have to free, so hand it
	 * to the scheduler and let it do the freeing from its own stack. */
	enqueue(&finished_q, me);

	disarm_timer();
	current = me;                 /* schedule() needs to see who just left */
	setcontext(&sched_ctx);       /* does not return */
};


/* Wait for thread termination */
int worker_join(worker_t thread, void **value_ptr) {
	
	// - wait for a specific thread to terminate
	// - de-allocate any dynamic memory created by the joining thread

	tcb *target;

	disable_preempt();

	if (!lib_init || !current) {
		enable_preempt();
		return -1;
	}

	target = find_tcb(thread);
	if (!target || target == current) {
		enable_preempt();
		return -1;            /* no such thread, or joining yourself */
	}

	while (target->status != FINISHED) {
		if (target->joiner && target->joiner != current) {
			enable_preempt();
			return -1;        /* somebody else is already joining it */
		}
		target->joiner  = current;
		current->status = BLOCKED;

		disarm_timer();
		swapcontext(&current->ctx, &sched_ctx);
		/* Resumed with SIGPROF still blocked; re-check the condition. */
	}

	if (value_ptr)
		*value_ptr = target->retval;

	/* The joined thread is gone for good now: drop its TCB (and its stack, if
	 * the scheduler has not already reclaimed it). */
	queue_remove(&finished_q, target);
	remove_from_all(target);
	free(target->stack);
	free(target);

	enable_preempt();
	return 0;
};

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, 
                          const pthread_mutexattr_t *mutexattr) {
	//- initialize data structures for this mutex

	(void) mutexattr;             /* ignored, as the write-up allows */

	if (!mutex)
		return -1;

	mutex->locked      = 0;
	mutex->owner       = NULL;
	mutex->waiters.head  = NULL;
	mutex->waiters.tail  = NULL;
	mutex->waiters.count = 0;
	mutex->initialized = 1;

	return 0;
};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex) {

        // - use the built-in test-and-set atomic function to test the mutex
        // - if the mutex is acquired successfully, enter the critical section
        // - if acquiring mutex fails, push current thread into block list and
        // context switch to the scheduler thread

	if (!mutex)
		return -1;

	/* Before the first worker_create() there is only one thread of execution,
	 * so there is nothing to serialize against. */
	if (!lib_init) {
		mutex->locked = 1;
		return 0;
	}

	/* Fast path.  An uncontended test-and-set touches no scheduler state, so
	 * it needs no preemption guard at all -- worth caring about, because the
	 * benchmarks take and release these mutexes millions of times and
	 * sigprocmask() is a real system call. */
	if (__sync_lock_test_and_set(&mutex->locked, 1) == 0) {
		mutex->owner = current;
		return 0;
	}

	/* Contended: from here on the wait list and the runqueues are in play. */
	disable_preempt();

	while (__sync_lock_test_and_set(&mutex->locked, 1) == 1) {
		/* Held by somebody else: block instead of spinning.  Spinning would
		 * deadlock -- this is one kernel thread, so the holder can only make
		 * progress if we give the CPU back. */
		current->status = BLOCKED;
		enqueue(&mutex->waiters, current);

		disarm_timer();
		swapcontext(&current->ctx, &sched_ctx);
		/* Woken by worker_mutex_unlock(); go around and retry the
		 * test-and-set, since another waiter may have won the race. */
	}

	mutex->owner = current;

	enable_preempt();
        return 0;
};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex) {
	// - release mutex and make it available again. 
	// - put threads in block list to run queue 
	// so that they could compete for mutex later.

	tcb *t;

	if (!mutex)
		return -1;

	if (!lib_init) {
		mutex->locked = 0;
		return 0;
	}

	mutex->owner = NULL;
	__sync_lock_release(&mutex->locked);

	/* Release first, then look for waiters -- never the other way round.  If
	 * this thread were preempted after finding the wait list empty but before
	 * releasing, a thread that blocked in the meantime would never be woken.
	 * In this order the worst case is a thread that finds the lock already
	 * free and takes it without ever being queued, which is harmless. */
	if (mutex->waiters.head == NULL)
		return 0;

	disable_preempt();

	/* Hand everybody back to the scheduler and let them compete for the lock
	 * again, rather than picking a winner here. */
	while ((t = dequeue(&mutex->waiters)) != NULL)
		make_runnable(t);

	enable_preempt();
	return 0;
};


/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex) {
	// - de-allocate dynamic memory created in worker_mutex_init

	tcb *t;

	if (!mutex)
		return -1;

	disable_preempt();

	/* Release it before tearing it down, so nothing is left blocked forever. */
	while ((t = dequeue(&mutex->waiters)) != NULL)
		make_runnable(t);

	__sync_lock_release(&mutex->locked);
	mutex->owner       = NULL;
	mutex->initialized = 0;

	enable_preempt();
	return 0;
};

/* scheduler */
static void schedule() {
	// - every time a timer interrupt occurs, your worker thread library 
	// should be contexted switched from a thread context to this 
	// schedule() function

	// - invoke scheduling algorithms according to the policy (PSJF or MLFQ)

	// if (sched == PSJF)
	//		sched_psjf();
	// else if (sched == MLFQ)
	// 		sched_mlfq();

	/* Entered from three places: the SIGPROF handler (quantum expired), a
	 * voluntary swapcontext() in yield/join/mutex_lock, and setcontext() from
	 * worker_exit().  Every one of them lands back at the swapcontext() at the
	 * bottom of this loop, so the loop just runs forever. */

	for (;;) {
		tcb *prev = current;
		tcb *next;
		long now  = now_us();

		/* --- 1. charge the thread that just left the CPU ----------------- */
		if (prev) {
			long ran = now - dispatch_us;
			if (ran < 0)
				ran = 0;

			if (prev->status != FINISHED) {
				prev->cpu_us += ran;

				/* PSJF has no idea how long a thread will run for, so it uses
				 * CPU time already consumed as the estimate: the longer a
				 * thread has run, the longer it is assumed to still need. */
				prev->elapsed = prev->cpu_us / ((long) QUANTUM * 1000L);
			}

			if (prev->status == READY) {
#ifdef MLFQ
				mlfq_charge(prev, ran);   /* rules 2-4: demote when used up */
#endif
				make_runnable(prev);
			}

			/* BLOCKED threads are already parked on a wait list, and FINISHED
			 * threads are on finished_q -- neither goes on a runqueue. */
		}
		current = NULL;

		/* --- 2. reclaim the stacks of threads that have exited ----------- */
		reap_finished();

		/* --- 3. pick the next thread ------------------------------------- */

// - schedule policy
#ifndef MLFQ
	// Choose PSJF
		sched_psjf();
#else 
	// Choose MLFQ
		sched_mlfq();
#endif

		next = current;

		if (!next) {
			/* Nothing is runnable.  Either every remaining thread is blocked
			 * -- a real deadlock -- or there is simply nothing left to run,
			 * which can only happen once main itself has exited. */
			int blocked = 0;
			tcb *t;

			disarm_timer();
			for (t = all_threads; t; t = t->all_next)
				if (t->status == BLOCKED)
					blocked = 1;

			if (blocked) {
				fprintf(stderr,
				        "thread-worker: deadlock -- every thread is blocked\n");
				exit(1);
			}
			exit(0);
		}

		/* --- 4. give it the CPU ------------------------------------------ */
		next->status = SCHEDULED;

		if (!next->started) {
			next->started      = 1;
			next->first_run_ns = now_ns();
			if (!next->is_main)
				next->resp_time = (double) (next->first_run_ns - next->arrival_ns)
				                  / STAT_DIVISOR;
		}

		next->cntx_switches++;
		tot_cntx_switches++;

		/* Only run the clock when somebody else is actually waiting for the
		 * CPU.  With a single runnable thread there is nothing to switch to,
		 * so preempting it would just burn context switches. */
		if (runnable_count() > 0)
			arm_timer(slice_for(next));
		else if (timer_armed)
			disarm_timer();

		dispatch_us = now_us();
		swapcontext(&sched_ctx, &next->ctx);
	}
}

#ifndef MLFQ
/* Pre-emptive Shortest Job First (POLICY_PSJF) scheduling algorithm */
static void sched_psjf() {
	// - your own implementation of PSJF
	// (feel free to modify arguments and return types)

	/* "Shortest job" is approximated by the thread that has consumed the
	 * fewest whole quanta so far (see the comment in schedule()).  A strict
	 * `<` keeps the comparison stable, so threads tied on elapsed time are
	 * served in FIFO order. */

	tcb *best = NULL;
	tcb *t;

	for (t = runq[0].head; t; t = t->qnext)
		if (!best || t->elapsed < best->elapsed)
			best = t;

	if (best)
		queue_remove(&runq[0], best);

	current = best;
}
#endif /* !MLFQ */


#ifdef MLFQ
/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq() {
	// - your own implementation of MLFQ
	// (feel free to modify arguments and return types)

	int i;

	/* Rule 5: every BOOST_PERIOD milliseconds, move every thread back to the
	 * topmost queue.  This is what stops long-running threads from starving at
	 * the bottom, and it also defeats a thread that games the scheduler by
	 * yielding just before its slice runs out. */
	if (now_us() - last_boost_us >= (long) BOOST_PERIOD * 1000L) {
		tcb *t;

		for (i = 1; i < NUM_LEVELS; i++)
			while ((t = dequeue(&runq[i])) != NULL) {
				t->priority = 0;
				t->allot_us = 0;
				enqueue(&runq[0], t);
			}

		/* Blocked threads get boosted too, so that they come back at the top
		 * once whatever they are waiting on releases them. */
		for (t = all_threads; t; t = t->all_next)
			if (t->status == BLOCKED) {
				t->priority = 0;
				t->allot_us = 0;
			}

		last_boost_us = now_us();
	}

	/* Rules 1 and 2: always run something from the highest non-empty queue,
	 * round-robin within a level. */
	for (i = 0; i < NUM_LEVELS; i++) {
		tcb *t = dequeue(&runq[i]);
		if (t) {
			current = t;
			return;
		}
	}

	current = NULL;
}
#endif /* MLFQ */

//DO NOT MODIFY THIS FUNCTION
/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void) {

       fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
       fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
       fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}


// Feel free to add any other functions you need

/* Release what is still outstanding when the process exits.  The timer has to
 * be stopped first: a SIGPROF arriving mid-teardown would try to switch onto
 * memory that is being freed.
 *
 * Two allocations are deliberately left alone, because this destructor can run
 * on either of them.  A benchmark may call exit() from inside a worker thread
 * -- external_cal does exactly that when ./genRecord.sh has not been run yet
 * -- which would run this code on that worker's stack; and exit() from
 * schedule() runs it on the scheduler's stack.  Unmapping the stack underneath
 * ourselves would turn a clean exit into a segfault, and the kernel reclaims
 * both a moment later anyway. */
__attribute__((destructor))
static void worker_lib_dtor(void) {
	tcb *t;

	if (!lib_init)
		return;

	disable_preempt();
	disarm_timer();

	t = all_threads;
	while (t) {
		tcb *nxt = t->all_next;
		if (t != current)
			free(t->stack);     /* not the stack we may be running on */
		free(t);
		t = nxt;
	}
	all_threads = NULL;
	current     = NULL;
	main_tcb    = NULL;
	lib_init    = 0;
}
