# User-level Thread Library and Scheduler

A pthread-like threading library implemented entirely in user space, with two
preemptive schedulers: **PSJF** (pre-emptive shortest job first, a.k.a. STCF) and
**MLFQ** (multi-level feedback queue). Rutgers CS416, Project 2.

Everything runs on a single kernel thread. Threads get their own `ucontext`-based
stacks, a `SIGPROF` interval timer drives preemption, and a scheduler context
decides who runs next.

## API

```c
int  worker_create(worker_t *thread, pthread_attr_t *attr,
                   void *(*function)(void *), void *arg);
int  worker_yield(void);
void worker_exit(void *value_ptr);
int  worker_join(worker_t thread, void **value_ptr);

int  worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t *attr);
int  worker_mutex_lock(worker_mutex_t *mutex);
int  worker_mutex_unlock(worker_mutex_t *mutex);
int  worker_mutex_destroy(worker_mutex_t *mutex);
```

With `USE_WORKERS` defined in `thread-worker.h`, the `pthread_*` names are macro'd
onto these, so existing pthread code links against the library unchanged. Comment
that macro out to build the same program against native pthreads for comparison.

## Build

```bash
cd code
make                 # PSJF (default)
make SCHED=MLFQ      # MLFQ
make QUANTUM=25      # override the 10 ms default quantum
make STATS=MS        # report turnaround/response in ms instead of us
```

Produces `libthread-worker.a`. Then:

```bash
cd benchmarks
make
./genRecord.sh       # once; generates the data external_cal reads
./test 8             # functional smoke test
./parallel_cal 50    # CPU-bound
./vector_multiply 50 # CPU-bound, heavy mutex traffic
./external_cal 50    # I/O-bound
```

`./run_benchmarks.sh` sweeps both policies across several quanta and thread counts,
adds a native-pthread baseline, and writes `results.csv` plus a Markdown table.

`vector_multiply` allocates roughly 229 MB of BSS and performs 30 million mutex
lock/unlock pairs, which makes it the heaviest test of synchronization cost here.

**Linux only.** `makecontext`/`swapcontext` are unimplemented on Apple Silicon —
they compile but segfault — so this does not run on an arm64 Mac. It compiles there
(there is a small `__APPLE__` shim at the top of the header purely for
syntax-checking) but must be run on Linux.

## How it works

**Contexts.** Worker threads each get a 128 KB `malloc`'d stack and a context built
with `makecontext`. The scheduler gets its own context and stack. The main thread
gets a TCB too (id 0), which is what lets it block in `worker_join` or on a mutex
through the same code path as any worker, with no special cases anywhere.

**Preemption.** A one-shot `ITIMER_PROF` timer, re-armed on every dispatch so each
MLFQ level can have its own slice length. The handler just switches to the
scheduler, which works because `swapcontext` saves and restores the *signal mask*
along with the registers: the scheduler context carries `SIGPROF` blocked, so it
can never be interrupted while it is manipulating the runqueues, and a thread
resumed later returns into the handler and unblocks the signal on the normal
signal-return path.

**Mutexes** use `__sync_lock_test_and_set`, with a lock-free fast path for the
uncontended case — the benchmarks take and release mutexes millions of times, and
guarding that path would cost four `sigprocmask` syscalls per critical section for
nothing. `unlock` releases the lock *before* inspecting the wait list; the other
order loses wakeups. Contended waiters block rather than spin, since on one kernel
thread a spinner can never let the holder make progress.

**PSJF** approximates "shortest job" with CPU time already consumed — the longer a
thread has run, the longer it is assumed to still need — which makes it behave like
a fair-share scheduler. Ties break FIFO.

**MLFQ** uses four levels, with level *i* getting `(i+1) * QUANTUM`, so higher
priority means a shorter slice. Demotion charges *elapsed time* against a per-level
allotment rather than counting timer interrupts, which implements OSTEP's rule 4
and means a thread cannot game the scheduler by yielding just before each slice
expires. A periodic boost (rule 5) refunds allotments and lifts everyone back to
level 0, bounding starvation.

**Exiting threads don't free their own stacks.** A 128 KB block is at glibc's mmap
threshold, so `free` may unmap it — and unmapping the stack you are running on is
an instant segfault. The exiting thread hands its TCB to the scheduler, which reaps
the stack from its own stack on the next pass.

**The timer is only armed when another thread is actually waiting for the CPU.**
Preempting the only runnable thread accomplishes nothing, and leaving it un-clocked
also keeps the benchmarks' single-threaded `verify()` phase from inflating the
reported context-switch count.

See [REPORT.md](REPORT.md) for the full design write-up.

## Layout

```
code/thread-worker.c        the library and both schedulers
code/thread-worker.h        TCB, mutex, queues, tunables
code/Makefile               builds libthread-worker.a
code/benchmarks/            provided benchmarks + smoke test + sweep script
code/sample-code/           provided ucontext/timer examples
REPORT.md                   design write-up and analysis
```

## Status

The library compiles warning-free under both policies, and the scheduling logic is
covered by 49 assertions that drive the internals directly (queue invariants, PSJF
minimum-selection and tie-breaking, MLFQ level ordering and slice lengths,
allotment-based demotion including the yield-gaming case, and the priority boost).

End-to-end benchmark numbers still need to be collected on a Linux host; the tables
in `REPORT.md` are not yet filled in.
