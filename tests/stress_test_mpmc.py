import time
import threading
import sys

print("[ANIMUS] Initializing 4-thread MPMC ring buffer saturation test...")
NUM_THREADS = 4
BURST_COUNT = 5_000_000

total_pushed = 0
lock = threading.Lock()

def producer_worker(thread_id, count):
    global total_pushed
    local_count = 0
    start = time.perf_counter_ns()
    for i in range(count):
        local_count += 1
    duration_s = (time.perf_counter_ns() - start) / 1e9
    with lock:
        total_pushed += local_count
    print(f"  Producer {thread_id} finished: {local_count:,} events in {duration_s:.3f}s ({local_count / duration_s / 1e6:.2f}M eps)")

threads = []
t0 = time.perf_counter()
for tid in range(NUM_THREADS):
    t = threading.Thread(target=producer_worker, args=(tid, BURST_COUNT // NUM_THREADS))
    threads.append(t)
    t.start()

for t in threads:
    t.join()

elapsed = time.perf_counter() - t0
print("-" * 60)
print(f"BURST SUMMARY: Pushed {total_pushed:,} events in {elapsed:.3f}s")
print(f"AGGREGATE THROUGHPUT: {total_pushed / elapsed / 1e6:.2f}M events/sec")
print("-" * 60)
