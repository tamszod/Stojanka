#include "core/analysis/indicators.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace strojanka::analysis {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

std::vector<double> closes_of(const std::vector<Bar>& bars) {
    std::vector<double> c;
    c.reserve(bars.size());
    for (const auto& b : bars) c.push_back(b.close);
    return c;
}

std::vector<double> rsi(const std::vector<double>& c, int period) {
    std::vector<double> out(c.size(), kNaN);
    if (c.size() <= static_cast<size_t>(period)) return out;
    double gain = 0, loss = 0;
    for (int i = 1; i <= period; ++i) {
        const double d = c[i] - c[i - 1];
        (d > 0 ? gain : loss) += std::fabs(d);
    }
    gain /= period;
    loss /= period;
    auto value = [](double g, double l) { return l <= 0 ? (g <= 0 ? 50.0 : 100.0) : 100.0 - 100.0 / (1.0 + g / l); };
    out[period] = value(gain, loss);
    for (size_t i = static_cast<size_t>(period) + 1; i < c.size(); ++i) {
        const double d = c[i] - c[i - 1];
        gain = (gain * (period - 1) + (d > 0 ? d : 0)) / period;
        loss = (loss * (period - 1) + (d < 0 ? -d : 0)) / period;
        out[i] = value(gain, loss);
    }
    return out;
}

std::vector<double> percent_b(const std::vector<double>& c, int period, double k) {
    std::vector<double> out(c.size(), kNaN);
    if (c.size() < static_cast<size_t>(period)) return out;
    double sum = 0, sum_sq = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        sum += c[i];
        sum_sq += c[i] * c[i];
        if (i >= static_cast<size_t>(period)) {
            sum -= c[i - period];
            sum_sq -= c[i - period] * c[i - period];
        }
        if (i + 1 >= static_cast<size_t>(period)) {
            const double mean = sum / period;
            const double var = std::max(0.0, sum_sq / period - mean * mean);
            const double sd = std::sqrt(var);
            const double lower = mean - k * sd, upper = mean + k * sd;
            out[i] = upper - lower > 1e-12 ? (c[i] - lower) / (upper - lower) : 0.5;
        }
    }
    return out;
}

double daily_volatility(const std::vector<double>& c, size_t lookback) {
    if (c.size() < 3) return 0.02;
    const size_t start = c.size() - 1 > lookback ? c.size() - 1 - lookback : 0;
    std::vector<double> r;
    for (size_t i = start + 1; i < c.size(); ++i) {
        if (c[i - 1] > 0 && c[i] > 0) r.push_back(std::log(c[i] / c[i - 1]));
    }
    if (r.size() < 2) return 0.02;
    double mean = 0;
    for (double x : r) mean += x;
    mean /= static_cast<double>(r.size());
    double var = 0;
    for (double x : r) var += (x - mean) * (x - mean);
    var /= static_cast<double>(r.size() - 1);
    return std::sqrt(var);
}

Regression log_price_regression(const std::vector<double>& c, size_t from, size_t to) {
    Regression reg;
    to = std::min(to, c.size());
    if (to <= from + 2) return reg;
    const double n = static_cast<double>(to - from);
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (size_t i = from; i < to; ++i) {
        const double x = static_cast<double>(i - from);
        const double y = std::log(std::max(c[i], 1e-9));
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        syy += y * y;
    }
    const double cov = sxy - sx * sy / n;
    const double vx = sxx - sx * sx / n;
    const double vy = syy - sy * sy / n;
    if (vx <= 0) return reg;
    reg.slope = cov / vx;
    reg.r2 = vy > 0 ? (cov * cov) / (vx * vy) : 0;
    return reg;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

double clamp01(double x) { return std::isnan(x) ? 0.0 : std::min(1.0, std::max(0.0, x)); }

double smoothstep(double edge0, double edge1, double x) {
    if (edge0 == edge1) return x >= edge1 ? 1.0 : 0.0;
    const double t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3 - 2 * t);
}

}  // namespace strojanka::analysis
