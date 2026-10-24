# CS416 Project 2 — User-level Thread Library and Scheduler

**Name(s):** Yug Shah
**NetID(s):** ys991
**iLab machine used:** _(fill in, e.g. ilab1.cs.rutgers.edu)_

---

## 1. Implementation logic

### 1.0 Overall structure

The library is a single kernel thread that multiplexes N user-level threads over
it. There are three kinds of `ucontext_t` in play:

| context | stack | created | purpose |
|---|---|---|---|
| worker contexts | `malloc`'d, 128 KB each | `worker_create` | run the thread bodies |
| scheduler context | `malloc`'d, 128 KB | first `worker_create` | runs `schedule()` |
| main context | the real process stack | lazily, on the first switch away | runs the benchmark's `main` |

The main thread gets a TCB of its own (thread id 0). That is the single decision
that most simplifies everything else: because `main` is an ordinary schedulable
entity, it can be blocked in `worker_join` or on a mutex and resumed through
exactly the same code path as any worker. Nothing in the library needs a special
case for "the thread that is not really a thread". `main` is excluded from the
reported averages, since it is not a worker created through `worker_create` and
would otherwise distort both of them.

The library is set up lazily, on the first call to `worker_create`. That matters
for the benchmarks: `parallel_cal`, for instance, allocates and fills a
10000 x 100000 array *before* it creates any thread, and none of that work should
be charged to any thread's CPU time.

### 1.1 Thread creation — `worker_create`

1. On the first call, `init_library()` builds the main TCB, the scheduler context
   and its stack, and installs the `SIGPROF` handler.
2. Allocate and zero a TCB (`calloc`), assign a unique id from a monotonically
   increasing counter (ids are never reused, so a stale id can never alias a live
   thread).
3. Allocate a 128 KB stack. The write-up suggests `SIGSTKSZ`, but on recent glibc
   that can be as small as 8 KB, which is not enough for benchmark code that calls
   `fscanf`, so a fixed 128 KB is used instead.
4. `getcontext` to seed the context, then point `uc_stack` at the new stack, set
   `uc_link` to the scheduler context as a fallback, and `makecontext` it to start
   at `worker_stub`.
5. Record `arrival_us` — the moment the thread is first put on a runqueue. Both
   response and turnaround time are measured from here.
6. Link the TCB into the global thread list and enqueue it on the runqueue.

Two details in step 4 are worth calling out.

**`makecontext` takes `int` arguments, which cannot portably carry a 64-bit
pointer.** Rather than splitting a pointer across two `int`s, every thread starts
at the same zero-argument `worker_stub`, which reads its own TCB out of the global
`current`. This is safe because the scheduler assigns `current` immediately before
it swaps into a thread, so when `worker_stub` begins executing, `current` is by
construction its own TCB.

**The new context inherits the creator's signal mask**, and `worker_create` runs
with `SIGPROF` blocked. Left alone, every worker would therefore start life
un-preemptible. `sigdelset(&t->ctx.uc_sigmask, SIGPROF)` fixes that explicitly.

`worker_stub` also means a thread body that simply returns, instead of calling
`worker_exit`, is still cleaned up correctly — the stub calls `worker_exit` with
the body's return value.

### 1.2 Thread yield — `worker_yield`

Block `SIGPROF`, mark the thread `READY`, disarm the timer, and
`swapcontext` into the scheduler. On the way back it unblocks `SIGPROF` and
returns. The timer is disarmed because the thread is giving up the CPU early; the
scheduler arms a fresh slice for whoever runs next.

### 1.3 Thread exit — `worker_exit`

1. Store `value_ptr` in the TCB and mark the thread `FINISHED`.
2. Charge the final sliver of CPU time, then fold this thread's turnaround and
   response times into the running averages and recompute `avg_turn_time` and
   `avg_resp_time`.
3. If a thread is blocked in `worker_join` on this one, make it runnable.
4. Push the TCB onto a `finished` list and `setcontext` into the scheduler.

Step 4 exists because **a thread cannot free the stack it is standing on.**
`free()`ing a 128 KB block hits glibc's mmap threshold, so the allocator may
unmap it outright — and unmapping your own stack mid-function is an immediate
segfault. So the exiting thread hands its TCB to the scheduler and the scheduler
frees the stack on its next pass, from its own stack. The TCB itself is kept alive
so that a later `worker_join` can still read the return value.

Averages are updated on exit rather than on first dispatch so that threads which
have started but not finished cannot skew them.

### 1.4 Thread join — `worker_join`

Look the target up by id. While it is not `FINISHED`, register as its joiner, mark
self `BLOCKED`, and switch to the scheduler; re-check the condition on every
wake-up rather than trusting it (a classic condition-variable discipline). Once the
target is finished, copy out its return value and free its TCB and stack.

The wake-up cannot be lost. If the target exits *before* `worker_join` is called,
it is already `FINISHED` and the loop never blocks. If it exits *after*, it finds
the registered joiner and wakes it. The window between the two is covered because
everything here runs with `SIGPROF` blocked, so no context switch can occur in
between.

### 1.5 Synchronization — `worker_mutex_*`

The mutex is `{ volatile int locked; int initialized; tcb *owner; tcb_queue waiters; }`.

**`worker_mutex_lock`** has a fast path and a slow path:

```c
/* fast: an uncontended test-and-set touches no scheduler state */
if (__sync_lock_test_and_set(&mutex->locked, 1) == 0) {
        mutex->owner = current;
        return 0;
}
/* slow: the wait list and the runqueues are now in play */
disable_preempt();
while (__sync_lock_test_and_set(&mutex->locked, 1) == 1) {
        current->status = BLOCKED;
        enqueue(&mutex->waiters, current);
        swapcontext(&current->ctx, &sched_ctx);
}
mutex->owner = current;
enable_preempt();
```

The split matters a great deal for performance. `disable_preempt` is a
`sigprocmask` system call, and the benchmarks take and release these mutexes
*millions* of times — `external_cal` performs roughly 10 million lock/unlock pairs.
Guarding the uncontended path would add four system calls per critical section for
no benefit, because an uncontended test-and-set is atomic by itself and touches no
shared scheduler state. (`mutex->owner` is informational only, so being preempted
between the test-and-set and the assignment is harmless.)

A contended waiter **blocks rather than spins.** With one kernel thread, spinning
cannot work: the holder can only make progress if the spinner gives the CPU back.
The `while` loop re-tests after every wake-up because the wake-all policy below
means several threads may race for one lock.

**`worker_mutex_unlock`** releases the lock *before* it looks for waiters, and the
order is load-bearing:

```c
mutex->owner = NULL;
__sync_lock_release(&mutex->locked);
if (mutex->waiters.head == NULL)        /* fast path, no syscall */
        return 0;
disable_preempt();
while ((t = dequeue(&mutex->waiters)) != NULL)
        make_runnable(t);
enable_preempt();
```

Checking the wait list first and releasing afterwards would lose wake-ups: a thread
preempted between "wait list is empty" and "release" could have a new waiter
enqueue itself in the meantime, and that waiter would then sleep forever on a free
mutex. In the order above, the worst case is a thread that finds the lock already
free and takes it without ever being queued — harmless. The remaining spurious case
is a woken thread that loses the race and re-blocks, which the retry loop handles.

Waiters are woken *all at once* and left to compete again, as the write-up
specifies, rather than handing the lock to a chosen successor.

**`worker_mutex_destroy`** releases the lock and wakes any remaining waiters first,
so that nothing is left blocked on a mutex that no longer exists.

### 1.6 Preemption

`ITIMER_PROF` is used, so the clock only advances while this process (or the kernel
on its behalf) is actually running — wall-clock timers give erratic slices when
threads make system calls. The handler is installed with `sigaction`, not `signal`,
and with `SA_RESTART` so that a blocking read inside `external_cal`'s `fscanf` is
resumed rather than failing with `EINTR`.

The handler itself does almost nothing:

```c
static void timer_handler(int signum) {
        if (!current || current->status != SCHEDULED)
                return;
        timer_armed     = 0;
        current->status = READY;
        swapcontext(&current->ctx, &sched_ctx);
}
```

Switching contexts out of a signal handler works because **`swapcontext` saves and
restores the signal mask along with the registers.** The three consequences are:

* `sched_ctx` was captured with `SIGPROF` added to its `uc_sigmask`, so the
  scheduler can never be interrupted by the quantum timer while it is manipulating
  the runqueues.
* The context saved into `current->ctx` has `SIGPROF` blocked, because the kernel
  blocks it for the duration of the handler.
* When that thread is later resumed, it comes back *inside the handler*, returns
  from it, and the kernel's normal signal-return path restores the pre-signal mask
  and resumes the interrupted instruction — restarting the interrupted system call
  if there was one.

Shared state (runqueues, the global TCB list, mutex wait lists) is protected by
blocking `SIGPROF` around library critical sections — the user-level equivalent of
disabling interrupts in a kernel. `enable_preempt` is called after every voluntary
`swapcontext` returns, because the context that was saved had the signal blocked.

**Timers are one-shot and re-armed on every dispatch**, rather than periodic. That
gives each MLFQ level its own slice length, and it means a thread that gives up the
CPU early is not immediately interrupted by the remainder of someone else's
quantum.

One further refinement: **the timer is only armed when another thread is actually
waiting for the CPU.** If the thread being dispatched is the only runnable one,
preempting it would accomplish nothing — there is nobody to switch to — so the
timer is left off and the thread runs until it blocks or exits. This removes a
large number of pointless context switches, most visibly during the benchmarks'
single-threaded `verify()` phase, which runs after the workers are gone but
*before* `print_app_stats()` and would otherwise inflate the reported switch count
by thousands. Correspondingly, `make_runnable` re-arms the timer if it wakes a
thread while the running thread is un-clocked, so a thread that never yields cannot
starve one it just woke.

### 1.7 The scheduler — `schedule()`

`schedule()` runs on its own context and is an infinite loop. Every entry point —
the `SIGPROF` handler, a voluntary `swapcontext` from yield/join/lock, and
`setcontext` from `worker_exit` — lands at the `swapcontext` at the bottom of that
loop, so the loop structure needs no special casing for how it was entered.

Each pass does four things:

1. **Charge the departing thread.** Add the elapsed microseconds to its `cpu_us`
   and recompute `elapsed = cpu_us / QUANTUM`. Under MLFQ, charge its level
   allotment and demote if exhausted. If it is `READY`, put it back on a runqueue;
   `BLOCKED` threads are already parked on a wait list and `FINISHED` ones are on
   the finished list, so neither is enqueued.
2. **Reap exited stacks** — safe here and only here, since this runs on the
   scheduler's own stack.
3. **Select** via `sched_psjf()` or `sched_mlfq()`, chosen at compile time by
   `#ifndef MLFQ`.
4. **Dispatch**: mark `SCHEDULED`, record first-run time (and hence response time)
   if this is its first turn, bump the context-switch counters, arm the timer, and
   `swapcontext` into it.

If nothing is runnable, the scheduler distinguishes two cases: if any thread is
`BLOCKED`, that is a genuine deadlock and it reports it on `stderr` and exits 1;
otherwise there is simply nothing left to run, which can only happen once `main`
itself has exited, so it exits 0.

### 1.8 PSJF / STCF

A user-level scheduler cannot know how long a thread will run. Following the
write-up, elapsed CPU time is used as the estimate — the longer a thread has
already run, the longer it is assumed to still need — which makes this behave like
a fair-share scheduler.

`sched_psjf` scans the single runqueue and removes the thread with the smallest
`elapsed`. The comparison is a strict `<`, so threads tied on elapsed time are
served in FIFO order, which keeps the policy stable and starvation-free among
equals. `elapsed` is derived from *measured* microseconds rather than from a count
of timer interrupts, which means a thread that yields after a fraction of a quantum
is charged only for the fraction it used, and a spurious wake-up costs it nothing.

Selection is O(n) in the number of runnable threads. With the 50–100 threads the
grading uses, a linear scan is cheaper than maintaining a heap, and it keeps the
queue code shared with MLFQ.

### 1.9 MLFQ

`NUM_LEVELS` (default 4) runqueues, level 0 highest priority. Level *i* gets a
slice of `(i + 1) * QUANTUM` ms, so **the higher the priority, the shorter the time
slice**, per OSTEP §8.5. New threads enter at level 0.

The five rules map onto the code as follows:

* **Rules 1–2** (run the highest-priority runnable thread; round-robin within a
  level): `sched_mlfq` scans levels top-down and dequeues the head.
* **Rules 3–4** (demotion): handled by `mlfq_charge`, which charges the CPU time a
  thread used to its *allotment at its current level* and demotes it only once that
  allotment is exhausted:

  ```c
  static void mlfq_charge(tcb *t, long ran) {
          long slice = slice_for(t);
          t->allot_us += ran;
          if (t->allot_us < slice) return;
          if (t->priority < NUM_LEVELS - 1) { t->priority++; t->allot_us = 0; }
          else                              { t->allot_us = slice; }
  }
  ```

  Charging *time* rather than counting timer interrupts is what implements OSTEP's
  modern rule 4 and defeats gaming the scheduler: giving the CPU up early preserves
  the unused part of the allotment but does not refund it, so a thread that yields
  just before every slice expires sinks at exactly the same rate as one that runs
  flat out. A thread that genuinely blocks early keeps its remaining allotment and
  stays at its level. The bottom level is a floor.
* **Rule 5** (periodic boost): every `BOOST_PERIOD` ms (default 100) every thread is
  moved back to level 0 and its allotment refunded — including blocked threads, so
  they return at the top when released. This is the only thing that refunds an
  allotment, and it is what bounds starvation at the bottom level.

Because MLFQ with a single level degenerates to round-robin, the queue primitives
are shared verbatim with PSJF; only the selection function differs.

### 1.10 Metrics

* **Response time** = first dispatch − enqueue at creation.
* **Turnaround time** = `worker_exit` − enqueue at creation.
* Both are accumulated per thread and averaged over completed *worker* threads only,
  and reported in **microseconds**, matching the unit of the `Total run time` line
  the benchmarks print. The write-up does not state a unit; `make STATS=MS` switches
  both to milliseconds via a one-line `STAT_DIVISOR` in `thread-worker.h`.
* **These two metrics are timestamped in nanoseconds**, not microseconds, and only
  converted to the reported unit at the end. Scheduling itself still runs on a
  microsecond clock, since a quantum is a whole number of milliseconds and there is
  nothing finer to resolve. The distinction matters because a short-lived thread can
  be created and finish inside a single microsecond tick: on a microsecond clock its
  arrival and finish timestamps are *equal*, so its turnaround is exactly 0 — and 0
  divided by any unit is still 0. That is the real cause of a reported turnaround of
  zero, and it is a resolution problem rather than a formatting one, so no choice of
  unit fixes it. With nanosecond timestamps a 400 ns thread reports 0.4 µs instead
  of 0.
* **`tot_cntx_switches`** counts dispatches — each time the scheduler puts a thread
  on the CPU — including the main thread's. Per-thread counts are also kept in each
  TCB.
* Timing uses `clock_gettime(CLOCK_MONOTONIC)`, which is async-signal-safe and so
  is also legal from the timer handler.

### 1.11 Known limitations

* **glibc re-entrancy.** With one kernel thread, a thread preempted inside `malloc`
  or inside a `FILE`'s internal lock can deadlock the process if another thread
  enters the same lock, since the blocking primitive underneath is a real futex. The
  benchmarks stay clear of this — the workers do not allocate, and `external_cal`
  gives each thread its own `FILE` — but it is a genuine limitation of user-level
  threading, not something the library can fix.
* `attr` and `mutexattr` are ignored, as the write-up permits.
* `worker_join` on an already-joined thread returns −1 rather than being undefined.
* The scheduler's PSJF selection is O(n) per dispatch.

---

## 2. Benchmark results

### Modifications to the provided benchmarks

Two changes were made on the course's instruction:

* **`Total run time` now really is microseconds.** All three benchmarks computed
  `(end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000`,
  which is milliseconds, under a label that said micro-seconds. All three now use
  `(end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000`.
  The fix was applied to every benchmark, not just one, so run times stay
  comparable between them.
* **`VECTOR_SIZE` raised from 3,000,000 to 30,000,000** in `vector_multiply.c`, so
  the threads run long enough to produce non-zero turnaround times. (The underlying
  cause of zero turnaround was clock resolution, addressed separately in §1.10 by
  timestamping the metrics in nanoseconds; a larger vector raises the signal as
  well, and is what the course asked for.)

The second change has two consequences worth noting when reading the numbers.
`r[]` and `s[]` together now occupy about 229 MB of BSS, and the benchmark performs
30 million mutex lock/unlock pairs rather than 3 million — which makes
`vector_multiply` by far the heaviest test of mutex cost, and the clearest place to
see the gap against native pthreads. The reported sum still overflows `int`, but
`verify()` overflows identically, so the correctness check remains exact.

### Collecting the numbers

Collected with `benchmarks/run_benchmarks.sh`, which sweeps both policies, several
quantum values and thread counts, and then re-runs everything against the native
`pthread` library as a baseline:

```bash
cd code/benchmarks
./genRecord.sh          # once, generates the data external_cal reads
./run_benchmarks.sh     # or -q for a quick sweep
```

It writes `results.csv` (one row per run) and `results.md` (tables formatted for
this report).

> **Paste the contents of `results.md` here.** Correctness check: for every
> configuration, "Total sum is" must equal the value `verify()` computes —
> `parallel_cal` and `vector_multiply` are deterministic, so any variation means a
> missed mutex.

_(tables go here)_

---

## 3. Analysis

_Fill this in from your own numbers; the questions below are the ones the measured
data should answer, and the expectations are what the design above predicts._

**Correctness.** Does every configuration reproduce the verified sum, at 1, 50 and
100 threads, under both policies and every quantum?

**Quantum sensitivity.** A shorter quantum should raise the context-switch count
and improve average response time while leaving turnaround roughly flat until
switching overhead starts to dominate. Does the total run time rise at the smallest
quantum? Where is the crossover?

**PSJF vs MLFQ.** The benchmark workers are near-identical CPU-bound loops, so PSJF
should keep them in lockstep (they accumulate CPU time at the same rate, so the
minimum-elapsed thread keeps rotating) and MLFQ should look similar once everyone
has sunk to the bottom level, with the periodic boost showing up as a burst of
switches every `BOOST_PERIOD` ms. Does `external_cal`, the I/O-bound benchmark,
separate the two policies more clearly, since threads that block keep their
allotment and stay near the top under MLFQ?

**Thread count.** Turnaround should grow roughly linearly with the number of
threads (the same total work split more ways, each thread finishing later), while
response time should grow with queueing delay. Is that what the data shows at
50–100 threads?

**Comparison with Linux pthreads.** Expect the native library to win on wall-clock
time, for reasons the design makes explicit: it has real kernel-level parallelism
across cores, whereas this library multiplexes everything onto one kernel thread —
so on an *n*-core iLab machine the ceiling is roughly 1/*n* of pthread throughput
on CPU-bound work. Against that, a user-level context switch here is a
`swapcontext` (one `sigprocmask` plus a register save) with no kernel scheduler
involvement, and an uncontended mutex is a single atomic instruction. Which effect
dominates in your measurements, and does the gap widen or narrow as the thread
count rises?

**MLFQ's `S`.** Re-run with `BOOST_PERIOD` set to a few different values (override
it in `thread-worker.h` or with `-DBOOST_PERIOD=`). A very small `S` collapses MLFQ
toward round-robin; a very large one lets low-priority threads languish. Report what
you see.

**Mutex cost at 30 million operations.** `vector_multiply` locks and unlocks once
per element, so at `VECTOR_SIZE = 30000000` it is essentially a microbenchmark of
mutex overhead. Compare its run time against `parallel_cal`, which does the same
kind of arithmetic but takes the lock only once per row: the difference is what this
library's synchronization costs. How does that compare with the native pthread
baseline, where an uncontended lock is also a single atomic but contention drops
into a futex rather than this scheduler?

---

## 4. Collaboration and references

**People.** _(List any partner and the division of work; state "none" if you worked
alone.)_

**AI assistance.** This implementation and this report were developed with
Anthropic's Claude (Claude Code). The nature of the use was: _(describe accurately —
e.g. the library design and the code in `thread-worker.c`/`.h`, the smoke test in
`benchmarks/test.c`, the sweep script, and the design write-up above were produced
with Claude; the benchmark runs, the numbers in section 2 and the analysis in
section 3 are my own.)_ **Check your course's AI policy and amend this section so it
states exactly what you did and what was assisted.**

**Documentation consulted.**

* `makecontext(3)`, `swapcontext(3)`, `getcontext(3)`, `setitimer(2)`,
  `sigaction(2)`, `sigprocmask(2)`, `clock_gettime(2)` — Linux man pages.
* GCC manual, "Legacy `__sync` built-ins for atomic memory access", for
  `__sync_lock_test_and_set` / `__sync_lock_release`.
* *Operating Systems: Three Easy Pieces*, chapter 8 (MLFQ) — rules 1–5 and the
  priority-boost discussion in §8.5–8.6; chapter 7 for STCF.
* The Project 2 write-up and the provided `sample-code/` programs.

_(Add anything else you consulted.)_
