# AnimusCore live dashboard -- diagnostic snapshot

Captured 2026-09-29 from a dashboard running locally at `http://127.0.0.1:8765/`
(not reachable from outside this machine -- this bundle exists so it can be
inspected without that access).

## What's in this folder

| File | What it is |
|---|---|
| `index.html` | The exact frontend served by `dashboard_server.py` -- open it directly in a browser. It will try to open a WebSocket to `ws://127.0.0.1:8765/ws` on load, which will fail here (no live server behind this file) -- the charts/tiles will just sit at their zero state. It's included so the HTML/CSS/JS itself (layout, chart-drawing code, WebSocket message handling) can be read and reviewed as static source. |
| `websocket_sample_10s.jsonl` | 49 real JSON messages, one per line, captured live over 10 seconds from the running dashboard's `/ws` endpoint (~5/sec, matching the server's 200ms broadcast interval). Each line is one snapshot: `throughput_eps`, `p50_ns`/`p90_ns`/`p99_ns`, `occupancy`/`capacity`, `produced_total`/`consumed_total`/`dropped_total`, `drop_rate_eps`. |
| `benchmark_output.txt` | Fresh stdout from `benchmark_harness.exe`, `soak_harness.exe`, and `python_bridge_test.py` -- the same C++/Python programs the dashboard's live numbers are drawn from, run standalone. |

## Important caveat on the benchmark numbers in this specific file

`benchmark_output.txt` was captured **while the live dashboard was still running**
in the background, competing for CPU cores with these benchmarks. That's why
`benchmark_harness.exe`'s MPMC throughput reads "below target on this hardware"
here (~11M pushes/sec vs. its own 16.5M target) and `soak_harness.exe`'s P50
reads ~65µs instead of the ~3µs measured with nothing else running -- both are
real numbers, just measured under contention, not the pipeline's actual ceiling.
Run these two standalone (dashboard stopped) for a clean number.

## How the pieces relate

`index.html`'s JS opens one WebSocket and, on each message, updates five stat
tiles and redraws two `<canvas>` line charts (throughput; P50/P99 latency) by
appending to a 120-point rolling history array. `websocket_sample_10s.jsonl`
is exactly the sequence of messages that JS is built to consume -- replaying
those lines through `ws.onmessage`'s logic (or just reading the JSON) shows
precisely what drives the "flickering numbers": each field is a live snapshot
of a synthetic, intentionally-saturating C++ producer thread feeding a
lock-free ring buffer, sampled every 200ms. It is not real market/customer
telemetry -- it's the transport benchmark harness itself, visualized live.
