# CleverCSV Performance Engineering Take-Home

## Task
48-hour take-home: speed up CleverCSV's CSV dialect detection using a 
C++ pybind11 extension. Deliver 3 reproducible Colab notebooks proving 
the speedup on real FEC data.

## Target repo and baseline
- Repo: alan-turing-institute/CleverCSV (MIT, 1.3k stars)
- Baseline commit SHA: ae043c948fd03eea2ae726525c4f347646d22316 (v0.8.3, Dec 2023)
- License: MIT

## Workload
- First 25,000 rows of real FEC 2018 individual contributions
- Pipe-delimited, 21 columns, ~4.5 MB
- File: data/fec_sample.txt in this repo

## Hot path (confirmed via cProfile)
- `clevercsv.consistency.ConsistencyDetector.compute_type_score`
- Called 23 times per detect() invocation (once per dialect candidate)
- Total ~29s of 37s wall-clock at baseline
- Currently uses `self._cached_is_known_type` which wraps regex-bank-per-cell

## Current state
- v1 C++ extension in src/type_detector.cpp — 1.37x speedup, 14.81s median
- v1 signature: `bulk_type_score(rows: List[List[Tuple[str, bool]]], eps: float) -> float`
- v1 bottleneck: marshaling 3.97M Python tuples to C++ pairs per detect()

## Goal
- v2: target 4x+ speedup, ideally 5-8x
- v2 approach: parse CSV inside C++ to eliminate marshaling tax
- Target signature: `bulk_type_score(data: str, delimiter: str, quotechar: str, escapechar: str, eps: float) -> float`

## Constraints (from brief)
- Must produce same SimpleDialect('|', '', '') on FEC workload
- All 164 existing CleverCSV tests must pass
- No upstream patches, no library swaps, no flag flips
- Speedup must come from new work
- Total notebook runtime ≤75 min across 3 notebooks
- Colab CPU only (no GPU)

## Files in this repo
- baseline_colab.ipynb     — done, 20.30s median baseline locked in
- candidate_colab.ipynb    — work in progress (v1 working, v2 pending)
- tests.ipynb              — not started
- README.md                — placeholder
- data/fec_sample.txt      — workload (120k rows, use first 25k)
- src/type_detector.cpp    — v1 extension
- backups/consistency_patched_v1.py — v1 patched consistency.py
- outputs/baseline_output.{json,pkl}, baseline_test_results.json — golden refs

## Build command (Colab)
g++ -O3 -march=native -shared -fPIC -std=c++17 
$(python -m pybind11 --includes) 
src/type_detector.cpp -o type_detector$(python -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))")

## Test command
cd /content/CleverCSV && python -m pytest tests/ -v

## How we measure
- 2 warmup runs (discarded)
- 5 measured runs
- Report median + IQR
- Compute speedup = baseline_median / candidate_median = 20.30 / candidate_median

## Definition of done for v2
- candidate_colab.ipynb runs end-to-end on fresh Colab CPU and reports speedup ≥4x
- C++ score matches Python score within 1e-9 on the FEC workload
- All 164 CleverCSV tests still pass with the patched consistency.py
- src/type_detector.cpp compiles clean with no warnings (besides multichar)
