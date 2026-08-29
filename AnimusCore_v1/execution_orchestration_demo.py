"""Phase 21: RBAC-gated multi-tenant execution orchestration.

Wires animus.ShmOrderRingChannel (a Python binding over
include/animus/shm_ipc.hpp's ShmRing<T>, instantiated for
animus::OrderRequest) to animus.SecurityContext (a Python binding over
animus_security.hpp's TenantRegistry + SecureExecutionGateway) -- a
producer process pushes orders for one tenant across zero-copy shared
memory; a consumer process drains them and routes each one through a
per-tenant, RBAC-checked animus::ExecutionClient.

One ShmOrderRingChannel PER TENANT, not one shared ring carrying a tenant
id on the wire: matches animus_security.hpp's own "isolation is
structural" design (SecureExecutionGateway's docstring) -- a producer for
tenant A physically cannot address tenant B's ring, the same way tenant
A's Engine and tenant B's Engine were already structurally separate
before this feature existed.

Usage:
  python execution_orchestration_demo.py --consumer <total_orders_per_tenant>
  python execution_orchestration_demo.py --producer <ring_name> <tenant_id> <count>

Start the consumer FIRST -- it owns the SecurityContext, both tenants'
setup, and both rings' creation/unlink, same "the process that should be
up first, is" convention as shm_ipc_bench.cpp/shm_ipc_ingest_demo.cpp.
"""
import sys
import time

from animus import (
    AccessToken,
    ExecStatus,
    OrderRequest,
    OrderSide,
    OrderType,
    Permission,
    AuditOutcome,
    Role,
    SecurityContext,
    ShmOrderRingChannel,
)

TENANT_A = 10
TENANT_B = 20
RING_NAME_A = "AnimusExecDemoTenant10"
RING_NAME_B = "AnimusExecDemoTenant20"
BATCH_SIZE = 256


def _percentile(sorted_samples, pct):
    if not sorted_samples:
        return 0.0
    k = (pct / 100.0) * (len(sorted_samples) - 1)
    f, c = int(k), min(int(k) + 1, len(sorted_samples) - 1)
    if f == c:
        return sorted_samples[int(k)]
    return sorted_samples[f] * (c - k) + sorted_samples[c] * (k - f)


def run_consumer(orders_per_tenant: int) -> int:
    ctx = SecurityContext.create()
    admin = AccessToken.make(tenant_id=0, principal_id=1, role=Role.ADMIN)

    for tenant_id in (TENANT_A, TENANT_B):
        if not ctx.create_tenant(admin, new_tenant_id=tenant_id):
            print(f"[consumer] FAIL: create_tenant({tenant_id}) denied", file=sys.stderr)
            return 1
        if not ctx.create_execution_tenant(admin, tenant_id=tenant_id):
            print(f"[consumer] FAIL: create_execution_tenant({tenant_id}) denied", file=sys.stderr)
            return 1
    print(f"[consumer] tenants {TENANT_A}/{TENANT_B} created and execution-wired", file=sys.stderr)

    rings = {
        TENANT_A: ShmOrderRingChannel.create(RING_NAME_A, capacity=4096),
        TENANT_B: ShmOrderRingChannel.create(RING_NAME_B, capacity=4096),
    }
    print("[consumer] rings ready, waiting for producers...", file=sys.stderr)

    operators = {tid: AccessToken.make(tenant_id=tid, principal_id=100 + tid, role=Role.OPERATOR) for tid in rings}
    filled = {tid: 0 for tid in rings}
    latencies_ns = {tid: [] for tid in rings}
    total_expected = orders_per_tenant * len(rings)
    total_received = 0

    t0 = time.perf_counter_ns()
    while total_received < total_expected:
        made_progress = False
        for tenant_id, ring in rings.items():
            if filled[tenant_id] >= orders_per_tenant:
                continue
            batch = ring.pop_batch(max_count=BATCH_SIZE)
            if not batch:
                continue
            made_progress = True
            token = operators[tenant_id]
            for order in batch:
                call_start = time.perf_counter_ns()
                report = ctx.submit_order(token, order)
                call_end = time.perf_counter_ns()
                latencies_ns[tenant_id].append(call_end - call_start)
                if report is None or ExecStatus(report.status) != ExecStatus.FILLED:
                    print(f"[consumer] FAIL: order {order.client_order_id} for tenant {tenant_id} not filled",
                          file=sys.stderr)
                    return 1
                filled[tenant_id] += 1
                total_received += 1
        if not made_progress:
            time.sleep(0.0005)
    t1 = time.perf_counter_ns()

    elapsed_s = (t1 - t0) / 1e9
    print("=" * 60)
    for tenant_id in rings:
        samples = sorted(latencies_ns[tenant_id])
        print(f"[exec_orchestration] Tenant {tenant_id}: filled {filled[tenant_id]}/{orders_per_tenant}, "
              f"submit_order p50={_percentile(samples, 50):.0f}ns p99={_percentile(samples, 99):.0f}ns "
              f"mean={sum(samples) / len(samples):.0f}ns (Python-observed, ctypes-call-inclusive)")
    throughput = total_received / elapsed_s if elapsed_s > 0 else float("inf")
    print(f"[exec_orchestration] Total: {total_received} orders in {elapsed_s * 1000:.2f} ms "
          f"({throughput:,.0f} orders/sec)")
    print("=" * 60)

    ok = True
    for tenant_id in rings:
        if filled[tenant_id] != orders_per_tenant:
            print(f"[exec_orchestration] FAIL: tenant {tenant_id} filled {filled[tenant_id]}, "
                  f"expected {orders_per_tenant}", file=sys.stderr)
            ok = False

    # Negative-path check: a Viewer token cannot submit orders, and the
    # denial shows up in the audit log -- not just eyeballed, actually
    # verified (same bar as secure_multitenancy_demo.cpp's own checks).
    #
    # poll_execution_audit_log() is FIFO (oldest first), same as every
    # other poll_* in this SDK -- with ~10,000 prior entries already
    # queued from the allowed submit_order() calls above, draining only a
    # handful would return the OLDEST entries, not the denial this check
    # cares about. Fully flush the backlog first so the only entry left
    # after the probe is the probe's own.
    drained = 0
    while True:
        batch = ctx.poll_execution_audit_log(max_count=10_000)
        if not batch:
            break
        drained += len(batch)
    print(f"[exec_orchestration] Flushed {drained} prior audit entries before the negative-path probe")

    viewer = AccessToken.make(tenant_id=TENANT_A, principal_id=999, role=Role.VIEWER)
    probe_order = OrderRequest(client_order_id=999999, instrument_id=1, side=OrderSide.BUY,
                                type=OrderType.MARKET, price_ticks=1, quantity=1)
    denied_report = ctx.submit_order(viewer, probe_order)
    if denied_report is not None:
        print("[exec_orchestration] FAIL: Viewer token was able to submit an order", file=sys.stderr)
        ok = False
    else:
        print("[exec_orchestration] Negative-path check: Viewer submit_order correctly denied")

    audit = ctx.poll_execution_audit_log(max_count=10)
    if len(audit) != 1 or audit[0].tenant_id != TENANT_A or AuditOutcome(audit[0].outcome) != AuditOutcome.DENIED \
            or Permission(audit[0].permission) != Permission.SUBMIT_ORDER:
        print(f"[exec_orchestration] FAIL: expected exactly one Denied/SubmitOrder/tenant={TENANT_A} audit "
              f"entry after the flush, got {[(e.tenant_id, Permission(e.permission).name, AuditOutcome(e.outcome).name) for e in audit]}",
              file=sys.stderr)
        ok = False
    else:
        print("[exec_orchestration] Negative-path check: denial correctly recorded in the audit log")

    print(f"[exec_orchestration] {'End-to-end verification passed.' if ok else 'End-to-end verification FAILED.'}")

    for ring, name in ((rings[TENANT_A], RING_NAME_A), (rings[TENANT_B], RING_NAME_B)):
        ring.close()
        ShmOrderRingChannel.unlink(name)
    ctx.close()
    return 0 if ok else 1


def run_producer(ring_name: str, tenant_id: int, count: int) -> int:
    deadline = time.time() + 5.0
    ring = None
    while time.time() < deadline:
        try:
            ring = ShmOrderRingChannel.open(ring_name)
            break
        except OSError:
            time.sleep(0.01)
    if ring is None:
        print(f"[producer] could not open ring {ring_name!r} within 5s -- start the consumer first", file=sys.stderr)
        return 1

    print(f"[producer] ring {ring_name!r} opened, sending {count} orders for tenant {tenant_id}...", file=sys.stderr)
    sent = 0
    while sent < count:
        batch_len = min(BATCH_SIZE, count - sent)
        batch = [
            OrderRequest(
                client_order_id=sent + i, instrument_id=1, side=OrderSide.BUY if i % 2 == 0 else OrderSide.SELL,
                type=OrderType.MARKET, price_ticks=101250 + i, quantity=10,
            )
            for i in range(batch_len)
        ]
        pushed = ring.push_batch(batch)
        sent += pushed
        if pushed == 0:
            time.sleep(0.0005)
    print(f"[producer] done: sent {sent} orders for tenant {tenant_id}", file=sys.stderr)
    ring.close()
    return 0


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "--consumer":
        return run_consumer(int(sys.argv[2]))
    if len(sys.argv) >= 5 and sys.argv[1] == "--producer":
        return run_producer(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    print(
        "Usage:\n"
        "  python execution_orchestration_demo.py --consumer <total_orders_per_tenant>\n"
        "  python execution_orchestration_demo.py --producer <ring_name> <tenant_id> <count>\n"
        "Launch these as two (or three: one producer per tenant) SEPARATE\n"
        "OS processes, consumer first -- see this file's own module docstring.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
