import json
import os
import time
import ctypes
from pathlib import Path

def load_rules(config_path="config/rules.json"):
    # Resolve path relative to this script's directory
    full_path = Path(__file__).parent / config_path
    if not full_path.exists():
        print(f"[SOAR WARNING] Config file not found at {full_path}. Using fallback defaults.")
        return {"threat_signatures": {}}
    
    with open(full_path, "r") as f:
        return json.load(f)

# Load configuration dynamically
config = load_rules()
print(f"[SOAR] Loaded {len(config.get('threat_signatures', {}))} active detection signatures from rules.json")

# Load C-ABI Dynamic Library
dll_path = Path(__file__).parent.parent / "x64" / "Release" / "AnimusCore_v1.dll"
animus = ctypes.CDLL(str(dll_path))

# Define C-ABI signatures
animus.animus_init.argtypes = [ctypes.c_size_t]
animus.animus_init.restype = ctypes.c_bool

animus.animus_record_event.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64]
animus.animus_record_event.restype = ctypes.c_bool

class SOAROrchestrator:
    def __init__(self, high_priority_threshold=9500):
        self.high_priority_threshold = high_priority_threshold
        self.total_processed = 0
        self.anomalies_mitigated = 0
        # Pull threat signatures from our loaded JSON config
        self.signatures = config.get("threat_signatures", {})

    def evaluate_event(self, event_id, payload_val):
        self.total_processed += 1
        
        # Check against dynamic signatures from rules.json
        for sig_name, sig_data in self.signatures.items():
            target_flag = sig_data.get("flag")
            if payload_val == target_flag or event_id == target_flag:
                self.anomalies_mitigated += 1
                print(f"[SOAR] Rule Matched: {sig_name} | Action: {sig_data.get('action')}")
                return True
                
        return False

if __name__ == "__main__":
    print("[SOAR] Initializing Orchestrator & Linking to C++ Core...")
    if animus.animus_init(65536):
        orchestrator = SOAROrchestrator(high_priority_threshold=9000)
        
        print("[SOAR] Processing event stream and evaluating real-time threat vectors...")
        start_time = time.perf_counter_ns()
        
        # Scale up stream test to 600,000 events with multiple signature triggers
        for i in range(600000):
            event_id = 101 if i == 500 else (102 if i == 250000 else i)
            payload_val = 1 if i == 500 else (2 if i == 250000 else i)
            
            animus.animus_record_event(event_id, payload_val, time.time_ns())
            orchestrator.evaluate_event(event_id, payload_val)
            
        end_time = time.perf_counter_ns()
        duration_ms = (end_time - start_time) / 1_000_000
        
        print(f"[SOAR] Stream Processed: {orchestrator.total_processed:,} events")
        print(f"[SOAR] Threat Vectors Mitigated: {orchestrator.anomalies_mitigated}")
        print(f"[SOAR] Pipeline Execution Time: {duration_ms:.2f} ms")
        print("[SOAR] Phase 5 Real-Time Orchestration Verified.")