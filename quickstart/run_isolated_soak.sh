#!/usr/bin/env bash
# run_isolated_soak.sh -- 30-minute pinned latency soak for AnimusCore (sandbox_quickstart.cpp).
#
#   1. resolves the producer/consumer cores (node.env written by setup_baremetal_soak.sh, or --cores)
#   2. refuses SMT-sibling pairs; checks governor / hypervisor / boot isolation and records the result
#   3. compiles sandbox_quickstart.cpp (-O3 -march=native -std=c++17 -pthread) on the housekeeping CPUs
#   4. runs a short smoke pass, then the soak, with the process affinity restricted to producer,consumer
#      and one housekeeping CPU (taskset) and the two hot threads pinned inside the binary
#   5. logs p50 / p90 / p99 / p99.9 / max per interval (percentiles.csv) and overall
#   6. gates: zero hot-path malloc/free (interposer self-tested), zero sequence/payload/TSC errors,
#      pinning verified, optional p99 / p99.9 ceilings
#
# Usage:  ./run_isolated_soak.sh [options]          (root not required; run setup_baremetal_soak.sh first)
#   --duration-s N          soak length in seconds                    (default 1800)
#   --interval-ns N         producer pacing, ns per message           (default 1000)
#   --report-every-s N      interval report period                    (default 60)
#   --cores P,C             override node.env
#   --housekeeping-cpu N    override node.env
#   --out-dir DIR           default ./soak_runs/<UTC timestamp>
#   --state-dir DIR         where node.env lives (default /var/run/animus_soak)
#   --cxx PATH              compiler (default: $CXX, g++, clang++)
#   --max-p99-ns N          fail the run if overall p99 exceeds N
#   --max-p999-ns N         fail the run if overall p99.9 exceeds N
#   --allow-siblings        permit P and C on one physical core (measures same-core handoff only)
#   --strict                treat governor != performance, hypervisor, and missing boot isolation as failures
#   -h, --help
#
# Exit status: 0 = all gates passed, 1 = a gate failed, 2 = usage / environment error.
# The output directory contains host CPU/kernel details; do not commit it to a public repository.

set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC="$HERE/sandbox_quickstart.cpp"
STATE_DIR=/var/run/animus_soak
DURATION=1800
INTERVAL_NS=1000
REPORT_S=60
CORES_ARG=""
HK_ARG=""
OUT_DIR=""
CXX_BIN=${CXX:-}
MAX_P99=""
MAX_P999=""
ALLOW_SIB=0
STRICT=0

while (( $# )); do
    case "$1" in
        --duration-s)       DURATION=${2:?}; shift 2 ;;
        --interval-ns)      INTERVAL_NS=${2:?}; shift 2 ;;
        --report-every-s)   REPORT_S=${2:?}; shift 2 ;;
        --cores)            CORES_ARG=${2:?}; shift 2 ;;
        --housekeeping-cpu) HK_ARG=${2:?}; shift 2 ;;
        --out-dir)          OUT_DIR=${2:?}; shift 2 ;;
        --state-dir)        STATE_DIR=${2:?}; shift 2 ;;
        --cxx)              CXX_BIN=${2:?}; shift 2 ;;
        --max-p99-ns)       MAX_P99=${2:?}; shift 2 ;;
        --max-p999-ns)      MAX_P999=${2:?}; shift 2 ;;
        --allow-siblings)   ALLOW_SIB=1; shift ;;
        --strict)           STRICT=1; shift ;;
        -h|--help)          sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

die()  { printf 'error: %s\n' "$*" >&2; exit 2; }
note() { printf '[soak] %s\n' "$*"; }
GATE_FAILS=0
WARNS=0
gate() {   # gate NAME 0|1 DETAIL   (1 = pass)
    if (( $2 )); then printf '  [PASS] %-34s %s\n' "$1" "$3"
    else printf '  [FAIL] %-34s %s\n' "$1" "$3"; GATE_FAILS=$((GATE_FAILS + 1)); fi
}
soft() {   # soft NAME OK DETAIL : warning, or failure under --strict
    if (( $2 )); then printf '  [PASS] %-34s %s\n' "$1" "$3"
    elif (( STRICT )); then printf '  [FAIL] %-34s %s\n' "$1" "$3"; GATE_FAILS=$((GATE_FAILS + 1))
    else printf '  [WARN] %-34s %s\n' "$1" "$3"; WARNS=$((WARNS + 1)); fi
}

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die "Linux x86-64 only"
[[ -f $SRC ]] || die "missing $SRC"
[[ $DURATION =~ ^[0-9]+$ && $DURATION -ge 10 ]] || die "--duration-s must be an integer >= 10"
[[ $INTERVAL_NS =~ ^[0-9]+$ ]] || die "--interval-ns must be an integer"
[[ $REPORT_S =~ ^[0-9]+$ ]] || die "--report-every-s must be an integer"

expand_list() {
    local IFS=, part
    for part in $1; do
        [[ -z $part ]] && continue
        if [[ $part == *-* ]]; then seq "${part%-*}" "${part#*-}"; else echo "$part"; fi
    done
}
core_key() {
    local d=/sys/devices/system/cpu/cpu$1/topology
    local p c
    p=$(cat "$d/physical_package_id" 2>/dev/null || echo -1); c=$(cat "$d/core_id" 2>/dev/null || echo -1)
    if (( p < 0 || c < 0 )); then echo "unknown-$1"; else echo "$p:$c"; fi
}
in_list() { local x; for x in $(expand_list "$2"); do [[ $x == "$1" ]] && return 0; done; return 1; }

# ---------------------------------------------------------------------------------------------------
# 1. resolve cores
# ---------------------------------------------------------------------------------------------------
P=""; C=""; HK=""; HK_LIST=""
if [[ -r $STATE_DIR/node.env ]]; then
    # shellcheck disable=SC1091
    source "$STATE_DIR/node.env"
    P=${ANIMUS_PRODUCER_CPU:-}; C=${ANIMUS_CONSUMER_CPU:-}
    HK=${ANIMUS_HOUSEKEEPING_CPU:-}; HK_LIST=${ANIMUS_HOUSEKEEPING_CPUS:-}
fi
if [[ -n $CORES_ARG ]]; then P=${CORES_ARG%%,*}; C=${CORES_ARG##*,}; fi
[[ -n $HK_ARG ]] && { HK=$HK_ARG; HK_LIST=$HK_ARG; }
[[ -n $P && -n $C ]] || die "no cores: run sudo ./setup_baremetal_soak.sh first, or pass --cores P,C"
[[ $P =~ ^[0-9]+$ && $C =~ ^[0-9]+$ && $P != "$C" ]] || die "producer/consumer must be two distinct CPU ids (got '$P','$C')"
ONLINE=$(cat /sys/devices/system/cpu/online)
in_list "$P" "$ONLINE" || die "CPU $P is not online"
in_list "$C" "$ONLINE" || die "CPU $C is not online"
if [[ -n $HK ]]; then in_list "$HK" "$ONLINE" || die "housekeeping CPU $HK is not online"; fi
[[ $HK != "$P" && $HK != "$C" ]] || die "housekeeping CPU must differ from producer/consumer"

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
[[ -n $OUT_DIR ]] || OUT_DIR="$PWD/soak_runs/$STAMP"
mkdir -p "$OUT_DIR"
BIN="$OUT_DIR/sandbox_quickstart"
LOG="$OUT_DIR/soak.log"
note "output: $OUT_DIR"
note "producer CPU $P | consumer CPU $C | housekeeping ${HK:-none} | ${DURATION}s @ ${INTERVAL_NS} ns/msg"

# ---------------------------------------------------------------------------------------------------
# 2. preflight (recorded, not silent)
# ---------------------------------------------------------------------------------------------------
echo "preflight:"
SIBLINGS=0
[[ $(core_key "$P") == "$(core_key "$C")" ]] && SIBLINGS=1
if (( SIBLINGS && ! ALLOW_SIB )); then
    gate "distinct physical cores" 0 "CPUs $P and $C are SMT siblings of one core (use --allow-siblings only to measure same-core handoff)"
else
    gate "distinct physical cores" 1 "CPUs $P,$C: $(core_key "$P") vs $(core_key "$C")"
fi
gov_ok=1; gov_detail="performance on $P,$C"
for x in "$P" "$C"; do
    g=$(cat "/sys/devices/system/cpu/cpu$x/cpufreq/scaling_governor" 2>/dev/null || echo none)
    [[ $g == performance ]] || { gov_ok=0; gov_detail="CPU $x governor is '$g'"; }
done
soft "governor" "$gov_ok" "$gov_detail"
grep -qw hypervisor /proc/cpuinfo && hv=1 || hv=0
soft "bare metal (no hypervisor)" $(( ! hv )) "$( (( hv )) && echo 'hypervisor flag set: results are VM figures, not bare-metal' || echo 'no hypervisor flag')"
iso=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)
iso_ok=1
{ in_list "$P" "${iso:-}" && in_list "$C" "${iso:-}"; } || iso_ok=0
soft "boot isolation (isolcpus)" "$iso_ok" "isolated='${iso:-none}'"
clk=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo unknown)
[[ $clk == tsc ]] && clk_ok=1 || clk_ok=0
soft "clocksource tsc" "$clk_ok" "current_clocksource=$clk"
for f in constant_tsc nonstop_tsc rdtscp; do
    grep -qw "$f" /proc/cpuinfo && r=1 || r=0
    gate "cpu flag $f" "$r" ""
done
(( GATE_FAILS == 0 )) || { echo; note "preflight failed; nothing was run"; exit 1; }

# ---------------------------------------------------------------------------------------------------
# 3. compile
# ---------------------------------------------------------------------------------------------------
if [[ -z $CXX_BIN ]]; then for c in g++ clang++ c++; do command -v "$c" >/dev/null && { CXX_BIN=$c; break; }; done; fi
[[ -n $CXX_BIN ]] || die "no C++ compiler (set CXX or pass --cxx)"
CXX_FLAGS=(-O3 -march=native -std=c++17 -pthread -Wall -Wextra)
note "compiling with $("$CXX_BIN" --version | head -1)"
PIN_HK=()
[[ -n $HK_LIST ]] && command -v taskset >/dev/null && PIN_HK=(taskset -c "$HK_LIST")
if ! ${PIN_HK[@]+"${PIN_HK[@]}"} "$CXX_BIN" "${CXX_FLAGS[@]}" "$SRC" -o "$BIN" 2>"$OUT_DIR/build.log"; then
    cat "$OUT_DIR/build.log" >&2
    die "compilation failed"
fi
[[ -s $OUT_DIR/build.log ]] && note "compiler emitted warnings: $OUT_DIR/build.log"
command -v taskset >/dev/null || die "taskset (util-linux) is required"

# ---------------------------------------------------------------------------------------------------
# evidence snapshot (no hostname / machine id)
# ---------------------------------------------------------------------------------------------------
{
    echo "utc_start=$STAMP"
    echo "kernel=$(uname -r)"
    echo "cpu_model=$(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "online=$ONLINE isolated=${iso:-none} nohz_full=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo none)"
    echo "cmdline=$(cat /proc/cmdline)"
    echo "governor_P=$(cat /sys/devices/system/cpu/cpu$P/cpufreq/scaling_governor 2>/dev/null || echo n/a) governor_C=$(cat /sys/devices/system/cpu/cpu$C/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
    echo "freq_khz_P=$(cat /sys/devices/system/cpu/cpu$P/cpufreq/scaling_cur_freq 2>/dev/null || echo n/a) freq_khz_C=$(cat /sys/devices/system/cpu/cpu$C/cpufreq/scaling_cur_freq 2>/dev/null || echo n/a)"
    echo "thp_enabled=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo n/a)"
    echo "thp_defrag=$(cat /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || echo n/a)"
    echo "clocksource=$clk hypervisor=$hv smt_siblings=$SIBLINGS"
    echo "compiler=$("$CXX_BIN" --version | head -1)"
    echo "flags=${CXX_FLAGS[*]}"
    echo "src_sha256=$(sha256sum "$SRC" | cut -d' ' -f1)"
    echo "bin_sha256=$(sha256sum "$BIN" | cut -d' ' -f1)"
    echo "duration_s=$DURATION interval_ns=$INTERVAL_NS report_every_s=$REPORT_S"
} > "$OUT_DIR/environment.txt"

irq_snapshot() {   # prints "<total on P> <total on C> <LOC on P> <LOC on C>"
    awk -v p="$P" -v c="$C" '
        NR == 1 { next }
        {
            tp = tp + $(p + 2); tc = tc + $(c + 2)
            if ($1 == "LOC:") { lp = $(p + 2); lc = $(c + 2) }
        }
        END { printf "%d %d %d %d\n", tp, tc, lp, lc }' /proc/interrupts
}

# ---------------------------------------------------------------------------------------------------
# 4. smoke pass
# ---------------------------------------------------------------------------------------------------
RUN_CPUS="$P,$C${HK:+,$HK}"
HK_FLAG=()
[[ -n $HK ]] && HK_FLAG=(--housekeeping-cpu "$HK")
BASE_ARGS=(--producer-cpu "$P" --consumer-cpu "$C" ${HK_FLAG[@]+"${HK_FLAG[@]}"} --interval-ns "$INTERVAL_NS")

echo
note "smoke pass (300k messages)"
if ! taskset -c "$RUN_CPUS" "$BIN" "${BASE_ARGS[@]}" --messages 300000 --warmup 20000 >"$OUT_DIR/smoke.log" 2>&1; then
    tail -n 25 "$OUT_DIR/smoke.log" >&2
    note "smoke pass failed; the soak was not started (see $OUT_DIR/smoke.log)"
    exit 1
fi
note "smoke pass ok"

# ---------------------------------------------------------------------------------------------------
# 5. soak
# ---------------------------------------------------------------------------------------------------
TIME_BIN=""
[[ -x /usr/bin/time ]] && TIME_BIN=/usr/bin/time
read -r irq0_p irq0_c loc0_p loc0_c < <(irq_snapshot)
note "starting ${DURATION}s soak at $(date -u +%H:%M:%SZ); taskset -c $RUN_CPUS"
trap 'echo; note "interrupted; partial log: $LOG"; exit 130' INT TERM

set +e
if [[ -n $TIME_BIN ]]; then
    taskset -c "$RUN_CPUS" "$TIME_BIN" -v -o "$OUT_DIR/time.txt" \
        timeout --signal=KILL $((DURATION + 300)) \
        "$BIN" "${BASE_ARGS[@]}" --duration-s "$DURATION" --report-every-s "$REPORT_S" 2>&1 | tee "$LOG"
else
    taskset -c "$RUN_CPUS" \
        timeout --signal=KILL $((DURATION + 300)) \
        "$BIN" "${BASE_ARGS[@]}" --duration-s "$DURATION" --report-every-s "$REPORT_S" 2>&1 | tee "$LOG"
fi
RC=${PIPESTATUS[0]}
set -e
trap - INT TERM

read -r irq1_p irq1_c loc1_p loc1_c < <(irq_snapshot)

# ---------------------------------------------------------------------------------------------------
# 6. parse + gates
# ---------------------------------------------------------------------------------------------------
RESULT=$(grep '^RESULT ' "$LOG" | tail -n 1 || true)
[[ -n $RESULT ]] || { note "no RESULT line in $LOG (binary exit code $RC)"; exit 1; }
kv() { grep -o " $1=[^ ]*" <<<" $RESULT" | head -n 1 | cut -d= -f2; }

# percentiles.csv: one row per interval + the overall row (all values in nanoseconds)
{
    echo "scope,t_s,samples,p50_ns,p90_ns,p99_ns,p99_9_ns,max_ns"
    grep '^INTERVAL ' "$LOG" | awk '{
        for (i = 2; i <= NF; i++) { split($i, a, "="); v[a[1]] = a[2] }
        gsub("s", "", v["t"])
        printf "interval,%s,%s,%s,%s,%s,%s,%s\n", v["t"], v["n"], v["p50_ns"], v["p90_ns"], v["p99_ns"], v["p99_9_ns"], v["max_ns"]
    }'
    echo "overall,$DURATION,$(kv samples),$(kv p50_ns),$(kv p90_ns),$(kv p99_ns),$(kv p99_9_ns),$(kv max_ns)"
} > "$OUT_DIR/percentiles.csv"

# drift / worst-interval analysis
read -r first_p50 last_p50 worst_p99 worst_p999 n_int < <(awk -F, '
    $1 == "interval" { n++; if (n == 1) f = $4; l = $4; if ($6 + 0 > w99) w99 = $6 + 0; if ($7 + 0 > w999) w999 = $7 + 0 }
    END { printf "%.1f %.1f %.1f %.1f %d\n", f, l, w99, w999, n }' "$OUT_DIR/percentiles.csv")

allocs=$(kv allocs); frees=$(kv frees)
seqerr=$(kv seq_errors); payerr=$(kv payload_errors); tscreg=$(kv tsc_regressions)
stalls=$(kv ring_full_stalls); sent=$(kv sent); recv=$(kv received)
pcpu=$(kv producer_cpu); ccpu=$(kv consumer_cpu)
p50=$(kv p50_ns); p90=$(kv p90_ns); p99=$(kv p99_ns); p999=$(kv p99_9_ns); pmax=$(kv max_ns)

ctx_inv="n/a"; minflt="n/a"; majflt="n/a"
if [[ -f $OUT_DIR/time.txt ]]; then
    ctx_inv=$(awk -F: '/Involuntary context switches/ {gsub(/ /, "", $2); print $2}' "$OUT_DIR/time.txt")
    minflt=$(awk -F: '/Minor \(reclaiming a frame\) page faults/ {gsub(/ /, "", $2); print $2}' "$OUT_DIR/time.txt")
    majflt=$(awk -F: '/Major \(requiring I\/O\) page faults/ {gsub(/ /, "", $2); print $2}' "$OUT_DIR/time.txt")
fi

echo
echo "gates:"
gate "binary exit status"             $(( RC == 0 ))                                  "exit=$RC"
gate "interposer armed (self-test)"   $(( $(kv alloc_audit) == 1 ))                   "malloc + operator new observed on a control call"
gate "hot-path malloc == 0"           $(( allocs == 0 ))                              "allocs=$allocs"
gate "hot-path free == 0"             $(( frees == 0 ))                               "frees=$frees"
gate "sequence errors == 0"           $(( seqerr == 0 ))                              "seq_errors=$seqerr"
gate "payload errors == 0"            $(( payerr == 0 ))                              "payload_errors=$payerr"
gate "TSC regressions == 0"           $(( tscreg == 0 ))                              "tsc_regressions=$tscreg (consumer RX stamp earlier than producer stamp)"
gate "all messages delivered"         $(( sent == recv && sent > 0 ))                 "sent=$sent received=$recv"
gate "threads ran on assigned CPUs"   $(( pcpu == P && ccpu == C && $(kv pin_ok) == 1 )) "producer=$pcpu (want $P) consumer=$ccpu (want $C)"
gate "no SMT-sibling pairing"         $(( ALLOW_SIB || $(kv smt_siblings) != 1 ))    "smt_siblings=$(kv smt_siblings)"
gate "intervals logged"               $(( REPORT_S == 0 || n_int >= 1 ))              "$n_int interval rows"
if [[ -n $MAX_P99 ]];  then gate "overall p99 <= ${MAX_P99} ns"   "$(awk -v a="$p99"  -v b="$MAX_P99"  'BEGIN{print (a <= b)}')" "p99=${p99} ns"; fi
if [[ -n $MAX_P999 ]]; then gate "overall p99.9 <= ${MAX_P999} ns" "$(awk -v a="$p999" -v b="$MAX_P999" 'BEGIN{print (a <= b)}')" "p99.9=${p999} ns"; fi

# ---------------------------------------------------------------------------------------------------
# 7. summary
# ---------------------------------------------------------------------------------------------------
VERDICT=PASS; (( GATE_FAILS )) && VERDICT=FAIL
{
    echo "AnimusCore isolated soak -- $VERDICT"
    echo "  duration            : ${DURATION}s, pacing ${INTERVAL_NS} ns/msg, samples=$(kv samples)"
    echo "  cores               : producer=$P consumer=$C housekeeping=${HK:-none} (hypervisor=$hv, governor ok=$gov_ok, boot-isolated=$iso_ok)"
    echo "  one-way handoff ns  : p50=$p50  p90=$p90  p99=$p99  p99.9=$p999  max=$pmax"
    echo "  worst 1-interval    : p99=${worst_p99} ns  p99.9=${worst_p999} ns   (p50 first=${first_p50} last=${last_p50} ns across $n_int intervals)"
    echo "  hot-path heap       : malloc-family=$allocs free=$frees"
    echo "  integrity           : seq_errors=$seqerr payload_errors=$payerr tsc_regressions=$tscreg ring_full_stalls=$stalls"
    echo "  interrupts during run: cpu$P total=$((irq1_p - irq0_p)) (LOC timer $((loc1_p - loc0_p))) | cpu$C total=$((irq1_c - irq0_c)) (LOC timer $((loc1_c - loc0_c)))"
    echo "  process counters    : involuntary ctx switches=$ctx_inv minor faults=$minflt major faults=$majflt (whole process incl. setup)"
    echo "  files               : $OUT_DIR/{soak.log,percentiles.csv,environment.txt,summary.json}"
    echo "  caveats             : latency is one-way producer-stamp -> consumer-RDTSCP on this host; timer floor $(kv timer_floor_cyc) cycles not subtracted;"
    echo "                        percentiles are histogram bucket upper bounds (<= 1.6% above 127 cycles)."
} | tee "$OUT_DIR/summary.txt"

printf '{"verdict":"%s","duration_s":%s,"interval_ns":%s,"samples":%s,"producer_cpu":%s,"consumer_cpu":%s,"hypervisor":%s,"p50_ns":%s,"p90_ns":%s,"p99_ns":%s,"p99_9_ns":%s,"max_ns":%s,"worst_interval_p99_ns":%s,"worst_interval_p99_9_ns":%s,"allocs":%s,"frees":%s,"seq_errors":%s,"payload_errors":%s,"tsc_regressions":%s,"ring_full_stalls":%s,"irq_total_producer_cpu":%s,"irq_total_consumer_cpu":%s}\n' \
    "$VERDICT" "$DURATION" "$INTERVAL_NS" "$(kv samples)" "$P" "$C" "$hv" "$p50" "$p90" "$p99" "$p999" "$pmax" \
    "$worst_p99" "$worst_p999" "$allocs" "$frees" "$seqerr" "$payerr" "$tscreg" "$stalls" \
    "$((irq1_p - irq0_p))" "$((irq1_c - irq0_c))" > "$OUT_DIR/summary.json"

(( GATE_FAILS == 0 ))
