How to Use This Tool

Step 1 — Clean Air Baseline
Power on, wait warm-up, watch the values in clean air:
VALUE   | STATUS     | vs THRESHOLD | MIN  | MAX  | AVG
625     | SAFE       |  -275        | 618  | 631  | 624
Note your AVG — that's your real baseline.

Step 2 — Test With Smoke
Wave smoke near the sensor and watch:
625     | SAFE       |  -275        | 618  | 875  | 641
876     | HAZARDOUS  |  -24         | 618  | 876  | 643   ⚠ NEAR THRESHOLD
920     | HAZARDOUS  |  +20         | 618  | 920  | 651   🔴 ABOVE THRESHOLD

Step 3 — Adjust Live with Commands
Type in Serial Monitor without pressing Enter (set to No line ending):
TypeEffect+Raises threshold by 50-Lowers threshold by 50RResets MIN/MAX/AVG stats

Step 4 — Copy Your Final Threshold
Once you find the right number, copy it into your main project:
cppconst int SMOKE_THRESHOLD = 750; // ← your found value here