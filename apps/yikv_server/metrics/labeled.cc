#include "metrics/labeled.h"

#include <cstdio>

namespace yikv_server::metrics {

std::string EscapeLabelValue(std::string_view v) {
    std::string out;
    out.reserve(v.size() + 4);
    for (char c : v) {
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '"':  out.append("\\\""); break;
            case '\n': out.append("\\n");  break;
            default:   out.push_back(c);   break;
        }
    }
    return out;
}

static void AppendLabelsBraces(const MetricSample& s, std::string* out) {
    if (s.label_names.empty()) return;
    out->push_back('{');
    for (std::size_t i = 0; i < s.label_names.size(); ++i) {
        if (i) out->push_back(',');
        out->append(s.label_names[i]);
        out->append("=\"");
        out->append(EscapeLabelValue(
            i < s.label_values.size() ? std::string_view(s.label_values[i]) : std::string_view{}));
        out->push_back('"');
    }
    out->push_back('}');
}

static void AppendLabelsBracesWithExtra(const MetricSample& s,
                                        std::string_view extra_name,
                                        std::string_view extra_value,
                                        std::string* out) {
    out->push_back('{');
    bool first = true;
    for (std::size_t i = 0; i < s.label_names.size(); ++i) {
        if (!first) out->push_back(',');
        out->append(s.label_names[i]);
        out->append("=\"");
        out->append(EscapeLabelValue(
            i < s.label_values.size() ? std::string_view(s.label_values[i]) : std::string_view{}));
        out->push_back('"');
        first = false;
    }
    if (!first) out->push_back(',');
    out->append(extra_name);
    out->append("=\"");
    out->append(extra_value);
    out->push_back('"');
    out->push_back('}');
}

static const char* TypeToString(MetricType t) {
    switch (t) {
        case MetricType::Counter: return "counter";
        case MetricType::Gauge:   return "gauge";
        case MetricType::Summary: return "summary";
    }
    return "untyped";
}

void RenderHelpType(const MetricSample& s, std::string* out) {
    out->append("# HELP ");
    out->append(s.name);
    out->push_back(' ');
    // Help text may contain newlines per spec; escape them.
    for (char c : s.help) {
        if (c == '\n')      out->append("\\n");
        else if (c == '\\') out->append("\\\\");
        else                 out->push_back(c);
    }
    out->push_back('\n');

    out->append("# TYPE ");
    out->append(s.name);
    out->push_back(' ');
    out->append(TypeToString(s.type));
    out->push_back('\n');
}

static std::string FormatQuantile(double q) {
    // Stable, locale-independent rendering matching Prometheus conventions.
    char buf[32];
    if (q == 0.0)      return "0";
    if (q == 1.0)      return "1";
    // Strip trailing zeros after the decimal point.
    std::snprintf(buf, sizeof(buf), "%.6f", q);
    std::string s(buf);
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

void RenderSampleLines(const MetricSample& s, std::string* out) {
    if (s.type != MetricType::Summary) {
        out->append(s.name);
        AppendLabelsBraces(s, out);
        out->push_back(' ');
        out->append(std::to_string(s.value));
        out->push_back('\n');
        return;
    }
    // Summary: one line per quantile, plus _count and _sum.
    for (const auto& [q, v] : s.summary.quantiles) {
        out->append(s.name);
        AppendLabelsBracesWithExtra(s, "quantile", FormatQuantile(q), out);
        out->push_back(' ');
        out->append(std::to_string(v));
        out->push_back('\n');
    }
    out->append(s.name);
    out->append("_count");
    AppendLabelsBraces(s, out);
    out->push_back(' ');
    out->append(std::to_string(s.summary.count));
    out->push_back('\n');

    out->append(s.name);
    out->append("_sum");
    AppendLabelsBraces(s, out);
    out->push_back(' ');
    out->append(std::to_string(s.summary.sum));
    out->push_back('\n');
}

void RenderSample(const MetricSample& s, std::string* out) {
    RenderHelpType(s, out);
    RenderSampleLines(s, out);
}

}  // namespace yikv_server::metrics
