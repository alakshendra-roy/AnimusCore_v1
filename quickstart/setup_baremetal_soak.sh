#!/usr/bin/env bash
# setup_baremetal_soak.sh -- prepare a Linux x86-64 node for an AnimusCore latency soak.
#
# What it does (every write is recorded and undone by --restore):
#   1. verifies the clock the benchmark depends on: constant_tsc / nonstop_tsc / rdtscp flags, clocksource,
#      then a RUNTIME probe -- RDTSCP monotonicity on each chosen core and a cross-core handoff test that
#      counts negative deltas (the exact failure that would corrupt a producer->consumer TSC measurement)
#   2. CPU governor -> performance (plus EPP=performance where intel_pstate exposes it); optional --no-turbo
#   3. THP: enabled=madvise, defrag=never, khugepaged defrag off; optional --hugepages N reservation
#   4. drops page cache/dentries/inodes and compacts memory so the run does not pay for it mid-flight
#   5. picks (or validates) one producer core and one consumer core on DIFFERENT physical cores, same NUMA node,
#      and builds a housekeeping set from everything else
#   6. steers IRQs, workqueues, and irqbalance off the isolated cores; optionally offlines their SMT siblings
#   7. writes $STATE_DIR/node.env, which run_isolated_soak.sh sources
#
# What it CANNOT do at runtime: remove timer ticks, RCU callbacks or kernel threads from a core. That needs
# boot parameters; the script prints the exact line when they are missing.
#
# Usage:  sudo ./setup_baremetal_soak.sh [options]
#   --cores P,C           producer,consumer CPU ids (default: auto)
#   --offline-siblings    take the SMT siblings of P and C offline (strongest option; undone by --restore)
#   --no-turbo            disable turbo/boost for a fixed-frequency run
#   --hugepages N         reserve N x 2 MiB hugetlb pages (vm.nr_hugepages)
#   --allow-siblings      permit --cores that share a physical core (measures same-core handoff only)
#   --require-bare-metal  fail if a hypervisor is detected
#   --state-dir DIR       default /var/run/animus_soak
#   --dry-run             print what would change; write nothing
#   --restore             undo every change recorded by earlier runs
#   -h, --help

set -Eeuo pipefail
shopt -s nullglob

if (( BASH_VERSINFO[0] < 4 )); then echo "bash >= 4 required" >&2; exit 2; fi

STATE_DIR=/var/run/animus_soak
CORES_ARG=""
OFFLINE_SIB=0
NO_TURBO=0
HUGEPAGES=""
ALLOW_SIB=0
REQ_BARE=0
DRY=0
RESTORE=0

while (( $# )); do
    case "$1" in
        --cores)              CORES_ARG=${2:?--cores needs P,C}; shift 2 ;;
        --offline-siblings)   OFFLINE_SIB=1; shift ;;
        --no-turbo)           NO_TURBO=1; shift ;;
        --hugepages)          HUGEPAGES=${2:?--hugepages needs N}; shift 2 ;;
        --allow-siblings)     ALLOW_SIB=1; shift ;;
        --require-bare-metal) REQ_BARE=1; shift ;;
        --state-dir)          STATE_DIR=${2:?--state-dir needs DIR}; shift 2 ;;
        --dry-run)            DRY=1; shift ;;
        --restore)            RESTORE=1; shift ;;
        -h|--help)            sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$STATE_DIR" 2>/dev/null || true
if [[ ! -w $STATE_DIR ]]; then
    (( DRY )) || die "cannot write to $STATE_DIR"
    STATE_DIR=$(mktemp -d)   # dry-run as non-root: scratch dir for the probe build only
fi
RESTORE_FILE="$STATE_DIR/restore.tsv"
NODE_ENV="$STATE_DIR/node.env"
WARNS=0
FAILS=0

log()  { printf '%s\n' "$*"; }
ok()   { printf '  [ ok ] %s\n' "$*"; }
warn() { printf '  [WARN] %s\n' "$*"; WARNS=$((WARNS + 1)); }
fail() { printf '  [FAIL] %s\n' "$*"; FAILS=$((FAILS + 1)); }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
hdr()  { printf '\n== %s\n' "$*"; }

[[ $(uname -s) == Linux ]] || die "Linux only"
[[ $(uname -m) == x86_64 ]] || die "x86-64 only (TSC/RDTSCP)"
if (( ! DRY )) && (( EUID != 0 )); then die "must run as root (or use --dry-run)"; fi

# ---------------------------------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------------------------------
expand_list() {   # "0-3,6" -> one id per line
    local IFS=, part
    for part in $1; do
        [[ -z $part ]] && continue
        if [[ $part == *-* ]]; then seq "${part%-*}" "${part#*-}"; else echo "$part"; fi
    done
}
join_by() { local IFS=,; echo "$*"; }
cpulist_to_mask() {   # space-separated CPU ids -> hex affinity mask
    local -a bits=()
    local c max=0 n i nib out=""
    for c in "$@"; do bits[c]=1; (( c > max )) && max=$c; done
    for (( n = (max / 4) * 4; n >= 0; n -= 4 )); do
        nib=0
        for i in 0 1 2 3; do (( ${bits[n + i]:-0} )) && nib=$(( nib | (1 << i) )); done
        out+=$(printf '%x' "$nib")
    done
    echo "$out"
}
sysfs_cpu() { echo "/sys/devices/system/cpu/cpu$1"; }
cpu_pkg()   { cat "$(sysfs_cpu "$1")/topology/physical_package_id" 2>/dev/null || echo -1; }
cpu_core()  { cat "$(sysfs_cpu "$1")/topology/core_id" 2>/dev/null || echo -1; }
cpu_key()   { local p c; p=$(cpu_pkg "$1"); c=$(cpu_core "$1"); if (( p < 0 || c < 0 )); then echo "cpu$1"; else echo "$p:$c"; fi; }
cpu_node()  { local d; for d in "$(sysfs_cpu "$1")"/node[0-9]*; do echo "${d##*node}"; return; done; echo 0; }
cpu_siblings() { expand_list "$(cat "$(sysfs_cpu "$1")/topology/thread_siblings_list" 2>/dev/null || echo "$1")"; }

SYSW_QUIET=0
declare -A SAVED=()
if [[ -f $RESTORE_FILE ]]; then
    while IFS=$'\t' read -r p _; do [[ -n $p ]] && SAVED[$p]=1; done < "$RESTORE_FILE"
fi

current_value() {   # normalises "always [madvise] never" to "madvise"
    local v
    v=$(cat "$1" 2>/dev/null) || return 1
    if [[ $v == *\[*\]* ]]; then v=${v#*\[}; v=${v%%\]*}; fi
    printf '%s' "$v"
}

# sysw PATH VALUE : record the previous value (first write wins across runs), then write.
sysw() {
    local path=$1 val=$2 old
    [[ -e $path ]] || return 1
    if (( DRY )); then
        (( SYSW_QUIET )) || printf '    [dry-run] %s <- %s\n' "$path" "$val"
        return 0
    fi
    if [[ -z ${SAVED[$path]:-} ]]; then
        old=$(current_value "$path") || return 1
        printf '%s\t%s\n' "$path" "$old" >> "$RESTORE_FILE"
        SAVED[$path]=1
    fi
    { printf '%s' "$val" > "$path"; } 2>/dev/null
}

# ---------------------------------------------------------------------------------------------------
# --restore
# ---------------------------------------------------------------------------------------------------
if (( RESTORE )); then
    [[ -f $RESTORE_FILE ]] || die "nothing to restore ($RESTORE_FILE missing)"
    hdr "restoring from $RESTORE_FILE"
    n_ok=0; n_bad=0
    # CPUs first, so their per-CPU sysfs nodes (governor etc.) exist again before we write them.
    for pass in online other; do
        while IFS=$'\t' read -r p v; do
            [[ -z $p ]] && continue
            if [[ $pass == online && $p != */online ]]; then continue; fi
            if [[ $pass == other && $p == */online ]]; then continue; fi
            if [[ $p == service:* ]]; then
                svc=${p#service:}
                if [[ $v == active ]] && command -v systemctl >/dev/null; then systemctl start "$svc" 2>/dev/null && n_ok=$((n_ok + 1)) || n_bad=$((n_bad + 1)); fi
                continue
            fi
            if (( DRY )); then printf '    [dry-run] %s <- %s\n' "$p" "$v"; continue; fi
            if { printf '%s' "$v" > "$p"; } 2>/dev/null; then n_ok=$((n_ok + 1)); else n_bad=$((n_bad + 1)); fi
        done < "$RESTORE_FILE"
    done
    if (( ! DRY )); then mv -f "$RESTORE_FILE" "$RESTORE_FILE.restored"; rm -f "$NODE_ENV"; fi
    log "restored $n_ok setting(s); $n_bad could not be written (per-IRQ/per-CPU entries that no longer exist are normal)"
    exit 0
fi

# ---------------------------------------------------------------------------------------------------
# 0. host sanity
# ---------------------------------------------------------------------------------------------------
hdr "0. host"
CPUINFO_FLAGS=$(grep -m1 '^flags' /proc/cpuinfo || true)
MODEL=$(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //' || true)
log "  cpu      : ${MODEL:-unknown}"
log "  kernel   : $(uname -r)"
if grep -qw hypervisor <<<"$CPUINFO_FLAGS"; then
    if (( REQ_BARE )); then die "hypervisor detected and --require-bare-metal given"; fi
    warn "hypervisor detected: this is a VM, not bare metal; vCPU pinning, TSC and IRQ steering are best-effort and results are not bare-metal figures"
else
    ok "no hypervisor flag"
fi
ONLINE_CPUS=($(expand_list "$(cat /sys/devices/system/cpu/online)"))
log "  online   : ${#ONLINE_CPUS[@]} CPUs"
(( ${#ONLINE_CPUS[@]} >= 2 )) || die "need at least 2 online CPUs"

# ---------------------------------------------------------------------------------------------------
# 1. core selection (needed before per-core probes)
# ---------------------------------------------------------------------------------------------------
hdr "1. core selection"
P=""; C=""
if [[ -n $CORES_ARG ]]; then
    P=${CORES_ARG%%,*}; C=${CORES_ARG##*,}
    [[ $P =~ ^[0-9]+$ && $C =~ ^[0-9]+$ && $P != "$C" ]] || die "--cores wants two distinct CPU ids, e.g. 2,4"
    for x in "$P" "$C"; do
        [[ " ${ONLINE_CPUS[*]} " == *" $x "* ]] || die "CPU $x is not online"
    done
    if [[ $(cpu_key "$P") == "$(cpu_key "$C")" ]]; then
        if (( ALLOW_SIB )); then warn "CPUs $P and $C are SMT siblings of one physical core (--allow-siblings): same-core handoff only"
        else die "CPUs $P and $C are SMT siblings of one physical core; pick different cores or pass --allow-siblings"; fi
    else
        ok "CPUs $P and $C are on different physical cores"
    fi
else
    ISOLATED_K=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)
    CAND=()
    if [[ -n $ISOLATED_K ]]; then
        CAND=($(expand_list "$ISOLATED_K"))
        log "  using kernel-isolated CPUs ($ISOLATED_K) as candidates"
    else
        CAND=("${ONLINE_CPUS[@]}")
        if [[ -r /sys/devices/cpu_core/cpus ]]; then   # Intel hybrid: prefer P-cores
            PCORES=" $(expand_list "$(cat /sys/devices/cpu_core/cpus)" | tr '\n' ' ') "
            FILTERED=()
            for x in "${CAND[@]}"; do [[ $PCORES == *" $x "* ]] && FILTERED+=("$x"); done
            (( ${#FILTERED[@]} >= 2 )) && CAND=("${FILTERED[@]}") && log "  hybrid CPU detected: restricting to P-cores"
        fi
        HK_KEY=$(cpu_key "${ONLINE_CPUS[0]}")   # leave the first core to the OS
        FILTERED=()
        for x in "${CAND[@]}"; do [[ $(cpu_key "$x") != "$HK_KEY" ]] && FILTERED+=("$x"); done
        if (( ${#FILTERED[@]} >= 2 )); then CAND=("${FILTERED[@]}"); else warn "too few CPUs to reserve a housekeeping core"; fi
    fi
    declare -A SEEN_KEY=() NODE_LIST=()
    NODE_ORDER=()
    for x in "${CAND[@]}"; do
        k=$(cpu_key "$x")
        [[ -n ${SEEN_KEY[$k]:-} ]] && continue     # only the first (primary) thread of each physical core
        SEEN_KEY[$k]=1
        nd=$(cpu_node "$x")
        [[ -z ${NODE_LIST[$nd]:-} ]] && NODE_ORDER+=("$nd")
        NODE_LIST[$nd]+="$x "
    done
    for nd in "${NODE_ORDER[@]}"; do
        read -r -a arr <<<"${NODE_LIST[$nd]}"
        if (( ${#arr[@]} >= 2 )); then P=${arr[0]}; C=${arr[1]}; log "  NUMA node $nd: picked CPUs $P and $C (distinct physical cores)"; break; fi
    done
    if [[ -z $P ]]; then
        ALL=()
        for nd in "${NODE_ORDER[@]}"; do read -r -a arr <<<"${NODE_LIST[$nd]}"; ALL+=("${arr[@]}"); done
        (( ${#ALL[@]} >= 2 )) || die "could not find two distinct physical cores; use --cores with --allow-siblings"
        P=${ALL[0]}; C=${ALL[1]}
        warn "no NUMA node has two free cores: CPUs $P and $C are on different nodes (cross-node handoff)"
    fi
fi
NODE_P=$(cpu_node "$P"); NODE_C=$(cpu_node "$C")
[[ $NODE_P == "$NODE_C" ]] && ok "producer CPU $P and consumer CPU $C share NUMA node $NODE_P" || warn "producer on NUMA node $NODE_P, consumer on node $NODE_C"

SIB_OFF=()
for x in "$P" "$C"; do
    for s in $(cpu_siblings "$x"); do [[ $s != "$x" && $s != "$P" && $s != "$C" ]] && SIB_OFF+=("$s"); done
done
HK=()
for x in "${ONLINE_CPUS[@]}"; do
    [[ $x == "$P" || $x == "$C" ]] && continue
    if (( OFFLINE_SIB )) && [[ " ${SIB_OFF[*]:-} " == *" $x "* ]]; then continue; fi
    HK+=("$x")
done
HK_LIST=$(join_by "${HK[@]:-}")
log "  isolated : producer=$P consumer=$C"
log "  housekeep: ${HK_LIST:-<none>}"
if (( ${#SIB_OFF[@]} )); then
    if (( OFFLINE_SIB )); then log "  SMT siblings to offline: ${SIB_OFF[*]}"
    else warn "SMT siblings of the isolated cores (${SIB_OFF[*]}) stay online and will share execution units with whatever runs on them; use --offline-siblings or keep them idle"; fi
fi

# ---------------------------------------------------------------------------------------------------
# 2. TSC verification
# ---------------------------------------------------------------------------------------------------
hdr "2. TSC invariants"
for f in constant_tsc nonstop_tsc rdtscp; do
    if grep -qw "$f" <<<"$CPUINFO_FLAGS"; then ok "cpuinfo flag: $f"; else fail "cpuinfo flag missing: $f"; fi
done
for f in tsc_reliable tsc_known_freq tsc_adjust; do
    grep -qw "$f" <<<"$CPUINFO_FLAGS" && log "  (info) flag present: $f"
done
CLK=$(cat /sys/devices/system/clocksource/clocksource0/current_clocksource 2>/dev/null || echo unknown)
if [[ $CLK == tsc ]]; then ok "current clocksource: tsc"; else warn "current clocksource is '$CLK', not tsc (check dmesg for 'tsc: Marking TSC unstable'; the harness still uses raw RDTSC but the kernel distrusts it)"; fi

PROBE_SRC="$STATE_DIR/tsc_probe.cpp"
PROBE_BIN="$STATE_DIR/tsc_probe"
CXX_BIN=${CXX:-}
if [[ -z $CXX_BIN ]]; then for c in g++ clang++ c++; do command -v "$c" >/dev/null && { CXX_BIN=$c; break; }; done; fi
if [[ -z $CXX_BIN ]]; then
    warn "no C++ compiler found: skipping the runtime RDTSCP / cross-core probe (flags above are the only TSC evidence)"
else
    cat > "$PROBE_SRC" <<'PROBE_EOF'
// tsc_probe: runtime verification of RDTSCP on one core, and TSC coherence between two cores.
#include <cpuid.h>
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static bool pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    return pthread_setaffinity_np(pthread_self(), sizeof s, &s) == 0;
}
static inline std::uint64_t rd() { unsigned a; return __rdtscp(&a); }
struct alignas(64) Shared { std::atomic<std::uint64_t> stamp{0}; std::atomic<unsigned> turn{0}; };
static Shared g_sh;
static int g_peer_cpu = 0;
static std::uint64_t g_neg_b = 0, g_min_b = ~0ull;
static constexpr unsigned kRounds = 200000;
static void* peer(void*) {
    pin(g_peer_cpu);
    for (unsigned i = 0; i < kRounds; ++i) {
        while (g_sh.turn.load(std::memory_order_acquire) != 1) {}
        const std::uint64_t s = g_sh.stamp.load(std::memory_order_relaxed), t = rd();
        if (t < s) ++g_neg_b; else if (t - s < g_min_b) g_min_b = t - s;
        g_sh.stamp.store(t, std::memory_order_relaxed);
        g_sh.turn.store(0, std::memory_order_release);
    }
    return nullptr;
}
int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "mono")) {
        const int cpu = std::atoi(argv[2]);
        if (!pin(cpu)) { std::printf("mono cpu=%d pin_failed=1\n", cpu); return 1; }
        unsigned a, b, c, d;
        __get_cpuid(0x80000007u, &a, &b, &c, &d);
        const int inv = (d >> 8) & 1;
        __get_cpuid(0x80000001u, &a, &b, &c, &d);
        const int rdtscp = (d >> 27) & 1;
        std::uint64_t prev = rd(), bad = 0, maxgap = 0;
        for (int i = 0; i < 5'000'000; ++i) {
            const std::uint64_t t = rd();
            if (t < prev) ++bad; else if (t - prev > maxgap) maxgap = t - prev;
            prev = t;
        }
        std::printf("mono cpu=%d invariant=%d rdtscp=%d regress=%llu max_gap_cyc=%llu\n", cpu, inv, rdtscp,
                    (unsigned long long)bad, (unsigned long long)maxgap);
        return (bad == 0 && inv && rdtscp) ? 0 : 1;
    }
    if (argc == 4 && !std::strcmp(argv[1], "pair")) {
        const int a = std::atoi(argv[2]);
        g_peer_cpu = std::atoi(argv[3]);
        if (!pin(a)) { std::printf("pair pin_failed=1\n"); return 1; }
        pthread_t th;
        pthread_create(&th, nullptr, peer, nullptr);
        std::uint64_t neg_a = 0, min_a = ~0ull;
        for (unsigned i = 0; i < kRounds; ++i) {
            while (g_sh.turn.load(std::memory_order_acquire) != 0) {}
            const std::uint64_t s = g_sh.stamp.load(std::memory_order_relaxed), t = rd();
            if (t < s) ++neg_a; else if (i && t - s < min_a) min_a = t - s;
            g_sh.stamp.store(t, std::memory_order_relaxed);
            g_sh.turn.store(1, std::memory_order_release);
        }
        pthread_join(th, nullptr);
        std::printf("pair a=%d b=%d negatives=%llu min_delta_cyc=%llu\n", a, g_peer_cpu,
                    (unsigned long long)(neg_a + g_neg_b), (unsigned long long)(min_a < g_min_b ? min_a : g_min_b));
        return (neg_a + g_neg_b) == 0 ? 0 : 1;
    }
    std::fprintf(stderr, "usage: tsc_probe mono CPU | pair CPU_A CPU_B\n");
    return 2;
}
PROBE_EOF
    if "$CXX_BIN" -O2 -std=c++17 -pthread "$PROBE_SRC" -o "$PROBE_BIN" 2>"$STATE_DIR/tsc_probe.build.log"; then
        for x in "$P" "$C"; do
            if out=$("$PROBE_BIN" mono "$x"); then ok "$out"; else fail "$out"; fi
        done
        if out=$("$PROBE_BIN" pair "$P" "$C"); then ok "$out (0 negatives = TSC coherent across these two cores)"; else fail "$out -- cross-core TSC skew: producer/consumer timestamps cannot be compared on this pair"; fi
    else
        warn "could not build the TSC probe (see $STATE_DIR/tsc_probe.build.log); runtime TSC verification skipped"
    fi
fi

# ---------------------------------------------------------------------------------------------------
# 3. CPU frequency
# ---------------------------------------------------------------------------------------------------
hdr "3. CPU governor"
n=0; bad=0
for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
    sysw "$f" performance && n=$((n + 1)) || bad=$((bad + 1))
done
if (( n == 0 )); then warn "no cpufreq scaling_governor interface (VM, or driver does not expose one): frequency is whatever the platform decides"
else
    ok "scaling_governor=performance written on $n CPU(s), $bad not writable"
    for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/energy_performance_preference; do sysw "$f" performance || true; done
    if (( ! DRY )); then
        notperf=0
        for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do [[ $(cat "$f") == performance ]] || notperf=$((notperf + 1)); done
        (( notperf == 0 )) && ok "readback: all governors are 'performance'" || fail "$notperf CPU(s) did not accept the performance governor"
    fi
fi
if (( NO_TURBO )); then
    if sysw /sys/devices/system/cpu/intel_pstate/no_turbo 1 || sysw /sys/devices/system/cpu/cpufreq/boost 0; then ok "turbo/boost disabled"; else warn "no turbo control found"; fi
else
    log "  turbo left as-is (pass --no-turbo for a fixed-frequency run)"
fi

# ---------------------------------------------------------------------------------------------------
# 4. memory: THP, hugepages, caches
# ---------------------------------------------------------------------------------------------------
hdr "4. memory"
THP=/sys/kernel/mm/transparent_hugepage
if [[ -d $THP ]]; then
    sysw $THP/enabled madvise && ok "THP enabled=madvise (no silent promotion of the ring / histogram pages)" || warn "could not set THP enabled"
    sysw $THP/defrag never && ok "THP defrag=never (no direct compaction stalls on page faults)" || warn "could not set THP defrag"
    sysw $THP/khugepaged/defrag 0 || true
else
    warn "no transparent_hugepage interface"
fi
if [[ -n $HUGEPAGES ]]; then
    sysw /proc/sys/vm/nr_hugepages "$HUGEPAGES" && ok "vm.nr_hugepages=$HUGEPAGES (2 MiB pages)" || warn "could not reserve hugepages"
    (( DRY )) || log "  HugePages_Total now: $(awk '/HugePages_Total/ {print $2}' /proc/meminfo)"
fi
sysw /proc/sys/kernel/numa_balancing 0 && ok "numa_balancing=0 (no page-migration hinting faults)" || true
sysw /proc/sys/kernel/nmi_watchdog 0 && ok "nmi_watchdog=0 (no periodic NMI)" || true

cached_before=$(awk '/^Cached:/ {print $2}' /proc/meminfo)
if (( DRY )); then
    log "    [dry-run] sync; echo 3 > /proc/sys/vm/drop_caches; echo 1 > /proc/sys/vm/compact_memory"
else
    sync
    if printf 3 > /proc/sys/vm/drop_caches 2>/dev/null; then
        ok "page cache dropped: Cached ${cached_before} kB -> $(awk '/^Cached:/ {print $2}' /proc/meminfo) kB"
    else
        warn "could not write /proc/sys/vm/drop_caches"
    fi
    printf 1 > /proc/sys/vm/compact_memory 2>/dev/null && ok "memory compacted" || true
fi

# ---------------------------------------------------------------------------------------------------
# 5. isolation
# ---------------------------------------------------------------------------------------------------
hdr "5. isolation of CPUs $P and $C"
CMDLINE=$(cat /proc/cmdline)
ISO_K=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)
NOHZ_K=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || true)
in_list() { local want=$1 list=$2 x; [[ -z $list || $list == "(null)" ]] && return 1; for x in $(expand_list "$list"); do [[ $x == "$want" ]] && return 0; done; return 1; }
boot_missing=0
for x in "$P" "$C"; do
    in_list "$x" "$ISO_K" || boot_missing=1
    in_list "$x" "$NOHZ_K" || boot_missing=1
done
if (( boot_missing )); then
    warn "CPUs $P,$C are not (fully) isolated at boot (isolated='${ISO_K:-none}' nohz_full='${NOHZ_K:-none}'): timer ticks and RCU callbacks can still land on them"
    log "         add to the kernel cmdline and reboot:  isolcpus=domain,managed_irq,$P,$C nohz_full=$P,$C rcu_nocbs=$P,$C"
    log "         optional for C-state jitter:           intel_idle.max_cstate=1 processor.max_cstate=1"
else
    ok "boot-time isolation present (isolcpus / nohz_full cover $P,$C)"
fi
grep -q 'rcu_nocbs=' <<<"$CMDLINE" || log "  (info) rcu_nocbs not set on the kernel command line"

if [[ -n $HK_LIST ]]; then
    # irqbalance would undo the steering below
    if command -v systemctl >/dev/null && systemctl is-active --quiet irqbalance 2>/dev/null; then
        if (( DRY )); then log "    [dry-run] systemctl stop irqbalance"
        else
            if [[ -z ${SAVED[service:irqbalance]:-} ]]; then printf 'service:irqbalance\tactive\n' >> "$RESTORE_FILE"; SAVED[service:irqbalance]=1; fi
            systemctl stop irqbalance && ok "irqbalance stopped (restarted by --restore)"
        fi
    fi
    irq_ok=0; irq_skip=0
    SYSW_QUIET=1   # one line per IRQ is noise in --dry-run; the summary below says what happened
    for f in /proc/irq/[0-9]*/smp_affinity_list; do
        if sysw "$f" "$HK_LIST"; then irq_ok=$((irq_ok + 1)); else irq_skip=$((irq_skip + 1)); fi
    done
    SYSW_QUIET=0
    ok "IRQ affinity -> $HK_LIST on $irq_ok IRQ(s); $irq_skip not movable (per-CPU / managed IRQs: normal)"
    read -r -a HKA <<<"${HK[*]}"
    HK_MASK=$(cpulist_to_mask "${HKA[@]}")
    sysw /proc/irq/default_smp_affinity "$HK_MASK" || true
    sysw /sys/devices/virtual/workqueue/cpumask "$HK_MASK" && ok "unbound workqueues -> mask $HK_MASK" || log "  (info) workqueue cpumask not writable here"
    sysw /sys/devices/virtual/workqueue/writeback/cpumask "$HK_MASK" || true
else
    warn "no housekeeping CPUs left; skipping IRQ/workqueue steering"
fi

if (( OFFLINE_SIB )); then
    for s in "${SIB_OFF[@]:-}"; do
        [[ -z $s ]] && continue
        sysw "$(sysfs_cpu "$s")/online" 0 && ok "CPU $s (SMT sibling) offlined" || warn "could not offline CPU $s"
    done
fi

# who is still running on the isolated cores right now?
if command -v ps >/dev/null; then
    busy=$(ps -eLo psr=,comm= 2>/dev/null | awk -v p="$P" -v c="$C" '($1 == p || $1 == c) && $2 !~ /^(ps|awk|bash|setup_baremetal)/ {n[$1" "$2]++} END {for (k in n) printf "%s(x%d) ", k, n[k]}' || true)
    [[ -n $busy ]] && log "  (info) threads currently scheduled on the isolated CPUs [cpu comm]: $busy"
fi

# ---------------------------------------------------------------------------------------------------
# 6. hand-off file
# ---------------------------------------------------------------------------------------------------
hdr "6. hand-off"
if (( DRY )); then
    log "    [dry-run] would write $NODE_ENV"
else
    {
        echo "# written by setup_baremetal_soak.sh -- sourced by run_isolated_soak.sh"
        echo "ANIMUS_SETUP_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "ANIMUS_PRODUCER_CPU=$P"
        echo "ANIMUS_CONSUMER_CPU=$C"
        echo "ANIMUS_HOUSEKEEPING_CPUS=$HK_LIST"
        echo "ANIMUS_HOUSEKEEPING_CPU=${HK[0]:-}"
        echo "ANIMUS_SIBLINGS_OFFLINED=$OFFLINE_SIB"
    } > "$NODE_ENV"
    chmod 0644 "$NODE_ENV"
    ok "wrote $NODE_ENV"
fi
log ""
log "  taskset bindings:"
log "    producer : taskset -c $P <cmd>"
log "    consumer : taskset -c $C <cmd>"
log "    run both : taskset -c $P,$C${HK[0]:+,${HK[0]}} ./sandbox_quickstart --producer-cpu $P --consumer-cpu $C${HK[0]:+ --housekeeping-cpu ${HK[0]}}"
log "  next: ./run_isolated_soak.sh        (undo everything with: sudo ./setup_baremetal_soak.sh --restore)"

hdr "summary"
log "  failures: $FAILS   warnings: $WARNS"
if (( FAILS )); then log "  RESULT: NOT READY"; exit 1; fi
if (( WARNS )); then log "  RESULT: READY WITH WARNINGS"; else log "  RESULT: READY"; fi
exit 0
