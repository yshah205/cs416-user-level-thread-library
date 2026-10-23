#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "../thread-worker.h"

/* A scratch program template on which to call and
 * test thread-worker library functions as you implement
 * them.
 *
 * You can modify and use this program as much as possible.
 * This will not be graded.
 *
 * Functional smoke test for the worker thread library.  Run this before the
 * real benchmarks: it is fast and it checks each API in isolation, so a
 * failure here points straight at the function that is broken.
 *
 *     $ make && ./test [num_threads]
 */

#define DEFAULT_THREAD_NUM 8
#define LOOPS              2000

static pthread_mutex_t  mutex;
static int              thread_num;
static int              shared_counter = 0;   /* guarded by mutex    */
static int              unguarded      = 0;   /* deliberately racy   */
static long             retvals[256];

static int failures = 0;

static void check(int cond, const char *what) {
	if (cond) {
		printf("  ok    %s\n", what);
	} else {
		printf("  FAIL  %s\n", what);
		failures++;
	}
}

/* Each worker hammers a mutex-protected counter.  If the mutex works, the
 * final value is exact; if it does not, increments get lost. */
static void *counting_worker(void *arg) {
	int  id = *((int *) arg);
	int  i;

	for (i = 0; i < LOOPS; i++) {
		pthread_mutex_lock(&mutex);
		shared_counter++;
		pthread_mutex_unlock(&mutex);

		unguarded++;

		if ((i % 256) == 0)
			worker_yield();          /* exercise the voluntary path */
	}

	/* hand a recognizable value back through worker_exit() */
	pthread_exit((void *) (long) (id * 100 + 7));
	return NULL;
}

/* A worker that never blocks and never yields: it can only lose the CPU by
 * being preempted, so reaching the end of this proves preemption works. */
static void *spinning_worker(void *arg) {
	volatile long i;
	long          budget = 20000000L;

	(void) arg;
	for (i = 0; i < budget; i++)
		;
	pthread_exit((void *) 42L);
	return NULL;
}

int main(int argc, char **argv) {

	pthread_t *threads;
	int       *ids;
	int        i;
	void      *ret;

	thread_num = (argc > 1) ? atoi(argv[1]) : DEFAULT_THREAD_NUM;
	if (thread_num < 1 || thread_num > 256) {
		printf("enter a valid thread number (1-256)\n");
		return 1;
	}

	printf("thread-worker smoke test: %d threads, QUANTUM = %d ms\n",
	       thread_num, QUANTUM);
#ifdef MLFQ
	printf("policy: MLFQ (%d levels, boost every %d ms)\n\n",
	       NUM_LEVELS, BOOST_PERIOD);
#else
	printf("policy: PSJF\n\n");
#endif

	threads = (pthread_t *) malloc(thread_num * sizeof(pthread_t));
	ids     = (int *)       malloc(thread_num * sizeof(int));
	for (i = 0; i < thread_num; i++)
		ids[i] = i;

	pthread_mutex_init(&mutex, NULL);

	/* ---- create / join / exit / mutex ---------------------------------- */
	printf("create, join, exit, mutex\n");

	for (i = 0; i < thread_num; i++)
		check(pthread_create(&threads[i], NULL, &counting_worker, &ids[i]) == 0,
		      "worker_create returned 0");

	for (i = 0; i < thread_num; i++) {
		ret = NULL;
		if (pthread_join(threads[i], &ret) != 0) {
			check(0, "worker_join returned 0");
			continue;
		}
		retvals[i] = (long) ret;
	}
	check(1, "every worker_join returned");

	for (i = 0; i < thread_num; i++)
		if (retvals[i] != ids[i] * 100 + 7) {
			printf("  FAIL  thread %d returned %ld, expected %d\n",
			       i, retvals[i], ids[i] * 100 + 7);
			failures++;
			break;
		}
	if (i == thread_num)
		check(1, "worker_exit value came back through worker_join");

	check(shared_counter == thread_num * LOOPS,
	      "mutex kept the guarded counter exact");
	printf("        guarded   = %d (expected %d)\n",
	       shared_counter, thread_num * LOOPS);
	printf("        unguarded = %d  (lower than %d means threads really "
	       "interleaved)\n", unguarded, thread_num * LOOPS);

	pthread_mutex_destroy(&mutex);

	/* ---- preemption ---------------------------------------------------- */
	printf("\npreemption: two threads that never yield\n");
	{
		pthread_t s[2];
		long      before = tot_cntx_switches;

		for (i = 0; i < 2; i++)
			pthread_create(&s[i], NULL, &spinning_worker, NULL);
		for (i = 0; i < 2; i++) {
			ret = NULL;
			pthread_join(s[i], &ret);
			check((long) ret == 42L, "spinning worker ran to completion");
		}
		check(tot_cntx_switches > before + 2,
		      "the timer forced extra context switches");
		printf("        context switches during this phase: %ld\n",
		       tot_cntx_switches - before);
	}

	free(threads);
	free(ids);

	printf("\n%s\n\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
	print_app_stats();

	return failures ? 1 : 0;
}
