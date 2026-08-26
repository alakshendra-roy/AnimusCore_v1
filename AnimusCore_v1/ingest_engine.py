import json
import time
import os

TELEMETRY_PATH = r"C:\Users\Alaks\source\repos\AnimusCore_v1\AnimusCore_v1\animus_telemetry.json"
CPU_ALERT_THRESHOLD = 80.0  # Alert if CPU exceeds 80%

def stream_telemetry():
    print("[+] Animus Ingestion Engine Active. Monitoring live telemetry...\n")
    
    if not os.path.exists(TELEMETRY_PATH):
        print(f"[-] Waiting for log file at: {TELEMETRY_PATH}")
        return

    with open(TELEMETRY_PATH, "r") as f:
        f.seek(0, os.SEEK_END)
        
        while True:
            line = f.readline()
            if not line:
                time.sleep(0.5)
                continue
            
            try:
                data = json.loads(line.strip())
                cpu = data.get("cpu_usage", 0.0)
                ram_used = data.get("total_ram_mb", 0) - data.get("avail_ram_mb", 0)
                
                # Check for anomaly
                status = "[ALERT - HIGH CPU]" if cpu > CPU_ALERT_THRESHOLD else "[OK]"
                print(f"{status} Time: {data['timestamp']} | CPU: {cpu:.1f}% | RAM Used: {ram_used} MB")
                
            except json.JSONDecodeError:
                continue

if __name__ == "__main__":
    try:
        stream_telemetry()
    except KeyboardInterrupt:
        print("\n[+] Ingestion Engine safely shut down.")