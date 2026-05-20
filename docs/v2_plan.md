# v2 optimization plan

## Why v1 only got 1.37x
Profile showed `regex.fullmatch` calls disappeared (good) but were
replaced by ~9s of Python→C++ marshaling overhead from materializing
3.97M tuples. Net: 5.5s saved out of a theoretical 20s.

## v2 strategy: parse in C++
Move CSV parsing into C++ so cells are classified inline as the parser
sees them. No intermediate Python list. The signature becomes:

```cpp
double bulk_type_score(
    const std::string& data,
    const std::string& delimiter,
    const std::string& quotechar,
    const std::string& escapechar,
    double eps
);
```

The C++ side does:
1. Walk `data` byte-by-byte, splitting on `delimiter`, respecting
   `quotechar` (if non-empty) and `escapechar` (if non-empty).
2. For each cell, classify in place using string_view (no copy).
3. Increment `total` and `known` counters inline.
4. Return `max(eps, known/total)` after the full pass.

## Parser semantics to mirror
Read /content/CleverCSV/clevercsv/cparser/parser.c for the exact CSV
parsing rules. Key edge cases:
- Empty quotechar means no quoting — just split on delimiter
- Empty escapechar means no escape — backslashes are literal
- Newlines are row separators; \r\n and \n both work
- Empty cells are valid and count as "empty type" (known)

## Inner classifier improvements
- Use string_view to avoid std::string allocations in hot loop
- Reorder predicate checks: cheapest first (is_empty, is_number,
  is_unicode_alphanum cover ~80% of FEC cells)
- Inline character-class lookups via 256-byte tables
- For is_number: single-pass digit/sign/decimal/exponent state machine
- Don't allocate for substring tests (use raw char* + length)

## Build flags
Add -march=native for SIMD auto-vectorization where the compiler finds it.

## Correctness validation
Before timing, prove output equivalence:
- Compute score with original Python implementation
- Compute score with new C++ implementation  
- Assert abs(diff) < 1e-9 for the detected dialect AND for 5 random
  alternate dialects from get_dialects(data)

## Iteration loop
1. Edit src/type_detector.cpp
2. Build in Colab (use a notebook with shell escape, not local — pybind11
   builds need the Colab Python toolchain to match)
3. Run correctness test on FEC data
4. If correctness passes, run timing
5. If speedup <4x, profile (cProfile + perf record if needed) to find
   what's left
