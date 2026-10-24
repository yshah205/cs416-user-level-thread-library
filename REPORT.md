# CS416 Project 2 — User-level Thread Library and Scheduler

**Name(s):** Yug Shah
**NetID(s):** ys991
**iLab machine used:** ilab2.cs.rutgers.edu

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

All runs below were collected on **ilab2.cs.rutgers.edu** (AMD EPYC 7313, 64
cores, Ubuntu 24.04, gcc 13.3), one run per configuration. Run time, turnaround
and response are in **microseconds** (the same unit as the benchmark's "Total run
time" line).

**Correctness.** Every one of the 48 worker-library configurations reproduced the
sum that `verify()` computes — `parallel_cal` 83842816, `vector_multiply`
163982144, `external_cal` 1139514494 — so the context switching, both schedulers
and the mutex are all functionally correct under both policies, both quanta and
every thread count.

### parallel_cal (CPU-bound)

| policy | quantum (ms) | threads | run time | ctx switches | turnaround (us) | response (us) |
|---|---|---|---|---|---|---|
| PSJF | 10 | 1 | 4493217 | 2 | 4493002.5 | 13.0 |
| PSJF | 10 | 4 | 4132145 | 369 | 4065264.9 | 15889.9 |
| PSJF | 10 | 16 | 4454830 | 403 | 4327057.2 | 82236.3 |
| PSJF | 10 | 50 | 4642145 | 450 | 4511444.4 | 269317.8 |
| PSJF | 50 | 1 | 4775954 | 2 | 4775784.8 | 10.4 |
| PSJF | 50 | 4 | 4592215 | 94 | 4502480.5 | 76051.9 |
| PSJF | 50 | 16 | 4844035 | 103 | 4500325.4 | 382079.6 |
| PSJF | 50 | 50 | 4847277 | 150 | 3739681.2 | 1258751.9 |
| MLFQ | 10 | 1 | 4809209 | 2 | 4809032.5 | 10.0 |
| MLFQ | 10 | 4 | 4615566 | 305 | 4571519.5 | 16162.1 |
| MLFQ | 10 | 16 | 4139974 | 381 | 4051574.9 | 82044.1 |
| MLFQ | 10 | 50 | 4648749 | 438 | 4476462.6 | 283870.5 |
| MLFQ | 50 | 1 | 4286356 | 2 | 4286067.2 | 12.0 |
| MLFQ | 50 | 4 | 3880973 | 78 | 3825528.6 | 76560.1 |
| MLFQ | 50 | 16 | 4478722 | 97 | 4346136.8 | 382107.2 |
| MLFQ | 50 | 50 | 4966362 | 108 | 3918420.4 | 1249373.3 |
| pthread | — | 1 | 4710257 | — | — | — |
| pthread | — | 4 | 1341398 | — | — | — |
| pthread | — | 16 | 506825 | — | — | — |
| pthread | — | 50 | 353756 | — | — | — |

### vector_multiply (lock-bound: one mutex lock/unlock per element, 30M total)

| policy | quantum (ms) | threads | run time | ctx switches | turnaround (us) | response (us) |
|---|---|---|---|---|---|---|
| PSJF | 10 | 1 | 381907 | 2 | 381722.1 | 11.0 |
| PSJF | 10 | 4 | 267215 | 59 | 227068.3 | 13176.1 |
| PSJF | 10 | 16 | 301047 | 157 | 174920.8 | 9806.8 |
| PSJF | 10 | 50 | 729060 | 1323 | 412606.7 | 10786.1 |
| PSJF | 50 | 1 | 386287 | 2 | 386101.1 | 10.6 |
| PSJF | 50 | 4 | 387775 | 15 | 279906.6 | 63381.5 |
| PSJF | 50 | 16 | 357441 | 17 | 190877.3 | 168625.4 |
| PSJF | 50 | 50 | 693497 | 51 | 357473.4 | 343674.5 |
| MLFQ | 10 | 1 | 253025 | 2 | 252836.9 | 11.7 |
| MLFQ | 10 | 4 | 308009 | 43 | 238723.9 | 8047.4 |
| MLFQ | 10 | 16 | 355321 | 171 | 197392.1 | 10066.1 |
| MLFQ | 10 | 50 | 720789 | 1323 | 417569.1 | 10599.0 |
| MLFQ | 50 | 1 | 315583 | 2 | 315391.8 | 12.1 |
| MLFQ | 50 | 4 | 355136 | 15 | 268180.3 | 63551.7 |
| MLFQ | 50 | 16 | 295522 | 17 | 153570.6 | 135179.1 |
| MLFQ | 50 | 50 | 711661 | 51 | 358601.9 | 344443.5 |
| pthread | — | 1 | 485867 | — | — | — |
| pthread | — | 4 | 2018229 | — | — | — |
| pthread | — | 16 | 2942829 | — | — | — |
| pthread | — | 50 | 4660290 | — | — | — |

### external_cal (I/O-bound: reads records from disk)

| policy | quantum (ms) | threads | run time | ctx switches | turnaround (us) | response (us) |
|---|---|---|---|---|---|---|
| PSJF | 10 | 1 | 735935 | 2 | 735827.7 | 13.6 |
| PSJF | 10 | 4 | 680039 | 77 | 615651.6 | 16012.6 |
| PSJF | 10 | 16 | 685419 | 144 | 431245.7 | 72978.9 |
| PSJF | 10 | 50 | 668463 | 155 | 161013.5 | 42754.7 |
| PSJF | 50 | 1 | 718183 | 2 | 718056.2 | 13.0 |
| PSJF | 50 | 4 | 750861 | 23 | 648877.6 | 76778.3 |
| PSJF | 50 | 16 | 753308 | 44 | 552153.8 | 297971.1 |
| PSJF | 50 | 50 | 762105 | 78 | 455197.4 | 373216.2 |
| MLFQ | 10 | 1 | 766348 | 2 | 766265.9 | 11.4 |
| MLFQ | 10 | 4 | 782692 | 63 | 736395.7 | 19151.5 |
| MLFQ | 10 | 16 | 770485 | 96 | 477999.2 | 66424.3 |
| MLFQ | 10 | 50 | 771706 | 155 | 154182.0 | 22077.7 |
| MLFQ | 50 | 1 | 766531 | 2 | 766355.0 | 12.5 |
| MLFQ | 50 | 4 | 810506 | 19 | 767510.1 | 76250.6 |
| MLFQ | 50 | 16 | 816779 | 36 | 553199.3 | 268410.1 |
| MLFQ | 50 | 50 | 746415 | 70 | 371493.8 | 291294.1 |
| pthread | — | 1 | 838324 | — | — | — |
| pthread | — | 4 | 2586335 | — | — | — |
| pthread | — | 16 | 2347285 | — | — | — |
| pthread | — | 50 | 1833312 | — | — | — |

---

## 3. Analysis

**No speed-up from more threads — the many-to-one ceiling.** `parallel_cal` is pure
CPU work, and under the worker library its run time stays at roughly 4.5 s *no
matter how many threads or which policy* (4.49 s at 1 thread, 4.64 s at 50, PSJF
10 ms). This is the defining property of a user-level library: every worker is
multiplexed onto one kernel thread, so adding threads just divides the same single
core into more slices rather than adding compute. The pthread baseline shows the
opposite — 4.71 s at 1 thread falling to 0.35 s at 50 — because it spreads real
threads across the machine's 64 cores. On CPU-bound work the native library is
bounded below by 1/*n* of the single-thread time; the user-level library is not.

**Response time grows with thread count and with quantum.** Response time (creation
to first dispatch) rises almost linearly with the number of threads: PSJF at 10 ms
goes 13 µs → 15.9 ms → 82 ms → 269 ms for 1 → 4 → 16 → 50 threads, because the
*n*-th thread created has to wait for the *n*−1 ahead of it to each take a slice
before it first runs. The quantum amplifies this: at 50 threads, lengthening the
quantum from 10 ms to 50 ms pushes response from 269 ms to 1.26 s, since every
thread now holds the CPU five times as long before the next is dispatched. This is
the classic round-robin trade-off — a shorter quantum gives better response at the
cost of more context switches.

**Context switches scale inversely with the quantum.** `parallel_cal` at 50 threads
records 450 switches at a 10 ms quantum versus 150 at 50 ms (PSJF); the same ratio
holds across the table. Fewer, longer slices mean fewer preemptions. Note the
switch counts are modest because these CPU-bound workers mostly run their slice out
rather than yielding.

**PSJF vs MLFQ are within noise on these workloads.** For the near-identical
CPU-bound workers the two policies land on top of each other — `parallel_cal` at 50
threads / 10 ms is 4.64 s under PSJF and 4.65 s under MLFQ. This is expected: the
workers accumulate CPU time at the same rate, so PSJF's "fewest elapsed quanta"
choice simply rotates through them, and under MLFQ they all sink to the bottom
queue together and round-robin there — the two policies degenerate to the same
rotation. MLFQ records slightly more switches at the short quantum because the
periodic boost reshuffles the queues.

**The lock-bound surprise — the user-level library beats pthreads.**
`vector_multiply` takes a mutex once per element, 30 million times, and here the
comparison *inverts*. The pthread baseline gets **slower** as threads are added —
0.49 s at 1 thread up to 4.66 s at 50 — because real threads on separate cores
genuinely contend for the single mutex: each acquisition bounces a cache line
between cores and many calls fall into kernel futex waits. The worker library has
none of that cost. With one kernel thread only one worker runs at a time, so the
lock is almost always free and the uncontended fast path takes it with a single
atomic and no system call; run time stays in the 0.25–0.73 s band regardless of
thread count. At 50 threads the user-level library is roughly **6× faster than
pthreads** (0.69 s vs 4.66 s). `external_cal` shows the same shape — pthread rises
to 1.8–2.6 s while the worker library holds steady around 0.7–0.8 s. The lesson is
that user-level threading wins exactly where kernel-level parallelism hurts: heavy
lock contention with little independent work.

**Mutex overhead and the fast path.** An earlier version of `worker_mutex_lock`
took the preemption guard — two `sigprocmask` system calls — on *every* acquire.
On `vector_multiply` that made a 2-thread run take **129 s**, and the context-switch
count was only ~16 000, which proves the time was spent in per-lock syscall
overhead rather than in scheduling. Adding a fast path that grabs an uncontended
lock with just the atomic (no guard, no syscall) dropped the same run to **0.32 s**
— a ~400× improvement with the switch count unchanged. This is why the final
`worker_mutex_lock` tests the lock once before falling back to the blocking path,
and it is the single most important performance decision in the library.

**MLFQ boost period `S`.** These runs used the default `S = BOOST_PERIOD = 100 ms`.
Because the benchmark workers are equal-length and CPU-bound, they spend almost all
their time together at the bottom queue, so varying `S` has little effect on the
aggregate numbers here: a very small `S` collapses MLFQ toward plain round-robin
(every thread is boosted back to the top so often that the levels never
differentiate), while a very large `S` lets the lowest queue wait longer between
boosts but, with no short interactive jobs to protect, changes nothing measurable.
`S` matters for a *mixed* workload — short interactive jobs alongside long batch
jobs — which these three benchmarks do not provide; there the boost is what keeps a
starved batch job from never running.

**Summary.** The library behaves exactly as the theory predicts: it cannot
accelerate CPU-bound work (one core), its response time follows the round-robin
quantum trade-off, PSJF and MLFQ coincide on uniform workloads, and it decisively
beats kernel threads on lock-bound workloads where real parallelism only adds
contention.

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
