// type_detector_v2.cpp
//
// v2: parse CSV inside C++ to eliminate Python->C++ marshaling tax.
// Signature: bulk_type_score(data, delimiter, quotechar, escapechar, eps) -> double.
//
// Module name remains "type_detector" so the v1 import path keeps working.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <cstdint>
#include <cctype>

namespace py = pybind11;

// ---------------------------------------------------------------------------
// Character class tables (initialized once)
// ---------------------------------------------------------------------------

static bool g_is_ws[256];
static bool g_is_digit[256];
static bool g_is_alpha[256];
static bool g_is_alnum[256];
static bool g_is_alnum_sep_unquoted[256];  // ASCII separators inside unquoted alphanum: space _ -
static bool g_is_alnum_sep_quoted[256];    // ASCII separators inside quoted alphanum: any printable punct / ws

static void init_tables() {
    for (int i = 0; i < 256; i++) {
        unsigned char c = static_cast<unsigned char>(i);
        g_is_ws[i]     = (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f');
        g_is_digit[i]  = (c >= '0' && c <= '9');
        g_is_alpha[i]  = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
        g_is_alnum[i]  = g_is_digit[i] || g_is_alpha[i];
        g_is_alnum_sep_unquoted[i] = (c == ' ' || c == '_' || c == '-');
        // any printable ASCII punctuation/whitespace; mirrors \p{P}|\s for ASCII
        g_is_alnum_sep_quoted[i]   = (c == ' ' || c == '\t')
            || (c >= 33  && c <= 47)   // ! " # $ % & ' ( ) * + , - . /
            || (c >= 58  && c <= 64)   // : ; < = > ? @
            || (c >= 91  && c <= 96)   // [ \ ] ^ _ `
            || (c >= 123 && c <= 126); // { | } ~
    }
}

// ---------------------------------------------------------------------------
// Cell classifiers — all operate on (const char*, size_t), zero allocations.
// Logic mirrors v1's is_known_type; only the API shape changed.
// ---------------------------------------------------------------------------

static inline bool is_nan(const char* s, size_t n) {
    if (n == 2) {
        if (s[0]=='n' && s[1]=='a') return true;
        if (s[0]=='N' && s[1]=='A') return true;
        return false;
    }
    if (n == 3) {
        if (s[0]=='n' && s[1]=='a' && s[2]=='n') return true;
        if (s[0]=='N' && s[1]=='a' && s[2]=='N') return true;
        if (s[0]=='N' && s[1]=='A' && s[2]=='N') return true;
        if (s[0]=='N' && s[1]=='/' && s[2]=='A') return true;
        if (s[0]=='n' && s[1]=='/' && s[2]=='a') return true;
        return false;
    }
    return false;
}

static inline bool is_number(const char* s, size_t n) {
    if (n == 0) return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') i++;
    if (i == n) return false;
    bool has_digit = false;
    while (i < n && g_is_digit[(unsigned char)s[i]]) { has_digit = true; i++; }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && g_is_digit[(unsigned char)s[i]]) { has_digit = true; i++; }
    }
    if (!has_digit) return false;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        size_t exp_start = i;
        while (i < n && g_is_digit[(unsigned char)s[i]]) i++;
        if (i == exp_start) return false;
    }
    return i == n;
}

static inline bool is_number_with_commas(const char* s, size_t n) {
    if (n == 0) return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') i++;
    bool has_digit = false;
    while (i < n && (g_is_digit[(unsigned char)s[i]] || s[i] == ',')) {
        if (g_is_digit[(unsigned char)s[i]]) has_digit = true;
        i++;
    }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && g_is_digit[(unsigned char)s[i]]) i++;
    }
    return has_digit && i == n;
}

static inline bool is_percentage(const char* s, size_t n) {
    if (n == 0 || s[n-1] != '%') return false;
    return is_number(s, n-1) || is_number_with_commas(s, n-1);
}

static inline bool is_currency(const char* s, size_t n) {
    if (n < 2 || s[0] != '$') return false;
    return is_number(s + 1, n - 1) || is_number_with_commas(s + 1, n - 1);
}

static inline bool is_ipv4(const char* s, size_t n) {
    int dots = 0, num = 0, digits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (g_is_digit[(unsigned char)c]) {
            num = num * 10 + (c - '0');
            digits++;
            if (num > 255 || digits > 3) return false;
        } else if (c == '.') {
            if (digits == 0) return false;
            dots++; num = 0; digits = 0;
        } else {
            return false;
        }
    }
    return dots == 3 && digits > 0;
}

static inline bool is_date_str(const char* s, size_t n) {
    if (n == 0 || !g_is_digit[(unsigned char)s[0]]) return false;
    int digits = 0, seps = 0;
    char sep = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (g_is_digit[(unsigned char)c]) digits++;
        else if (c == '-' || c == '/' || c == '.') {
            if (sep == 0) sep = c;
            else if (sep != c) return false;
            seps++;
        } else {
            return false;
        }
    }
    return digits >= 4 && digits <= 8 && seps == 2;
}

static inline bool is_time_str(const char* s, size_t n) {
    if (n == 0 || !g_is_digit[(unsigned char)s[0]]) return false;
    int digits = 0, colons = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (g_is_digit[(unsigned char)c]) digits++;
        else if (c == ':') colons++;
        else if (c == '.' || c == 'Z' || c == '+' || c == '-') { /* ok */ }
        else return false;
    }
    return digits >= 4 && colons >= 1 && colons <= 2;
}

static inline bool is_datetime(const char* s, size_t n) {
    if (n == 0 || !g_is_digit[(unsigned char)s[0]]) return false;
    size_t sep = n;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ' ' || s[i] == 'T') { sep = i; break; }
    }
    if (sep == n) return false;
    return is_date_str(s, sep) && is_time_str(s + sep + 1, n - sep - 1);
}

static inline bool is_url(const char* s, size_t n) {
    if (n >= 7  && s[0]=='h' && s[1]=='t' && s[2]=='t' && s[3]=='p' && s[4]==':' && s[5]=='/' && s[6]=='/') return true;
    if (n >= 8  && s[0]=='h' && s[1]=='t' && s[2]=='t' && s[3]=='p' && s[4]=='s' && s[5]==':' && s[6]=='/' && s[7]=='/') return true;
    if (n >= 6  && s[0]=='f' && s[1]=='t' && s[2]=='p' && s[3]==':' && s[4]=='/' && s[5]=='/') return true;
    return false;
}

static inline bool is_email(const char* s, size_t n) {
    size_t at = n;
    for (size_t i = 0; i < n; i++) if (s[i] == '@') { at = i; break; }
    if (at == n || at == 0 || at + 1 >= n) return false;
    for (size_t i = at + 1; i + 1 < n; i++) {
        if (s[i] == '.') return true;
    }
    return false;
}

static inline bool is_unix_path(const char* s, size_t n) {
    return n > 0 && s[0] == '/';
}

static inline bool is_bytearray(const char* s, size_t n) {
    if (n <= 12 || s[n-1] != ')') return false;
    static const char prefix[] = "bytearray(b";
    for (int i = 0; i < 11; i++) if (s[i] != prefix[i]) return false;
    return true;
}

static inline bool is_json_obj(const char* s, size_t n) {
    return n >= 2 && s[0] == '{' && s[n-1] == '}';
}

static inline bool is_unicode_alphanum(const char* s, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 128) continue;  // any high-bit byte counts as letter
        if (!g_is_alnum[c] && !g_is_alnum_sep_unquoted[c]) return false;
    }
    return true;
}

static inline bool is_unicode_alphanum_quoted(const char* s, size_t n) {
    // Quoted regex permits punctuation/whitespace between alphanum runs.
    // Approximation: any ASCII alnum/printable-punct/ws, with at least one alnum (or high-bit) char.
    if (n == 0) return false;
    bool any_alnum = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 128)        { any_alnum = true; continue; }
        if (g_is_alnum[c])   { any_alnum = true; continue; }
        if (g_is_alnum_sep_quoted[c]) continue;
        return false;
    }
    return any_alnum;
}

// ---------------------------------------------------------------------------
// Dispatcher: strip whitespace, first-char fast-paths, then full chain.
// is_quoted selects the unquoted vs. quoted alphanum check.
// ---------------------------------------------------------------------------

static inline bool is_known_type_sv(const char* s, size_t n, bool is_quoted) {
    // ASCII whitespace strip (matches v1 / Python str.strip for ASCII WS)
    while (n > 0 && g_is_ws[(unsigned char)s[0]])     { s++; n--; }
    while (n > 0 && g_is_ws[(unsigned char)s[n-1]])   { n--; }

    if (n == 0) return true;  // is_empty

    unsigned char c0 = (unsigned char)s[0];

    // First-char dispatch: try the most likely match for this first byte
    if (g_is_digit[c0]) {
        if (is_number(s, n)) return true;
    } else if (g_is_alpha[c0] || c0 >= 128) {
        if (is_quoted ? is_unicode_alphanum_quoted(s, n)
                      : is_unicode_alphanum(s, n)) return true;
    }

    // Full fallback chain (same order as v1 is_known_type)
    if (is_url(s, n))            return true;
    if (is_email(s, n))          return true;
    if (is_ipv4(s, n))           return true;
    if (is_number(s, n))         return true;
    if (is_number_with_commas(s, n)) return true;
    if (is_time_str(s, n))       return true;
    if (is_percentage(s, n))     return true;
    if (is_currency(s, n))       return true;
    if (is_unix_path(s, n))      return true;
    if (is_nan(s, n))            return true;
    if (is_date_str(s, n))       return true;
    if (is_datetime(s, n))       return true;
    if (is_quoted ? is_unicode_alphanum_quoted(s, n)
                  : is_unicode_alphanum(s, n)) return true;
    if (is_bytearray(s, n))      return true;
    if (is_json_obj(s, n))       return true;
    return false;
}

static inline void classify_emit(const char* s, size_t n, bool is_quoted,
                                  int64_t& total, int64_t& known) {
    total++;
    if (is_known_type_sv(s, n, is_quoted)) known++;
}

// ---------------------------------------------------------------------------
// FAST PATH: empty quotechar AND empty escapechar.
// Just split on delimiter and newline. Every cell is a string_view into data.
// If delimiter is also empty, every newline-bounded chunk is one cell.
// ---------------------------------------------------------------------------

static void run_fast_path(const char* data, size_t len,
                          char delim, bool no_delim,
                          int64_t& total, int64_t& known)
{
    const char* p   = data;
    const char* end = data + len;
    const char* cell_start = p;
    bool row_has_pending = false;

    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (!no_delim && c == (unsigned char)delim) {
            classify_emit(cell_start, (size_t)(p - cell_start), false, total, known);
            row_has_pending = true;
            p++;
            cell_start = p;
        } else if (c == '\n') {
            if (row_has_pending || p > cell_start) {
                classify_emit(cell_start, (size_t)(p - cell_start), false, total, known);
                row_has_pending = false;
            }
            p++;
            cell_start = p;
        } else if (c == '\r') {
            if (row_has_pending || p > cell_start) {
                classify_emit(cell_start, (size_t)(p - cell_start), false, total, known);
                row_has_pending = false;
            }
            p++;
            if (p < end && *p == '\n') p++;
            cell_start = p;
        } else {
            p++;
        }
    }
    // EOF flush: pending cell content, or trailing-delimiter empty cell
    if (cell_start < end || row_has_pending) {
        classify_emit(cell_start, (size_t)(end - cell_start), false, total, known);
    }
}

// ---------------------------------------------------------------------------
// GENERAL PATH: handles quotechar and/or escapechar.
// State machine: FIELD_START, IN_FIELD, IN_QUOTED, QUOTED_END_MAYBE.
// Only allocates scratch when an escape or doubled quote is actually encountered.
// Tracks is_quoted per cell and passes it to the classifier.
// ---------------------------------------------------------------------------

enum ParseState { FIELD_START, IN_FIELD, IN_QUOTED, QUOTED_END_MAYBE };

static void run_general_path(const char* data, size_t len,
                              char delim, bool no_delim,
                              char quote, bool no_quote,
                              char esc,   bool no_escape,
                              int64_t& total, int64_t& known)
{
    const char* p   = data;
    const char* end = data + len;
    const char* cell_start = p;

    ParseState state = FIELD_START;
    bool cell_is_quoted = false;
    bool row_has_pending = false;
    bool needs_unescape = false;
    std::string scratch;
    scratch.reserve(64);

    auto emit = [&](const char* s, size_t n) {
        if (needs_unescape) {
            classify_emit(scratch.data(), scratch.size(), cell_is_quoted, total, known);
            scratch.clear();
            needs_unescape = false;
        } else {
            classify_emit(s, n, cell_is_quoted, total, known);
        }
        cell_is_quoted = false;
        row_has_pending = true;
    };

    while (p < end) {
        unsigned char c = (unsigned char)*p;

        switch (state) {
        case FIELD_START: {
            if (!no_delim && c == (unsigned char)delim) {
                emit(p, 0);
                p++;
                cell_start = p;
                continue;
            }
            if (c == '\n') {
                if (row_has_pending) { emit(p, 0); row_has_pending = false; }
                p++;
                cell_start = p;
                continue;
            }
            if (c == '\r') {
                if (row_has_pending) { emit(p, 0); row_has_pending = false; }
                p++;
                if (p < end && *p == '\n') p++;
                cell_start = p;
                continue;
            }
            if (!no_quote && c == (unsigned char)quote) {
                cell_is_quoted = true;
                state = IN_QUOTED;
                p++;
                cell_start = p;
                continue;
            }
            // unquoted field begins at this byte; do not advance p, let IN_FIELD process it
            cell_is_quoted = false;
            state = IN_FIELD;
            cell_start = p;
            continue;
        }

        case IN_FIELD: {
            if (!no_escape && c == (unsigned char)esc) {
                if (!needs_unescape) {
                    scratch.assign(cell_start, (size_t)(p - cell_start));
                    needs_unescape = true;
                }
                p++;
                if (p < end) {
                    scratch.push_back(*p);
                    p++;
                }
                continue;
            }
            if (!no_delim && c == (unsigned char)delim) {
                emit(cell_start, (size_t)(p - cell_start));
                state = FIELD_START;
                p++;
                cell_start = p;
                continue;
            }
            if (c == '\n') {
                emit(cell_start, (size_t)(p - cell_start));
                row_has_pending = false;
                state = FIELD_START;
                p++;
                cell_start = p;
                continue;
            }
            if (c == '\r') {
                emit(cell_start, (size_t)(p - cell_start));
                row_has_pending = false;
                state = FIELD_START;
                p++;
                if (p < end && *p == '\n') p++;
                cell_start = p;
                continue;
            }
            if (needs_unescape) scratch.push_back(*p);
            p++;
            continue;
        }

        case IN_QUOTED: {
            if (!no_escape && c == (unsigned char)esc) {
                if (!needs_unescape) {
                    scratch.assign(cell_start, (size_t)(p - cell_start));
                    needs_unescape = true;
                }
                p++;
                if (p < end) {
                    scratch.push_back(*p);
                    p++;
                }
                continue;
            }
            if (!no_quote && c == (unsigned char)quote) {
                state = QUOTED_END_MAYBE;
                p++;
                continue;
            }
            if (needs_unescape) scratch.push_back(*p);
            p++;
            continue;
        }

        case QUOTED_END_MAYBE: {
            if (!no_quote && c == (unsigned char)quote) {
                // doubled quote: literal quote inside the field
                if (!needs_unescape) {
                    // capture content up to (but not including) the first closing quote at p-1
                    scratch.assign(cell_start, (size_t)((p - 1) - cell_start));
                    needs_unescape = true;
                }
                scratch.push_back((char)quote);
                state = IN_QUOTED;
                p++;
                continue;
            }
            if (!no_delim && c == (unsigned char)delim) {
                if (needs_unescape) emit(nullptr, 0);
                else emit(cell_start, (size_t)((p - 1) - cell_start));
                state = FIELD_START;
                p++;
                cell_start = p;
                continue;
            }
            if (c == '\n') {
                if (needs_unescape) emit(nullptr, 0);
                else emit(cell_start, (size_t)((p - 1) - cell_start));
                row_has_pending = false;
                state = FIELD_START;
                p++;
                cell_start = p;
                continue;
            }
            if (c == '\r') {
                if (needs_unescape) emit(nullptr, 0);
                else emit(cell_start, (size_t)((p - 1) - cell_start));
                row_has_pending = false;
                state = FIELD_START;
                p++;
                if (p < end && *p == '\n') p++;
                cell_start = p;
                continue;
            }
            // Stray content after a closing quote: drop back into IN_FIELD treating
            // the closing quote and trailing bytes as plain field text. Don't change
            // cell_start so the original quoted text is included.
            state = IN_FIELD;
            continue;
        }
        }
    }

    // EOF flush
    switch (state) {
    case FIELD_START:
        if (row_has_pending) emit(end, 0);
        break;
    case IN_FIELD:
        if (needs_unescape) emit(nullptr, 0);
        else emit(cell_start, (size_t)(end - cell_start));
        break;
    case IN_QUOTED:
        // unterminated quoted field — emit whatever we have
        if (needs_unescape) emit(nullptr, 0);
        else emit(cell_start, (size_t)(end - cell_start));
        break;
    case QUOTED_END_MAYBE:
        if (needs_unescape) emit(nullptr, 0);
        else emit(cell_start, (size_t)((end - 1) - cell_start));
        break;
    }
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

double bulk_type_score(
    const std::string& data,
    const std::string& delimiter,
    const std::string& quotechar,
    const std::string& escapechar,
    double eps)
{
    bool no_delim  = delimiter.empty();
    bool no_quote  = quotechar.empty();
    bool no_escape = escapechar.empty();

    char delim = no_delim  ? '\0' : delimiter[0];
    char quote = no_quote  ? '\0' : quotechar[0];
    char esc   = no_escape ? '\0' : escapechar[0];

    int64_t total = 0, known = 0;

    if (no_quote && no_escape) {
        run_fast_path(data.data(), data.size(), delim, no_delim, total, known);
    } else {
        run_general_path(data.data(), data.size(),
                         delim, no_delim,
                         quote, no_quote,
                         esc,   no_escape,
                         total, known);
    }

    if (total == 0) return eps;
    double score = static_cast<double>(known) / static_cast<double>(total);
    return score < eps ? eps : score;
}

// Kept for parity with v1 callers (notebooks, debugging).
static bool is_known_type_str(const std::string& cell, bool is_quoted) {
    return is_known_type_sv(cell.data(), cell.size(), is_quoted);
}

PYBIND11_MODULE(type_detector, m) {
    init_tables();
    m.doc() = "CleverCSV v2 type-score extension (parsing + classification in C++)";
    m.def("bulk_type_score", &bulk_type_score,
          py::arg("data"),
          py::arg("delimiter"),
          py::arg("quotechar"),
          py::arg("escapechar"),
          py::arg("eps") = 1e-10);
    m.def("is_known_type", &is_known_type_str,
          py::arg("cell"), py::arg("is_quoted") = false);
}
