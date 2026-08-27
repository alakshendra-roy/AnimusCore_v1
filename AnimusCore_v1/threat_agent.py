import random
import time

class ThreatAgent:
    """Simulates high-frequency adversary behavior and telemetry anomalies."""
    @staticmethod
    def generate_telemetry_batch(count=50000):
        events = []
        for i in range(count):
            # 2% chance of generating a high-severity threat event
            if random.random() < 0.02:
                events.append((999, i, random.randint(8500, 10000)))  # Critical Threat
            else:
                events.append((101, i, random.randint(100, 2000)))   # Normal Telemetry
        return events

if __name__ == "__main__":
    print("[Threat Agent] Generating synthetic anomaly stream...")
    stream = ThreatAgent.generate_telemetry_batch(10000)
    print(f"[Threat Agent] Generated {len(stream)} events with injected threat vectors.")