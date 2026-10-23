CC = gcc
CFLAGS = -g -c -Wall
AR = ar -rc
RANLIB = ranlib

# Scheduling policy: PSJF (the default, also known as STCF) or MLFQ.
#   make              -> PSJF
#   make SCHED=PSJF   -> PSJF
#   make SCHED=MLFQ   -> MLFQ
# Override the quantum (in milliseconds) with e.g.  make QUANTUM=25
SCHED ?= PSJF

ifdef QUANTUM
QFLAG = -DQUANTUM=$(QUANTUM)
endif

# Turnaround/response are reported in microseconds by default; report them in
# milliseconds instead with:  make STATS=MS
ifeq ($(STATS), MS)
QFLAG += -DSTATS_IN_MS
endif

all: clean thread-worker.a

thread-worker.a: thread-worker.o
	$(AR) libthread-worker.a thread-worker.o
	$(RANLIB) libthread-worker.a

thread-worker.o: thread-worker.h thread-worker.c

ifeq ($(SCHED), PSJF)
	$(CC) -pthread $(CFLAGS) $(QFLAG) -DPSJF thread-worker.c
else ifeq ($(SCHED), MLFQ)
	$(CC) -pthread $(CFLAGS) $(QFLAG) -DMLFQ thread-worker.c
else
	@echo "no such scheduling algorithm: SCHED=$(SCHED) (use PSJF or MLFQ)"; exit 1
endif

clean:
	rm -rf testfile *.o *.a
