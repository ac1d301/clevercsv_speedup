#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <vector>
#include <cctype>

namespace py = pybind11;

static inline bool is_empty(const std::string& s) { return s.empty(); }

static inline bool is_nan(const std::string& s) {
    return s == "nan" || s == "na" || s == "n/a" ||
           s == "NaN" || s == "NA" || s == "N/A" || s == "NAN";
}

static inline bool is_number(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') i++;
    if (i == s.size()) return false;
    bool has_digit = false;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) { has_digit = true; i++; }
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) { has_digit = true; i++; }
    }
    if (!has_digit) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
        if (i == s.size()) return false;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
    }
    return i == s.size();
}

static inline bool is_number_with_commas(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') i++;
    bool has_digit = false;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == ',')) {
        if (std::isdigit((unsigned char)s[i])) has_digit = true;
        i++;
    }
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
    }
    return has_digit && i == s.size();
}

static inline bool is_percentage(const std::string& s) {
    if (s.empty() || s.back() != '%') return false;
    std::string num = s.substr(0, s.size() - 1);
    return is_number(num) || is_number_with_commas(num);
}

static inline bool is_currency(const std::string& s) {
    if (s.empty()) return false;
    if (s[0] != '$') return false;
    return is_number(s.substr(1)) || is_number_with_commas(s.substr(1));
}

static inline bool is_ipv4(const std::string& s) {
    int dots = 0, num = 0, digits = 0;
    for (char c : s) {
        if (std::isdigit((unsigned char)c)) {
            num = num * 10 + (c - '0');
            digits++;
            if (num > 255 || digits > 3) return false;
        } else if (c == '.') {
            if (digits == 0) return false;
            dots++; num = 0; digits = 0;
        } else return false;
    }
    return dots == 3 && digits > 0;
}

static bool is_date_str(const std::string& s) {
    if (s.empty() || !std::isdigit((unsigned char)s[0])) return false;
    int digits = 0, seps = 0;
    char sep = 0;
    for (char c : s) {
        if (std::isdigit((unsigned char)c)) digits++;
        else if (c == '-' || c == '/' || c == '.') {
            if (sep == 0) sep = c;
            else if (sep != c) return false;
            seps++;
        } else return false;
    }
    return digits >= 4 && digits <= 8 && seps == 2;
}

static bool is_time_str(const std::string& s) {
    if (s.empty() || !std::isdigit((unsigned char)s[0])) return false;
    int digits = 0, colons = 0;
    for (char c : s) {
        if (std::isdigit((unsigned char)c)) digits++;
        else if (c == ':') colons++;
        else if (c == '.' || c == 'Z' || c == '+' || c == '-') { }
        else return false;
    }
    return digits >= 4 && colons >= 1 && colons <= 2;
}

static inline bool is_datetime(const std::string& s) {
    if (s.empty() || !std::isdigit((unsigned char)s[0])) return false;
    size_t sep = s.find(' ');
    if (sep == std::string::npos) sep = s.find('T');
    if (sep == std::string::npos) return false;
    return is_date_str(s.substr(0, sep)) && is_time_str(s.substr(sep + 1));
}

static inline bool is_url(const std::string& s) {
    if (s.size() < 7) return false;
    if (s.substr(0, 7) == "http://") return true;
    if (s.size() >= 8 && s.substr(0, 8) == "https://") return true;
    if (s.substr(0, 6) == "ftp://") return true;
    return false;
}

static inline bool is_email(const std::string& s) {
    size_t at = s.find('@');
    if (at == std::string::npos || at == 0 || at == s.size()-1) return false;
    size_t dot = s.find('.', at);
    return dot != std::string::npos && dot != s.size()-1;
}

static inline bool is_unix_path(const std::string& s) { return !s.empty() && s[0] == '/'; }

static inline bool is_bytearray(const std::string& s) {
    return s.size() > 12 && s.substr(0, 11) == "bytearray(b" && s.back() == ')';
}

static inline bool is_json_obj(const std::string& s) {
    return !s.empty() && s.front() == '{' && s.back() == '}';
}

static inline bool is_unicode_alphanum(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (c >= 128) return true;
        if (!std::isalnum(c) && c != ' ' && c != '_' && c != '-') return false;
    }
    return true;
}

static bool is_known_type(const std::string& cell, bool is_quoted) {
    size_t start = 0, end = cell.size();
    while (start < end && std::isspace((unsigned char)cell[start])) start++;
    while (end > start && std::isspace((unsigned char)cell[end-1])) end--;
    std::string s = cell.substr(start, end - start);
    if (is_empty(s))          return true;
    if (is_url(s))            return true;
    if (is_email(s))          return true;
    if (is_ipv4(s))           return true;
    if (is_number(s))         return true;
    if (is_number_with_commas(s)) return true;
    if (is_time_str(s))       return true;
    if (is_percentage(s))     return true;
    if (is_currency(s))       return true;
    if (is_unix_path(s))      return true;
    if (is_nan(s))            return true;
    if (is_date_str(s))       return true;
    if (is_datetime(s))       return true;
    if (is_unicode_alphanum(s)) return true;
    if (is_bytearray(s))      return true;
    if (is_json_obj(s))       return true;
    return false;
}

double bulk_type_score(
    const std::vector<std::vector<std::pair<std::string, bool>>>& rows,
    double eps = 1e-10
) {
    long long total = 0, known = 0;
    for (const auto& row : rows) {
        for (const auto& cell_pair : row) {
            total++;
            if (is_known_type(cell_pair.first, cell_pair.second)) known++;
        }
    }
    if (total == 0) return eps;
    double score = static_cast<double>(known) / total;
    return score < eps ? eps : score;
}

PYBIND11_MODULE(type_detector, m) {
    m.def("bulk_type_score", &bulk_type_score, py::arg("rows"), py::arg("eps") = 1e-10);
    m.def("is_known_type", &is_known_type, py::arg("cell"), py::arg("is_quoted") = false);
}
