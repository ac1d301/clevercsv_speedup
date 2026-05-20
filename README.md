# CleverCSV speedup — C++ pybind11 type scorer

## What I picked and why

Repo: `alan-turing-institute/CleverCSV`, MIT license, ~1.3k stars.
Baseline commit: `ae043c948fd03eea2ae726525c4f347646d22316` — this is v0.8.3, released December 2023, about 17 months old at time of submission.

CleverCSV detects CSV dialects in messy real-world files. The detection algorithm is expensive by design — it scores every candidate dialect against every cell in the file. At this commit the scoring loop was pure Python with no existing C equivalent, which made it a real target rather than a case of just flipping a flag.

## How I found the slow path

Ran cProfile on `Detector().detect(data)` with 25k rows of FEC 2018 campaign finance data. Output was pretty clear:

- `compute_type_score` — 28.9s out of 37.6s total
- `is_known_type` — called 480,869 times
- `regex.Pattern.fullmatch` — 3,973,796 calls

`compute_type_score` runs once per candidate dialect (23 dialects on this file). Each call walks ~480k cells and tries up to 15 compiled regex patterns per cell to figure out the type — number, date, email, URL, etc. That's ~3.97 million regex calls per detect() invocation.

The two existing C extensions in the repo (`cparser` and `cabstraction`) don't touch type scoring at all — I verified this by reading setup.py and checking which .so files exist at the baseline commit. The type scoring loop was entirely pure Python. CleverCSV issues #13 and #15 also document this — users were hitting multi-minute detection times on FEC-style files.

## What I changed

Wrote `src/type_detector.cpp` — a C++ pybind11 extension that replaces the 15-regex Python loop with a character-class state machine. No heap allocation in the hot path. Lookup tables (`g_is_digit`, `g_is_alpha`, `g_is_alnum`) are initialized once at module load. Number detection is a single-pass digit scan. Time validation checks HH/MM/SS ranges. URL and path detection are just prefix byte comparisons.

Then patched `ConsistencyDetector.compute_type_score` in `consistency.py` to call `bulk_type_score(rows, eps)` from the extension instead of the Python loop.

This eliminates the Python dispatch overhead and regex engine startup cost for every cell. The profiler showed those two things eating ~7s per detect() call.

Numbers on standard Colab CPU (Intel Xeon @ 2.20GHz):
- Baseline median: 21.68s
- Candidate median: 14.15s  
- Speedup: 1.53x

## Trade-offs

The `is_unicode_alphanum` check handles ASCII only. High-byte Unicode bytes (≥128) are treated as alphanumeric, which is conservative and works fine for ASCII-dominant files like FEC data. Python's regex library supports full Unicode property classes that the C++ version doesn't replicate for every edge case.

The bigger limitation is marshaling overhead. The extension receives pre-parsed rows as `List[List[Tuple[str, bool]]]` from cparser. Materializing ~480k tuples per dialect call and copying them into C++ vectors costs ~6-7s per detect() — that's what caps the speedup at 1.5x. A v2 that parses CSV inside C++ directly would eliminate this entirely.

Adding a compiled extension increases build complexity, but pybind11 and g++ are both available on Colab without any extra setup steps.

## What I'd do with another week

The main thing is finishing the v2 extension. `src/type_detector_v2.cpp` is already in this repo — it accepts the raw data string and dialect parameters directly and parses CSV internally in C++, eliminating the marshaling cost. It hit a correctness issue with `unicode_alphanum_quoted` semantics on quoted dialects that I ran out of time to resolve properly. Fixing that would likely push the speedup to 4-6x.

After that: SIMD digit scanning for `is_number` (most-called predicate on FEC data), and testing on a wider range of CSV files to make sure the character-class approximations hold outside FEC-style data.

## Caveats

Colab CPU allocation varies between sessions — my baseline ran anywhere from 13s to 22s depending on the VM. To handle this, tests.ipynb measures both baseline and candidate in the same session on the same machine. The speedup ratio stays around 1.5x regardless of which hardware the session gets.

The candidate test suite shows 163 passed, 1 skipped, 0 failed. The skipped test is `test_encoding_cchardet` — it checks encoding detection using the `cchardet` library, which is optional and not installed in standard Colab. The test skips itself cleanly via a decorator. This same test also skips when running against the unpatched baseline on the same Colab environment, so it's not a regression from this patch.

Type score tolerance is 1e-9 absolute. Both implementations do integer counting then divide — no floating point reordering. The FEC workload shows diff = 0.00e+00, exact match.
