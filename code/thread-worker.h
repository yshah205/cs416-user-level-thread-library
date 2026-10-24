// File:	worker_t.h

// List all group member's name: Yug Shah
// username of iLab: ys991
// iLab Server: ilab2.cs.rutgers.edu

#ifndef WORKER_T_H
#define WORKER_T_H

#define _GNU_SOURCE

// lets the ucontext functions compile on macOS (iLab/Linux ignores this)
#if defined(__APPLE__)
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
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

// scheduling quantum in ms (graders change this; can also pass -DQUANTUM=)
#ifndef QUANTUM
#define QUANTUM 10
#endif

// MLFQ: number of priority levels, 0 = highest
#ifndef NUM_LEVELS
#define NUM_LEVELS 4
#endif

// MLFQ rule 5: boost everyone to the top every this-many ms
#ifndef BOOST_PERIOD
#define BOOST_PERIOD 100
#endif

// turnaround/response reported in us; -DSTATS_IN_MS for ms. timestamps are
// kept in ns so a sub-microsecond thread doesn't round to 0.
#ifdef STATS_IN_MS
#define STAT_DIVISOR 1000000.0
#else
#define STAT_DIVISOR 1000.0
#endif

// SIGSTKSZ is too small on some systems for fscanf etc., so use a fixed size
#define STACK_SIZE (128 * 1024)

#define READY     0
#define SCHEDULED 1
#define BLOCKED   2
#define FINISHED  3   // exited, waiting to be joined

typedef struct TCB {
	/* add important states in a thread control block */
	// thread Id
	// thread status
	// thread context
	// thread stack
	// thread priority
	// And more ...

	worker_t      id;             // 0 is the main thread
	int           status;
	ucontext_t    ctx;
	void         *stack;          // NULL for main
	int           priority;       // MLFQ level

	void       *(*function)(void *);
	void         *arg;
	void         *retval;

	long          cpu_us;         // total CPU time used
	long          elapsed;        // quanta used -- PSJF key
	long          allot_us;       // CPU used at current MLFQ level
	long          cntx_switches;

	long          arrival_ns;     // first put on a runqueue
	long          first_run_ns;   // first scheduled
	long          finish_ns;      // exited
	double        resp_time;
	int           started;
	int           is_main;

	struct TCB   *joiner;         // thread waiting on us in join()

	struct TCB   *qnext;          // runqueue / wait-queue link
	struct TCB   *all_next;       // global list of all TCBs
} tcb;

// FIFO queue of TCBs with a tail pointer (runqueues, wait lists, exited list)
typedef struct tcb_queue {
	tcb *head;
	tcb *tail;
	int  count;
} tcb_queue;

/* mutex struct definition */
typedef struct worker_mutex_t {
	volatile int  locked;       // 0 free, 1 held (test-and-set)
	int           initialized;
	tcb          *owner;
	tcb_queue     waiters;
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
