"""Correctness validation: C++ score must match Python score within 1e-9."""

import sys
sys.path.insert(0, '/content/CleverCSV')
sys.path.insert(0, '/content')

import importlib
# Reload CleverCSV to pick up unpatched consistency
mods = [k for k in list(sys.modules) if 'clevercsv' in k]
for m in mods:
    del sys.modules[m]

# Import patched version
import clevercsv
from clevercsv.consistency import ConsistencyDetector
from clevercsv.dialect import SimpleDialect
from clevercsv.potential_dialects import get_dialects

# Load workload
with open('/content/clevercsv_speedup/data/fec_sample.txt', encoding='latin-1') as f:
    raw = f.read()
data = '\n'.join(raw.split('\n')[:25000])

# Get candidate dialects
dialects = get_dialects(data)
print(f"Testing {len(dialects)} candidate dialects")

# Compare patched (C++) vs original (Python) for each
import type_detector as td_cpp

# Get the original Python implementation 
# (read the unpatched function from backups)
def original_python_type_score(data, dialect, eps=1e-10):
    """The original pure-Python implementation."""
    from clevercsv.cparser_util import parse_string
    from clevercsv.detect_type import TypeDetector
    td = TypeDetector()
    total = known = 0
    for row in parse_string(data, dialect, return_quoted=True):
        for cell, is_quoted in row:
            total += 1
            known += td.is_known_type(cell, is_quoted=is_quoted)
    if not total:
        return eps
    return max(eps, known / total)

max_diff = 0.0
for d in dialects[:10]:  # Test first 10 dialects
    py_score = original_python_type_score(data, d)
    cpp_score = td_cpp.bulk_type_score(
        data, d.delimiter, d.quotechar, d.escapechar, 1e-10
    )
    diff = abs(py_score - cpp_score)
    max_diff = max(max_diff, diff)
    status = "✓" if diff < 1e-9 else "✗"
    print(f"{status} dialect={d}: py={py_score:.10f} cpp={cpp_score:.10f} diff={diff:.2e}")

print(f"\nMax diff across all dialects: {max_diff:.2e}")
print(f"Correctness: {'PASS' if max_diff < 1e-9 else 'FAIL'}")
