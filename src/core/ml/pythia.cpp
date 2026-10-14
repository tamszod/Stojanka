#include "core/ml/pythia.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/strings.h"
#include "core/analysis/analyzer.h"
#include "core/analysis/indicators.h"
#include "core/log.h"

namespace strojanka::ml {

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr int kFirstBar = 260;        // a year of history before the first sample
constexpr int64_t kGapSeconds = 35 * 86400;  // training stops this long before a test period
constexpr int kTrainingYears = 3;
constexpr int kAnnotateBars = 300;
constexpr const char* kModelName = "pythia";

double move_for(double vol20) { return std::clamp(2.5 * vol20, 0.015, 0.15); }

std::string date_minus(const std::string& date, int64_t seconds) { return unix_to_date(iso_to_unix(date) - seconds); }

std::vector<double> rolling_vol(const std::vector<double>& c, int n) {
    std::vector<double> out(c.size(), std::nan(""));
    for (size_t i = static_cast<size_t>(n); i < c.size(); ++i) {
        double mean = 0, sq = 0;
        for (size_t k = i - n + 1; k <= i; ++k) mean += std::log(c[k] / c[k - 1]);
        mean /= n;
        for (size_t k = i - n + 1; k <= i; ++k) {
            const double r = std::log(c[k] / c[k - 1]) - mean;
            sq += r * r;
        }
        out[i] = std::sqrt(sq / (n - 1));
    }
    return out;
}

GbdtOptions fold_options() {
    GbdtOptions o;
    o.trees = 100;
    return o;
}

nlohmann::json report_json(const PythiaReport& r) {
    return {{"trained_at", r.trained_at}, {"data_from", r.data_from}, {"data_to", r.data_to}, {"tested_from", r.tested_from},
            {"samples", r.samples}, {"oos_samples", r.oos_samples}, {"folds", r.folds}, {"base_up", r.base_up},
            {"base_down", r.base_down}, {"auc_up", r.auc_up}, {"auc_down", r.auc_down}, {"top_up", r.top_up},
            {"top_down", r.top_down}, {"seconds", r.seconds}};
}

PythiaReport report_from(const nlohmann::json& j) {
    PythiaReport r;
    r.trained_at = j.value("trained_at", std::string());
    r.data_from = j.value("data_from", std::string());
    r.data_to = j.value("data_to", std::string());
    r.tested_from = j.value("tested_from", std::string());
    r.samples = j.value("samples", 0);
    r.oos_samples = j.value("oos_samples", 0);
    r.folds = j.value("folds", 0);
    r.base_up = j.value("base_up", 0.0);
    r.base_down = j.value("base_down", 0.0);
    r.auc_up = j.value("auc_up", 0.5);
    r.auc_down = j.value("auc_down", 0.5);
    r.top_up = j.value("top_up", 0.0);
    r.top_down = j.value("top_down", 0.0);
    r.seconds = j.value("seconds", 0.0);
    return r;
}

double top_decile_rate(std::vector<std::pair<double, uint8_t>> scored) {
    if (scored.empty()) return 0;
    std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    const size_t k = std::max<size_t>(1, scored.size() / 10);
    double hits = 0;
    for (size_t i = 0; i < k; ++i) hits += scored[i].second;
    return hits / static_cast<double>(k);
}

}  // namespace

const std::vector<std::string>& Pythia::feature_names() {
    static const std::vector<std::string> names = {
        "return 1d", "return 5d", "return 20d", "return 60d", "return 120d", "volatility 20d", "volatility ratio 20/120",
        "RSI 14", "RSI 2", "Bollinger %B", "vs 20d average", "vs 50d average", "vs 200d average", "50d vs 200d average",
        "peak score", "support score", "swing direction", "from 52-week high", "from 52-week low", "volume vs 20d average",
        "market return 20d", "market return 60d", "share of stocks above 200d average", "return 60d vs market",
        "return 20d in volatility units"};
    return names;
}

Pythia::Context Pythia::market_context(const arena::Universe& u) {
    Context ctx;
    for (size_t d = 0; d < u.calendar.size(); ++d) {
        double r20 = 0, r60 = 0, above = 0;
        int n20 = 0, n60 = 0, nb = 0;
        for (const auto& s : u.symbols) {
            const int bi = s.at[d];
            if (bi < 0) continue;
            const double c = s.bars[bi].close;
            if (bi >= 20) {
                r20 += c / s.bars[bi - 20].close - 1;
                ++n20;
            }
            if (bi >= 60) {
                r60 += c / s.bars[bi - 60].close - 1;
                ++n60;
            }
            if (!std::isnan(s.sma200[bi])) {
                above += c > s.sma200[bi];
                ++nb;
            }
        }
        ctx[u.calendar[d]] = {n20 ? static_cast<float>(r20 / n20) : kNaN, n60 ? static_cast<float>(r60 / n60) : kNaN,
                              nb ? static_cast<float>(above / nb) : kNaN};
    }
    return ctx;
}

std::vector<std::vector<float>> Pythia::features(const arena::SymbolData& s, const Context& context) {
    const auto closes = analysis::closes_of(s.bars);
    const auto rsi14 = analysis::rsi(closes, 14);
    const auto pctb = analysis::percent_b(closes, 20, 2.0);
    const auto vol20 = rolling_vol(closes, 20);
    const auto vol120 = rolling_vol(closes, 120);
    const size_t n = s.bars.size();
    std::vector<std::vector<float>> rows(n, std::vector<float>(feature_names().size(), kNaN));
    auto f = [](double v) { return std::isfinite(v) ? static_cast<float>(v) : kNaN; };
    for (size_t i = 0; i < n; ++i) {
        auto& x = rows[i];
        const double c = closes[i];
        auto ret = [&](size_t k) { return i >= k ? c / closes[i - k] - 1 : std::nan(""); };
        x[0] = f(ret(1));
        x[1] = f(ret(5));
        x[2] = f(ret(20));
        x[3] = f(ret(60));
        x[4] = f(ret(120));
        x[5] = f(vol20[i]);
        x[6] = f(vol20[i] / vol120[i]);
        x[7] = f(rsi14[i]);
        x[8] = f(s.rsi2[i]);
        x[9] = f(pctb[i]);
        x[10] = f(c / s.sma20[i] - 1);
        x[11] = f(c / s.sma50[i] - 1);
        x[12] = f(c / s.sma200[i] - 1);
        x[13] = f(s.sma50[i] / s.sma200[i] - 1);
        x[14] = f(s.peak_score[i]);
        x[15] = f(s.support_score[i]);
        x[16] = static_cast<float>(s.direction[i]);
        if (i >= 251) {
            double hi = 0, lo = 1e300;
            for (size_t k = i - 251; k <= i; ++k) {
                hi = std::max(hi, s.bars[k].high > 0 ? s.bars[k].high : s.bars[k].close);
                lo = std::min(lo, s.bars[k].low > 0 ? s.bars[k].low : s.bars[k].close);
            }
            x[17] = f(c / hi - 1);
            x[18] = f(c / lo - 1);
        }
        if (i >= 20) {
            double avg = 0;
            for (size_t k = i - 20; k < i; ++k) avg += s.bars[k].volume;
            avg /= 20;
            x[19] = f(std::log((s.bars[i].volume + 1) / (avg + 1)));
        }
        const auto it = context.find(s.bars[i].date);
        if (it != context.end()) {
            x[20] = it->second[0];
            x[21] = it->second[1];
            x[22] = it->second[2];
            x[23] = f(ret(60) - it->second[1]);
        }
        x[24] = f(ret(20) / (vol20[i] * std::sqrt(20.0)));
    }
    return rows;
}

bool Pythia::load() {
    const auto stored = repo_.load_model(kModelName);
    if (!stored) return false;
    const auto models = nlohmann::json::parse((*stored)[0], nullptr, false);
    if (!models.is_object()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!up_.from_json(models.value("up", std::string())) || !down_.from_json(models.value("down", std::string()))) return false;
    report_ = report_from(nlohmann::json::parse((*stored)[1], nullptr, false));
    context_.clear();
    for (const auto& row : models.value("context", nlohmann::json::array())) {
        auto num = [](const nlohmann::json& v) { return v.is_number() ? v.get<float>() : kNaN; };
        context_[row[0].get<std::string>()] = {num(row[1]), num(row[2]), num(row[3])};
    }
    return true;
}

bool Pythia::ready() {
    std::lock_guard<std::mutex> lock(mutex_);
    return up_.trained() && down_.trained();
}

void Pythia::train(arena::Universe& u) {
    const auto started = std::chrono::steady_clock::now();
    const Context ctx = market_context(u);

    struct Sample {
        int sym, bi, year;
        uint8_t up, down;
        bool labelled;
    };
    std::vector<std::vector<std::vector<float>>> feats(u.symbols.size());
    std::vector<Sample> samples;
    for (size_t si = 0; si < u.symbols.size(); ++si) {
        const auto& s = u.symbols[si];
        feats[si] = features(s, ctx);
        const auto closes = analysis::closes_of(s.bars);
        for (int bi = kFirstBar; bi < static_cast<int>(s.bars.size()); ++bi) {
            const float vol = feats[si][bi][5];
            if (std::isnan(vol)) continue;
            const double move = move_for(vol);
            const auto up = analysis::evaluate_outcome(s.bars, bi, closes[bi], SignalSide::Support, move, kHorizonDays);
            const auto down = analysis::evaluate_outcome(s.bars, bi, closes[bi], SignalSide::Peak, move, kHorizonDays);
            const bool labelled = up != SignalOutcome::Pending && down != SignalOutcome::Pending;
            samples.push_back({static_cast<int>(si), bi, std::stoi(s.bars[bi].date.substr(0, 4)),
                               static_cast<uint8_t>(up == SignalOutcome::Correct), static_cast<uint8_t>(down == SignalOutcome::Correct),
                               labelled});
        }
    }
    if (samples.empty()) return;
    auto date_of = [&](const Sample& x) -> const std::string& { return u.symbols[x.sym].bars[x.bi].date; };
    int min_year = 9999, max_year = 0;
    for (const auto& x : samples) {
        min_year = std::min(min_year, x.year);
        max_year = std::max(max_year, x.year);
    }

    auto train_pair = [&](const std::string& cutoff, const GbdtOptions& options, Gbdt& up, Gbdt& down) {
        std::vector<std::vector<float>> X;
        std::vector<uint8_t> yu, yd;
        for (const auto& x : samples) {
            if (!x.labelled || date_of(x) >= cutoff) continue;
            X.push_back(feats[x.sym][x.bi]);
            yu.push_back(x.up);
            yd.push_back(x.down);
        }
        if (X.size() < 5000) return false;
        up.train(X, yu, options);
        down.train(X, yd, options);
        return true;
    };

    // Walk forward: each test year is predicted by models that never saw it.
    std::vector<std::pair<double, uint8_t>> oos_up, oos_down;
    PythiaReport r;
    for (int year = min_year + kTrainingYears; year <= max_year; ++year) {
        const std::string year_start = std::to_string(year) + "-01-01";
        Gbdt up, down;
        if (!train_pair(date_minus(year_start, kGapSeconds), fold_options(), up, down)) continue;
        ++r.folds;
        if (r.tested_from.empty()) r.tested_from = year_start;
        for (const auto& x : samples) {
            if (x.year != year) continue;
            const auto& row = feats[x.sym][x.bi];
            const double pu = up.predict(row), pd = down.predict(row);
            u.symbols[x.sym].oracle_up[x.bi] = pu;
            u.symbols[x.sym].oracle_down[x.bi] = pd;
            if (x.labelled) {
                oos_up.emplace_back(pu, x.up);
                oos_down.emplace_back(pd, x.down);
            }
        }
    }

    // Final models for live use: everything except the last weeks, whose outcomes are not known yet.
    std::string last_date;
    for (const auto& x : samples) last_date = std::max(last_date, date_of(x));
    Gbdt up, down;
    if (!train_pair(date_minus(last_date, kGapSeconds), GbdtOptions{}, up, down)) return;

    std::vector<double> su, sd;
    std::vector<uint8_t> lu, ld;
    for (const auto& [p, l] : oos_up) {
        su.push_back(p);
        lu.push_back(l);
        r.base_up += l;
    }
    for (const auto& [p, l] : oos_down) {
        sd.push_back(p);
        ld.push_back(l);
        r.base_down += l;
    }
    r.oos_samples = static_cast<int>(su.size());
    if (!su.empty()) {
        r.base_up /= static_cast<double>(su.size());
        r.base_down /= static_cast<double>(sd.size());
        r.auc_up = auc(su, lu);
        r.auc_down = auc(sd, ld);
        r.top_up = top_decile_rate(oos_up);
        r.top_down = top_decile_rate(oos_down);
    }
    r.samples = static_cast<int>(samples.size());
    r.data_from = u.calendar.empty() ? "" : u.calendar.front();
    r.data_to = last_date;
    r.trained_at = utc_now_iso();
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        up_ = std::move(up);
        down_ = std::move(down);
        report_ = r;
        context_ = ctx;
    }
    // The recent market context travels with the model, so predictions after a restart match.
    nlohmann::json context = nlohmann::json::array();
    for (auto it = ctx.size() > 400 ? std::prev(ctx.end(), 400) : ctx.begin(); it != ctx.end(); ++it) {
        context.push_back({it->first, it->second[0], it->second[1], it->second[2]});
    }
    repo_.save_model(kModelName, nlohmann::json{{"up", up_.to_json()}, {"down", down_.to_json()}, {"context", context}}.dump(),
                     report_json(r).dump());
    log_info("pythia: trained on " + std::to_string(r.samples) + " stock-days in " + format_double(r.seconds, 1) +
             " s; on unseen years its top 10% rise calls hit " + format_percent(r.top_up, 0) + " vs " +
             format_percent(r.base_up, 0) + " normally (AUC " + format_double(r.auc_up, 3) + ")");
}

void Pythia::annotate(arena::Universe& u) {
    if (!ready()) return;
    const Context ctx = market_context(u);
    std::vector<std::pair<std::string, Prediction>> picks;
    const std::string last_day = u.calendar.empty() ? "" : u.calendar.back();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& s : u.symbols) {
        const auto rows = features(s, ctx);
        const int from = std::max(kFirstBar, static_cast<int>(s.bars.size()) - kAnnotateBars);
        for (int bi = from; bi < static_cast<int>(s.bars.size()); ++bi) {
            if (std::isnan(s.oracle_up[bi])) {
                s.oracle_up[bi] = up_.predict(rows[bi]);
                s.oracle_down[bi] = down_.predict(rows[bi]);
            }
        }
        if (!s.bars.empty() && s.bars.back().date == last_day && static_cast<int>(s.bars.size()) > kFirstBar) {
            Prediction p;
            p.valid = true;
            p.up = s.oracle_up.back();
            p.down = s.oracle_down.back();
            p.base_up = report_.base_up;
            p.base_down = report_.base_down;
            p.move = move_for(rows.back()[5]);
            p.date = last_day;
            picks.emplace_back(s.symbol, p);
        }
    }
    context_ = ctx;
    picks_ = std::move(picks);
}

Prediction Pythia::predict(const std::string& symbol, const std::vector<Bar>& bars) {
    Prediction p;
    if (!ready() || static_cast<int>(bars.size()) <= kFirstBar) return p;
    const arena::Universe single = arena::build_universe({{symbol, bars}});
    if (single.symbols.empty()) return p;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto rows = features(single.symbols[0], context_);
    const auto& x = rows.back();
    if (std::isnan(x[5])) return p;
    p.valid = true;
    p.up = up_.predict(x);
    p.down = down_.predict(x);
    p.base_up = report_.base_up;
    p.base_down = report_.base_down;
    p.move = move_for(x[5]);
    p.date = bars.back().date;
    return p;
}

PythiaReport Pythia::report() {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

std::string Pythia::status_text() {
    if (!ready()) {
        return "**Pythia** (the built-in AI model) has not been trained yet. It trains once the arena has downloaded "
               "price history, usually within a few minutes of the bot starting.";
    }
    const PythiaReport r = report();
    std::vector<std::pair<std::string, Prediction>> picks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        picks = picks_;
    }
    std::ostringstream m;
    m << "**Pythia**, the built-in AI model (gradient-boosted trees, runs on this computer's processor)\n"
      << "Trained " << r.trained_at.substr(0, 10) << " on " << r.samples << " stock-days (" << r.data_from << " to "
      << r.data_to << ") in " << format_double(r.seconds, 0) << " s.\n"
      << "**Test on years it never saw** (" << r.folds << " years from " << r.tested_from.substr(0, 4) << ", "
      << r.oos_samples << " predictions):\n"
      << "- Rise calls: its most confident 10% were right **" << format_percent(r.top_up, 0) << "** of the time, vs "
      << format_percent(r.base_up, 0) << " on an average day (ranking skill " << format_double(r.auc_up, 2) << ", 0.50 = none)\n"
      << "- Fall calls: " << format_percent(r.top_down, 0) << " vs " << format_percent(r.base_down, 0) << " (ranking skill "
      << format_double(r.auc_down, 2) << ")\n";
    if (!picks.empty()) {
        std::vector<std::pair<std::string, Prediction>> rise = picks, fall = picks;
        std::sort(rise.begin(), rise.end(), [](auto& a, auto& b) { return a.second.up > b.second.up; });
        std::sort(fall.begin(), fall.end(), [](auto& a, auto& b) { return a.second.down > b.second.down; });
        m << "\n**Strongest rise chances** (" << picks.front().second.date << "):";
        for (size_t i = 0; i < 5 && i < rise.size(); ++i) {
            m << " " << rise[i].first << " " << format_percent(rise[i].second.up, 0);
        }
        m << "\n**Strongest fall chances**:";
        for (size_t i = 0; i < 5 && i < fall.size(); ++i) {
            m << " " << fall[i].first << " " << format_percent(fall[i].second.down, 0);
        }
        m << "\n";
    }
    m << "*A rise means +2.5x the stock's daily volatility before a fall of half that, within 20 trading days. "
         "Probabilities, not promises.*";
    return m.str();
}

}  // namespace strojanka::ml
