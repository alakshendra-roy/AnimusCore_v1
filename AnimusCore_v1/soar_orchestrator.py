"""Event-driven SOAR (Security Orchestration, Automation & Response) layer.

Registers each threat signature in config/rules.json as a native in-memory
rule (see Phase 4: EngineImpl::evaluate_rules, animus_add_rule in
animus_engine.cpp) rather than re-implementing signature matching in Python,
then continuously polls animus_poll_signals for matches on a background
thread and dispatches an automated trigger action per match -- the
same "poll on a background thread, never a one-shot post-hoc poll" pattern
used by ingest_engine.py, since the signal ring never blocks and can
saturate silently if left undrained.
"""
import argparse
import json
import os
import threading
import time
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

from animus.bindings import AnimusBindings, RuleComparator, ThreatSignal
from threat_agent import ThreatAgent

DEFAULT_CONFIG_PATH = Path(__file__).parent / "config" / "rules.json"

_COMPARATOR_MAP = {
    "GREATER_THAN": RuleComparator.GREATER_THAN,
    "LESS_THAN": RuleComparator.LESS_THAN,
    "EQUAL": RuleComparator.EQUAL,
}

_SEVERITY_MAP = {
    "LOW": 1,
    "MEDIUM": 2,
    "HIGH": 3,
    "CRITICAL": 4,
}


def load_config(config_path: Path = DEFAULT_CONFIG_PATH) -> Dict[str, Any]:
    if not config_path.exists():
        print(f"[SOAR WARNING] Config file not found at {config_path}. Using fallback defaults.")
        return {"threat_signatures": {}}
    with open(config_path, "r") as f:
        return json.load(f)


# Trigger-action handlers. This process has no real host-isolation or
# process-termination capability, so these simulate the response (structured
# logging) -- but are dispatched by action name so a real integration can be
# dropped in per action without touching the polling/dispatch loop below.
def _action_isolate_host(signal: ThreatSignal, name: str, sig: Dict[str, Any]) -> None:
    print(f"[SOAR ACTION] ISOLATE_HOST      | signature='{name}' severity={sig.get('severity')} trace_id={signal.trace_id} metric_value={signal.metric_value}")


def _action_terminate_process(signal: ThreatSignal, name: str, sig: Dict[str, Any]) -> None:
    print(f"[SOAR ACTION] TERMINATE_PROCESS | signature='{name}' severity={sig.get('severity')} trace_id={signal.trace_id} metric_value={signal.metric_value}")


def _action_default(signal: ThreatSignal, name: str, sig: Dict[str, Any]) -> None:
    print(f"[SOAR ACTION] {sig.get('action', 'UNKNOWN_ACTION')} | signature='{name}' severity={sig.get('severity')} trace_id={signal.trace_id} metric_value={signal.metric_value}")


ACTION_HANDLERS = {
    "ISOLATE_HOST": _action_isolate_host,
    "TERMINATE_PROCESS": _action_terminate_process,
}


class SOAROrchestrator:
    """Registers config-driven threat signatures as native rules and
    continuously drains matched signals into automated trigger actions.
    """

    def __init__(self, bindings: AnimusBindings, config: Dict[str, Any]) -> None:
        self.bindings = bindings
        self.signatures = config.get("threat_signatures", {})
        self._rule_index: Dict[int, Tuple[str, Dict[str, Any]]] = {}
        self.total_signals = 0
        self.actions_triggered = 0
        self._stop = threading.Event()
        self._poller: Optional[threading.Thread] = None

    def register_signatures(self) -> int:
        """Registers each config signature as a native RuleThreshold via
        animus_add_rule. Returns the number of rules successfully registered.
        """
        registered = 0
        for rule_id, (name, sig) in enumerate(self.signatures.items(), start=1):
            event_id = sig.get("event_id")
            threshold = sig.get("threshold")
            comparator_name = str(sig.get("comparator", "EQUAL")).upper()
            if event_id is None or threshold is None:
                print(f"[SOAR WARNING] Signature '{name}' missing event_id/threshold, skipping.")
                continue
            comparator = _COMPARATOR_MAP.get(comparator_name)
            if comparator is None:
                print(f"[SOAR WARNING] Signature '{name}' has unknown comparator '{comparator_name}', skipping.")
                continue
            severity = _SEVERITY_MAP.get(str(sig.get("severity", "LOW")).upper(), 1)
            if not self.bindings.add_rule(rule_id, event_id, threshold, comparator, severity):
                print(f"[SOAR WARNING] Failed to register rule '{name}' (rule_id={rule_id}).")
                continue
            self._rule_index[rule_id] = (name, sig)
            registered += 1
        print(f"[SOAR] Registered {registered} active detection signature(s) as native rules.")
        return registered

    def dispatch(self, signal: ThreatSignal) -> None:
        name, sig = self._rule_index.get(
            signal.rule_id, (f"rule_{signal.rule_id}", {"action": None, "severity": "UNKNOWN"})
        )
        handler = ACTION_HANDLERS.get(sig.get("action"), _action_default)
        handler(signal, name, sig)
        self.actions_triggered += 1

    def _poll_loop(self) -> None:
        while not self._stop.is_set():
            signals = self.bindings.poll_signals(max_count=4096)
            if signals:
                for signal in signals:
                    self.total_signals += 1
                    self.dispatch(signal)
            else:
                time.sleep(0.001)

    def start(self) -> None:
        self._poller = threading.Thread(target=self._poll_loop, daemon=True)
        self._poller.start()

    def stop(self) -> None:
        """Stops the poller, then performs a final drain so any signals
        pushed in the last batch before shutdown aren't left stranded.
        """
        self._stop.set()
        if self._poller:
            self._poller.join()
        trailing = self.bindings.poll_signals(max_count=100_000)
        while trailing:
            for signal in trailing:
                self.total_signals += 1
                self.dispatch(signal)
            trailing = self.bindings.poll_signals(max_count=100_000)


def main() -> None:
    parser = argparse.ArgumentParser(description="AnimusCore_v1 event-driven SOAR orchestration layer")
    parser.add_argument("--events", type=int, default=600_000, help="Synthetic telemetry events to stream through the pipeline")
    parser.add_argument("--ring-capacity", type=int, default=65536, help="Native ring buffer capacity")
    parser.add_argument("--log-file", type=str, default="soar_stream.bin", help="Persistence output path (rule evaluation runs inline on this worker's batches, so logging must be active for signals to be produced)")
    parser.add_argument("--config", type=str, default=str(DEFAULT_CONFIG_PATH), help="Path to threat signature config (rules.json)")
    args = parser.parse_args()

    config = load_config(Path(args.config))

    print("[SOAR] Initializing native engine & linking to C++ core...")
    bindings = AnimusBindings()
    if not bindings.init(args.ring_capacity):
        raise RuntimeError("animus_init failed")

    orchestrator = SOAROrchestrator(bindings, config)
    registered = orchestrator.register_signatures()
    if registered == 0:
        print("[SOAR WARNING] No signatures registered; pipeline will run with no active detections.")

    orchestrator.start()

    if os.path.exists(args.log_file):
        os.remove(args.log_file)
    bindings.start_logging(args.log_file)

    print(f"[SOAR] Streaming {args.events:,} synthetic telemetry events through the pipeline...")
    stream = ThreatAgent.generate_telemetry_batch(args.events)

    start = time.perf_counter_ns()
    accepted = 0
    for event_id, trace_id, metric_value in stream:
        if bindings.record_event(event_id=event_id, trace_id=trace_id, metric_value=metric_value):
            accepted += 1
    end = time.perf_counter_ns()

    # stop_logging() blocks until the persistence worker fully drains the
    # ring buffer, so every event's rule evaluation has already happened by
    # the time it returns; stop() below only needs to drain what's already
    # sitting in the signal ring.
    bindings.stop_logging()
    orchestrator.stop()

    elapsed_ms = (end - start) / 1e6

    print("=" * 60)
    print(f"[SOAR] Events streamed:          {len(stream):,}")
    print(f"[SOAR] Events accepted:          {accepted:,}")
    print(f"[SOAR] Pipeline execution time:  {elapsed_ms:.2f} ms")
    print(f"[SOAR] Threat signals evaluated: {orchestrator.total_signals:,}")
    print(f"[SOAR] Automated actions fired:  {orchestrator.actions_triggered:,}")
    print("=" * 60)

    if accepted and orchestrator.total_signals == 0 and registered > 0:
        raise RuntimeError("SOAR verification failed: rules registered but no signals were ever produced")

    print("[SOAR] Phase 5 Real-Time Orchestration Verified.")


if __name__ == "__main__":
    main()
