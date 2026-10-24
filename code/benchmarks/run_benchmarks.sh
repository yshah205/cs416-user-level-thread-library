#!/bin/bash
# Collect the numbers the project report asks for.
#
#   ./run_benchmarks.sh                 # full sweep (takes a while)
#   ./run_benchmarks.sh -q              # quick sweep, fewer configurations
#   ./run_benchmarks.sh -t "1 4 16"     # specific thread counts
#
# Writes results.csv (one row per run) and results.md (tables ready to paste
# into the report).  Run it from the benchmarks directory.

set -u

LIB_DIR=".."
HDR="$LIB_DIR/thread-worker.h"
BENCHES="parallel_cal vector_multiply external_cal"
THREADS="1 2 4 8 16 32 50 100"
QUANTA="5 10 25 50"
REPEATS=3

while getopts "qt:r:" opt; do
	case $opt in
		q) THREADS="1 4 16 50"; QUANTA="10 50"; REPEATS=1 ;;
		t) THREADS="$OPTARG" ;;
		r) REPEATS="$OPTARG" ;;
		*) echo "usage: $0 [-q] [-t \"1 4 16\"] [-r repeats]"; exit 1 ;;
	esac
done

if [ ! -f "$HDR" ]; then
	echo "error: run this from the benchmarks directory" >&2
	exit 1
fi

# external_cal needs its data files; build() regenerates them after each
# make (make clean deletes record/), so nothing to do here

cp "$HDR" "$HDR.bak"
restore() { mv -f "$HDR.bak" "$HDR" 2>/dev/null; }
trap restore EXIT

use_workers() {   # on|off
	if [ "$1" = on ]; then
		sed -i 's|^//#define USE_WORKERS 1|#define USE_WORKERS 1|' "$HDR"
	else
		sed -i 's|^#define USE_WORKERS 1|//#define USE_WORKERS 1|' "$HDR"
	fi
}

build() {         # build <SCHED> <QUANTUM>
	( cd "$LIB_DIR" && make clean >/dev/null 2>&1 \
	  && make SCHED="$1" QUANTUM="$2" >/dev/null 2>&1 ) || return 1
	make clean >/dev/null 2>&1
	make >/dev/null 2>&1
	# the benchmark "make" runs clean, which deletes record/, so (re)generate
	# the external_cal data after building, not before
	[ -d ./record ] || ./genRecord.sh >/dev/null 2>&1
}

CSV=results.csv
echo "policy,quantum_ms,benchmark,threads,run,runtime_ms,sum,cntx_switches,turnaround_ms,response_ms" > $CSV

run_one() {       # run_one <policy> <quantum> <bench> <threads> <run#>
	local pol=$1 q=$2 b=$3 n=$4 r=$5 out rt sum cs ta rs

	out=$(./"$b" "$n" 2>&1)
	rt=$(echo  "$out" | sed -n 's/.*Total run time: *\([0-9]*\).*/\1/p'         | head -1)
	sum=$(echo "$out" | sed -n 's/.*Total sum is: *\(-*[0-9]*\).*/\1/p'         | head -1)
	cs=$(echo  "$out" | sed -n 's/.*Total context switches *\([0-9]*\).*/\1/p'  | head -1)
	ta=$(echo  "$out" | sed -n 's/.*Average turnaround time *\([0-9.]*\).*/\1/p'| head -1)
	rs=$(echo  "$out" | sed -n 's/.*Average response time *\([0-9.]*\).*/\1/p'  | head -1)

	echo "$pol,$q,$b,$n,$r,${rt:-NA},${sum:-NA},${cs:-NA},${ta:-NA},${rs:-NA}" >> $CSV
	printf "    %-16s threads=%-4s run=%s  time=%-8s switches=%-8s turn=%-10s resp=%s\n" \
	       "$b" "$n" "$r" "${rt:-NA}" "${cs:-NA}" "${ta:-NA}" "${rs:-NA}"
}

# ---- the worker library, both policies, several quanta --------------------
use_workers on
for pol in PSJF MLFQ; do
	for q in $QUANTA; do
		echo "== $pol, QUANTUM=${q}ms =="
		if ! build "$pol" "$q"; then
			echo "    BUILD FAILED" >&2
			continue
		fi
		for b in $BENCHES; do
			for n in $THREADS; do
				for r in $(seq 1 "$REPEATS"); do
					run_one "$pol" "$q" "$b" "$n" "$r"
				done
			done
		done
	done
done

# ---- the Linux pthread baseline ------------------------------------------
echo "== Linux pthread baseline =="
use_workers off
if build PSJF 10; then
	for b in $BENCHES; do
		for n in $THREADS; do
			for r in $(seq 1 "$REPEATS"); do
				run_one pthread NA "$b" "$n" "$r"
			done
		done
	done
fi
use_workers on
restore
trap - EXIT

# ---- summarize -----------------------------------------------------------
awk -F, -v REPEATS="$REPEATS" '
NR == 1 { next }
{
	k = $1 "," $2 "," $3 "," $4
	if (!(k in seen)) { seen[k] = 1; korder[++nk] = k }
	if (!($3 in seen_b)) { seen_b[$3] = 1; border[++nb] = $3 }
	if ($6  != "NA") { tn[k]++;  t[k]  += $6  }
	if ($8  != "NA") { cn[k]++;  c[k]  += $8  }
	if ($9  != "NA") { an[k]++;  a[k]  += $9  }
	if ($10 != "NA") { rn[k]++;  r[k]  += $10 }
	if ($7  != "NA") { sum[k] = $7 }
}
function fmt(v, n, prec) {
	if (n == 0) return "-"
	return sprintf("%." prec "f", v / n)
}
END {
	print "# Benchmark results"
	print ""
	printf "Mean of %s run(s) per configuration. Run time is as the benchmark\n", REPEATS
	print  "prints it; turnaround and response are in milliseconds."
	for (i = 1; i <= nb; i++) {
		b = border[i]
		print ""
		print "## " b
		print ""
		print "| policy | quantum (ms) | threads | run time | context switches | turnaround (ms) | response (ms) |"
		print "|---|---|---|---|---|---|---|"
		for (j = 1; j <= nk; j++) {
			k = korder[j]
			split(k, f, ",")
			if (f[3] != b) continue
			printf "| %s | %s | %s | %s | %s | %s | %s |\n", f[1], f[2], f[4],
			       fmt(t[k], tn[k], 0), fmt(c[k], cn[k], 0),
			       fmt(a[k], an[k], 3), fmt(r[k], rn[k], 3)
		}
	}
}' $CSV | tee results.md >/dev/null

echo
echo "wrote $CSV and results.md"
