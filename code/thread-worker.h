// File:	worker_t.h

// List all group member's name:
// username of iLab:
// iLab Server:

#ifndef WORKER_T_H
#define WORKER_T_H

#define _GNU_SOURCE

/* The ucontext routines are deprecated on macOS and are hidden by the SDK
 * unless _XOPEN_SOURCE is defined.  This block exists only so the library can
 * be syntax-checked on a Mac; it compiles away on Linux (the iLab machines)
 * and can be deleted without affecting anything. */
#if defined(__APPLE__)
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
/* _XOPEN_SOURCE hides the BSD typedefs that _GNU_SOURCE gives us on Linux. */
#define _DARWIN_C_SOURCE 1
#endif

/* To use Linux pthread Library in Benchmark, you have to comment the USE_WORKERS macro */
#define USE_WORKERS 1

/* include lib header files that you need here: */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <ucontext.h>
#include <pthread.h>

typedef uint worker_t;

/* ------------------------------------------------------------------------ *
 * Tunables                                                                 *
 * ------------------------------------------------------------------------ */

/* Length of one scheduling quantum, in MILLISECONDS.  This is the knob the
 * graders change, so it lives here and can also be overridden on the command
 * line with -DQUANTUM=<ms>. */
#ifndef QUANTUM
#define QUANTUM 10
#endif

/* MLFQ: number of priority levels.  Level 0 is the highest priority and gets
 * the shortest time slice; level i gets (i+1) * QUANTUM milliseconds. */
#ifndef NUM_LEVELS
#define NUM_LEVELS 4
#endif

/* MLFQ rule 5: period S, in MILLISECONDS, after which every thread is boosted
 * back to the topmost queue so that nothing starves. */
#ifndef BOOST_PERIOD
#define BOOST_PERIOD 100
#endif

/* Per-thread stack size.  The write-up suggests SIGSTKSZ or "a few tens of
 * kilobytes".  SIGSTKSZ is only 8-16 KB on some glibc builds, which is tight
 * for benchmark code that calls fscanf(), so use a fixed 128 KB instead. */
#define STACK_SIZE (128 * 1024)

/* ------------------------------------------------------------------------ *
 * Thread states                                                            *
 * ------------------------------------------------------------------------ */

#define READY     0   /* on a runqueue, waiting for the CPU */
#define SCHEDULED 1   /* currently on the CPU */
#define BLOCKED   2   /* waiting on a mutex or on another thread to exit */
#define FINISHED  3   /* called worker_exit(), not yet joined */

typedef struct TCB {
	/* add important states in a thread control block */
	// thread Id
	// thread status
	// thread context
	// thread stack
	// thread priority
	// And more ...

	worker_t      id;             /* unique thread id (0 is the main thread) */
	int           status;         /* READY / SCHEDULED / BLOCKED / FINISHED */
	ucontext_t    ctx;            /* saved execution state */
	void         *stack;          /* malloc'd stack backing ctx (NULL for main) */
	int           priority;       /* MLFQ level, 0 = highest priority */

	/* what this thread runs */
	void       *(*function)(void *);
	void         *arg;
	void         *retval;         /* value handed to worker_exit() */

	/* scheduling bookkeeping */
	long          cpu_us;         /* total CPU time consumed, microseconds */
	long          elapsed;        /* whole quanta consumed -- the PSJF key */
	long          allot_us;       /* CPU used at the current MLFQ level */
	long          cntx_switches;  /* times this thread was put on the CPU */

	/* metrics */
	long          arrival_us;     /* when it was first put on a runqueue */
	long          first_run_us;   /* when it was first scheduled */
	long          finish_us;      /* when it called worker_exit() */
	double        resp_ms;        /* first_run_us - arrival_us, in ms */
	int           started;        /* has it been scheduled at least once? */
	int           is_main;        /* excluded from the per-thread averages */

	struct TCB   *joiner;         /* thread blocked in worker_join() on us */

	struct TCB   *qnext;          /* runqueue / wait-queue link */
	struct TCB   *all_next;       /* global list of live TCBs */
} tcb;

/* A simple FIFO of TCBs, with a tail pointer so that enqueue is O(1).
 * Used for the runqueues, the mutex wait lists and the exited list. */
typedef struct tcb_queue {
	tcb *head;
	tcb *tail;
	int  count;
} tcb_queue;

/* mutex struct definition */
typedef struct worker_mutex_t {
	volatile int  locked;       /* 0 = free, 1 = held; set with test-and-set */
	int           initialized;
	tcb          *owner;        /* thread currently holding the mutex */
	tcb_queue     waiters;      /* threads blocked trying to acquire it */
} worker_mutex_t;

/* Function Declarations: */

/* create a new thread */
int worker_create(worker_t * thread, pthread_attr_t * attr, void
    *(*function)(void*), void * arg);

/* give CPU pocession to other user level worker threads voluntarily */
int worker_yield();

/* terminate a thread */
void worker_exit(void *value_ptr);

/* wait for thread termination */
int worker_join(worker_t thread, void **value_ptr);

/* initial the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t
    *mutexattr);

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex);

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex);

/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex);


/* Global statistics, defined in thread-worker.c. */
extern long   tot_cntx_switches;
extern double avg_turn_time;
extern double avg_resp_time;

/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void);

#ifdef USE_WORKERS
#define pthread_t worker_t
#define pthread_mutex_t worker_mutex_t
#define pthread_create worker_create
#define pthread_exit worker_exit
#define pthread_join worker_join
#define pthread_mutex_init worker_mutex_init
#define pthread_mutex_lock worker_mutex_lock
#define pthread_mutex_unlock worker_mutex_unlock
#define pthread_mutex_destroy worker_mutex_destroy
#endif

#endif
