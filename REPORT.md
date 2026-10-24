# CS416 Project 2 — User-level Thread Library and Scheduler

Name: Yug Shah
NetID: ys991
iLab machine: ilab2.cs.rutgers.edu

## 1. Implementation logic

The library runs all worker threads on a single kernel thread. Each worker has
its own `ucontext_t` and a 128 KB stack. There is one extra context for the
scheduler and one TCB for the benchmark's `main` thread, so `main` can be
descheduled like any other thread. A `SIGPROF` interval timer drives preemption.

**worker_create.** The first call initializes the library: it allocates the
scheduler's context and stack, installs the `SIGPROF` handler with `sigaction`,
and builds a TCB for the currently running `main` thread. For each new thread it
allocates a TCB and a stack, builds a context with `getcontext`/`makecontext`
that starts in a small wrapper function, records the thread's arrival time, and
adds it to the run queue. The wrapper calls the thread's function and then calls
`worker_exit`, so a thread that just returns is still cleaned up. `SIGPROF` is
blocked while the shared run queue is touched.

**worker_yield.** Sets the thread's state to READY and switches to the scheduler
context with `swapcontext`, giving up the CPU voluntarily. The scheduler then
picks the next thread.

**worker_exit.** Sets the thread's state to FINISHED, records its finish time,
updates the running average turnaround and response times, and wakes any thread
that was waiting on it in `worker_join`. Because a thread cannot free the stack
it is currently running on, it hands its TCB to the scheduler, which frees the
stack on its next pass, then switches to the scheduler and does not return.

**worker_join.** Looks up the target thread. While the target is not FINISHED,
the caller marks itself BLOCKED, registers as the target's joiner, and switches
to the scheduler; it is woken when the target exits. It then copies out the
return value (if requested) and frees the target's TCB and stack.

**Mutex.** `worker_mutex_init` initializes the fields. `worker_mutex_lock` uses
the atomic `__sync_lock_test_and_set`: if the lock is free it is taken
immediately; if it is held, the thread is put on the mutex's wait list, marked
BLOCKED, and switched out, retrying when woken. Blocking (rather than spinning)
is required because with one kernel thread a spinner can never let the holder
run. `worker_mutex_unlock` releases the lock and moves all waiters back to the
run queue so they compete for it again. `worker_mutex_destroy` wakes any
remaining waiters and resets the mutex.

**Scheduler.** `schedule()` runs on its own context in an infinite loop. On each
pass it charges the thread that just left the CPU with the time it ran, puts it
back on the run queue if it is still READY, frees the stacks of any exited
threads, picks the next thread according to the policy, records its first-run
time (response time) if this is its first turn, counts the context switch, arms
the timer for one quantum, and switches to it. If nothing is runnable it reports
a deadlock if any thread is blocked, otherwise it exits.

**PSJF.** Since the scheduler cannot know how long a thread will run, it
estimates the shortest job as the one that has used the fewest whole quanta of
CPU so far. It scans the run queue, picks the thread with the smallest elapsed
count, and runs it; ties are broken in FIFO order.

**MLFQ.** Uses four priority levels; a higher-priority level gets a shorter time
slice (`(level+1) * QUANTUM`). New threads start at the top level. The scheduler
always runs a thread from the highest non-empty level, round-robin within a
level. A thread is charged for the CPU time it uses at its current level and is
demoted one level once it exceeds that level's allotment, so yielding early does
not let a thread avoid demotion. Every `BOOST_PERIOD` (100 ms) all threads are
moved back to the top level so nothing starves.

**Preemption and metrics.** A one-shot `ITIMER_PROF` timer is re-armed on every
dispatch and sends `SIGPROF` when the quantum expires; the handler switches to
the scheduler. `SIGPROF` is blocked around all accesses to shared scheduler
state. Turnaround time is finish minus arrival, response time is first-dispatch
minus arrival, both averaged over the worker threads and reported in
microseconds; the total context-switch count is incremented on every dispatch.

## 2. Benchmark results

Collected on ilab2.cs.rutgers.edu (AMD EPYC 7313, Ubuntu 24.04, gcc 13.3), one
run per configuration, at two quantum values and under both policies, plus the
default Linux pthread library as a baseline. Run time, turnaround and response
are in microseconds. Every configuration produced the correct verified sum
(`parallel_cal` 83842816, `vector_multiply` 163982144, `external_cal`
1139514494).

Two provided benchmarks were changed as instructed: all three had their run-time
formula corrected to microseconds, and `vector_multiply`'s `VECTOR_SIZE` was
raised from 3,000,000 to 30,000,000 so threads run long enough to show a non-zero
turnaround time.

### parallel_cal

| policy | quantum (ms) | threads | run time | ctx switches | turnaround | response |
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

### vector_multiply

| policy | quantum (ms) | threads | run time | ctx switches | turnaround | response |
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

### external_cal

| policy | quantum (ms) | threads | run time | ctx switches | turnaround | response |
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

## 3. Analysis

On the CPU-bound `parallel_cal`, run time stays around 4.5 s no matter how many
threads are used, under both policies. This is expected: all worker threads share
one kernel thread, so adding threads only splits one core into more slices rather
than adding compute. The pthread baseline instead drops from 4.7 s at 1 thread to
0.35 s at 50 threads, because it runs on multiple real cores.

Response time grows with both the thread count and the quantum. More threads mean
a new thread waits behind more others before its first turn, and a longer quantum
means each of those turns lasts longer — at 50 threads, raising the quantum from
10 ms to 50 ms increases response time from about 0.27 s to 1.26 s. A shorter
quantum also produces more context switches (450 vs 150 at 50 threads), since
threads are preempted more often.

PSJF and MLFQ give very similar results on these benchmarks because the worker
threads are near-identical: they use CPU at the same rate, so PSJF keeps rotating
through them and MLFQ sinks them all to the bottom level and round-robins there.

The lock-heavy `vector_multiply` shows the most interesting comparison. The
pthread baseline gets slower as threads are added (0.49 s to 4.66 s), because real
threads on different cores genuinely contend for the single mutex. The user-level
library does not have this problem — only one thread runs at a time, so the lock
is almost always free and is taken with a single atomic — and its run time stays
around 0.3–0.7 s, making it roughly 6× faster than pthreads at 50 threads.
`external_cal` behaves the same way, with pthread slower than the worker library.
So the user-level library loses on independent CPU-bound work but wins on
lock-heavy work where real parallelism only adds contention.

## 4. Collaboration and references

People: _(list any partner and NetID, or "none")_

References: OSTEP (Operating Systems: Three Easy Pieces) chapters 7–10; the Linux
man pages for `makecontext`, `swapcontext`, `setitimer`, and `sigaction`; the
Project 2 write-up and provided sample code.

AI assistance: _(state what you used, per the course policy)_
