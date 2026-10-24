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

static int         lib_init   = 0;
static ucontext_t  sched_ctx;          // scheduler runs here
static char       *sched_stack = NULL;
static tcb        *current    = NULL;  // thread on the CPU now
static tcb        *all_threads = NULL; // all live TCBs
static worker_t    next_tid   = 1;     // 0 is main

// PSJF uses runq[0] only; MLFQ uses all levels, runq[0] highest
static tcb_queue   runq[NUM_LEVELS];

// exited threads, stacks freed later by the scheduler
static tcb_queue   finished_q;

static sigset_t    prof_set;           // { SIGPROF }
static int         timer_armed  = 0;
static long        dispatch_us  = 0;   // when current got the CPU
static long        last_boost_us = 0;

// running totals for the averages (main thread excluded)
static long        completed  = 0;
static double      total_turn = 0;
static double      total_resp = 0;

static void schedule();
#ifndef MLFQ
static void sched_psjf();
#else
static void sched_mlfq();
#endif


// microsecond clock for scheduling
static long now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

// nanosecond clock for metrics (us would round short threads to 0)
static long now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

// block/unblock SIGPROF around anything that touches shared state
// (user-level version of disabling interrupts)
static void disable_preempt(void) {
	sigprocmask(SIG_BLOCK, &prof_set, NULL);
}

static void enable_preempt(void) {
	sigprocmask(SIG_UNBLOCK, &prof_set, NULL);
}


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

// remove from the middle (PSJF picks the min, not the head)
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


// one-shot ITIMER_PROF, re-armed every dispatch so each MLFQ level can have
// its own slice and an early yield isn't cut short by the old quantum
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

// MLFQ: lower priority -> longer slice
static long slice_for(tcb *t) {
#ifndef MLFQ
	return (long) QUANTUM * 1000L;
#else
	return (long) QUANTUM * 1000L * (t->priority + 1);
#endif
}

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

	// if we just woke a thread while the running one has the clock off,
	// re-arm it so a non-yielding thread can't starve the one it woke
	if (!timer_armed && current && current->status == SCHEDULED)
		arm_timer(slice_for(current));
}

#ifdef MLFQ
// charge CPU time to the thread's allotment at its level and demote once used
// up (rules 2-4). charging time, not timer ticks, stops gaming: yielding early
// keeps the leftover allotment but never refunds it.
static void mlfq_charge(tcb *t, long ran) {
	long slice = slice_for(t);

	t->allot_us += ran;
	if (t->allot_us < slice)
		return;

	if (t->priority < NUM_LEVELS - 1) {
		t->priority++;
		t->allot_us = 0;
	} else {
		t->allot_us = slice;      // already at the bottom
	}
}
#endif

// free exited stacks -- only safe from the scheduler's own stack. the TCB
// stays around so join() can still read the return value.
static void reap_finished(void) {
	tcb *t;
	while ((t = dequeue(&finished_q)) != NULL) {
		free(t->stack);
		t->stack = NULL;
	}
}


// quantum expired -> jump to the scheduler. swapcontext saves/restores the
// signal mask too: sched_ctx has SIGPROF blocked so the scheduler is never
// preempted, and a resumed thread returns through here and unblocks it again.
static void timer_handler(int signum) {
	if (!current || current->status != SCHEDULED)
		return;

	timer_armed = 0;          // one-shot, just fired
	current->status = READY;
	swapcontext(&current->ctx, &sched_ctx);
}


// first-call setup; returns with SIGPROF blocked
static void init_library(void) {
	struct sigaction sa;
	tcb *main_tcb;
	int i;

	sigemptyset(&prof_set);
	sigaddset(&prof_set, SIGPROF);
	disable_preempt();

	for (i = 0; i < NUM_LEVELS; i++)
		memset(&runq[i], 0, sizeof runq[i]);
	memset(&finished_q, 0, sizeof finished_q);

	// main needs a TCB so it can be descheduled in join()/mutex_lock. its
	// stack is the real process stack, nothing to allocate or free.
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
	sigaddset(&sched_ctx.uc_sigmask, SIGPROF);   // scheduler is never preempted
	makecontext(&sched_ctx, (void (*)(void)) schedule, 0);

	// sigaction (not signal, which may not stay installed); SA_RESTART so
	// external_cal's fscanf resumes instead of failing with EINTR
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
	timer_armed   = 0;
	lib_init      = 1;
}

// threads start here so a body that just returns is still cleaned up
static void worker_stub(void) {
	tcb  *me  = current;
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

	if (!lib_init)
		init_library();           // returns with SIGPROF blocked
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
	t->priority = 0;
	t->status   = READY;

	if (getcontext(&t->ctx) < 0) {
		free(t->stack);
		free(t);
		enable_preempt();
		return -1;
	}
	t->ctx.uc_link           = &sched_ctx;   // fallback if the body returns
	t->ctx.uc_stack.ss_sp    = t->stack;
	t->ctx.uc_stack.ss_size  = STACK_SIZE;
	t->ctx.uc_stack.ss_flags = 0;
	// getcontext copied our mask (SIGPROF blocked); workers start preemptible
	sigdelset(&t->ctx.uc_sigmask, SIGPROF);
	makecontext(&t->ctx, (void (*)(void)) worker_stub, 0);

	t->arrival_ns = now_ns();     // response/turnaround measured from here

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

	enable_preempt();         // back on the CPU, re-enable preemption
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

	// fold this thread's metrics into the averages now that it's really done
	if (!me->is_main) {
		total_turn += (double) (me->finish_ns - me->arrival_ns) / STAT_DIVISOR;
		total_resp += me->resp_time;
		completed++;
		avg_turn_time = total_turn / (double) completed;
		avg_resp_time = total_resp / (double) completed;
	}

	// wake anyone waiting on us in join()
	if (me->joiner) {
		make_runnable(me->joiner);
		me->joiner = NULL;
	}

	// can't free our own stack here; let the scheduler do it
	enqueue(&finished_q, me);

	disarm_timer();
	current = me;                 // so schedule() sees who left
	setcontext(&sched_ctx);       // does not return
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
		return -1;            // no such thread, or joining self
	}

	while (target->status != FINISHED) {
		if (target->joiner && target->joiner != current) {
			enable_preempt();
			return -1;        // already being joined by someone else
		}
		target->joiner  = current;
		current->status = BLOCKED;

		disarm_timer();
		swapcontext(&current->ctx, &sched_ctx);
		// resumed with SIGPROF still blocked; re-check the condition
	}

	if (value_ptr)
		*value_ptr = target->retval;

	// target is done for good now; drop its TCB and stack
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

	// single thread of execution before the first worker_create
	if (!lib_init) {
		mutex->locked = 1;
		return 0;
	}

	// if the lock is free, take it without the preemption/syscall overhead.
	// this matters a lot: the benchmarks lock/unlock millions of times.
	if (__sync_lock_test_and_set(&mutex->locked, 1) == 0) {
		mutex->owner = current;
		return 0;
	}

	disable_preempt();

	// held: block instead of spinning (one kernel thread, so spinning would
	// never let the holder run) and retry after being woken
	while (__sync_lock_test_and_set(&mutex->locked, 1) == 1) {
		current->status = BLOCKED;
		enqueue(&mutex->waiters, current);

		disarm_timer();
		swapcontext(&current->ctx, &sched_ctx);
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

	// nobody waiting: nothing to do, skip the scheduler/syscall overhead
	if (mutex->waiters.head == NULL)
		return 0;

	disable_preempt();

	// wake all waiters and let them compete for the lock again
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

	if (!lib_init) {
		mutex->locked      = 0;
		mutex->owner       = NULL;
		mutex->initialized = 0;
		return 0;
	}

	disable_preempt();

	// wake anything still waiting so it isn't stuck on a dead mutex
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

	// entered from the timer handler, a voluntary swap (yield/join/lock), and
	// setcontext() in exit; all land at the swap at the bottom, so loop forever
	for (;;) {
		tcb *prev = current;
		tcb *next;
		long now  = now_us();

		// 1. charge the thread that just left the CPU
		if (prev) {
			long ran = now - dispatch_us;
			if (ran < 0)
				ran = 0;

			if (prev->status != FINISHED) {
				prev->cpu_us += ran;
				// PSJF estimate: more CPU used so far => assumed longer job
				prev->elapsed = prev->cpu_us / ((long) QUANTUM * 1000L);
			}

			if (prev->status == READY) {
#ifdef MLFQ
				mlfq_charge(prev, ran);
#endif
				make_runnable(prev);
			}
			// BLOCKED are on a wait list, FINISHED on finished_q
		}
		current = NULL;

		// 2. reclaim exited stacks
		reap_finished();

		// 3. pick the next thread
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
			// nothing runnable: deadlock if anything is blocked, else main exited
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

		// 4. give it the CPU
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

		// only run the clock if someone else is waiting; no point preempting
		// the only runnable thread
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

	// pick the thread with the fewest quanta used; strict < keeps ties FIFO
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

	// rule 5: every BOOST_PERIOD ms move everyone back to the top queue so
	// low-priority threads don't starve
	if (now_us() - last_boost_us >= (long) BOOST_PERIOD * 1000L) {
		tcb *t;

		for (i = 1; i < NUM_LEVELS; i++)
			while ((t = dequeue(&runq[i])) != NULL) {
				t->priority = 0;
				t->allot_us = 0;
				enqueue(&runq[0], t);
			}

		// boost blocked threads too, so they come back at the top
		for (t = all_threads; t; t = t->all_next)
			if (t->status == BLOCKED) {
				t->priority = 0;
				t->allot_us = 0;
			}

		last_boost_us = now_us();
	}

	// rules 1-2: run from the highest non-empty queue, RR within a level
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
