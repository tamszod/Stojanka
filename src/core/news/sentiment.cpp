#include "core/news/sentiment.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "common/strings.h"

namespace strojanka::news {

namespace {

// Phrases are matched before single words, longest first, so "beats estimates" is not also
// counted as "beats". Weights: ±1 ordinary, ±2 strong.
const std::map<std::string, double>& phrases() {
    static const std::map<std::string, double> p = {
        {"beats estimates", 2},     {"beat estimates", 2},      {"tops estimates", 2},     {"beats expectations", 2},
        {"record profit", 2},       {"record revenue", 2},      {"raises guidance", 2},    {"raised guidance", 2},
        {"raises outlook", 2},      {"share repurchase", 2},    {"raises dividend", 2},    {"dividend increase", 2},
        {"all time high", 2},       {"wins contract", 2},       {"strong buy", 2},         {"price target raised", 2},
        {"misses estimates", -2},   {"missed estimates", -2},   {"misses expectations", -2}, {"cuts guidance", -2},
        {"lowers guidance", -2},    {"lowered guidance", -2},   {"cuts outlook", -2},      {"profit warning", -2},
        {"dividend cut", -2},       {"cuts dividend", -2},      {"sell rating", -2},       {"price target cut", -2},
        {"price target lowered", -2}, {"job cuts", -2},
        {"raises target", 2},       {"raises price target", 2}, {"lifts price target", 2}, {"boosts price target", 2},
        {"cuts target", -2},        {"cuts price target", -2},  {"lowers price target", -2}, {"lowers target", -2},
        {"sell off", -1},
    };
    return p;
}

const std::map<std::string, double>& words() {
    static const std::map<std::string, double> w = {
        // strong positive
        {"upgrade", 2}, {"upgraded", 2}, {"upgrades", 2}, {"buyback", 2}, {"surges", 2}, {"soars", 2},
        {"jumps", 2}, {"skyrockets", 2}, {"outperform", 2}, {"approval", 2}, {"approved", 2},
        // positive
        {"beat", 1}, {"beats", 1}, {"tops", 1}, {"gain", 1}, {"gains", 1}, {"rise", 1}, {"rises", 1},
        {"climbs", 1}, {"higher", 1}, {"growth", 1}, {"grows", 1}, {"profit", 1}, {"profitable", 1},
        {"strong", 1}, {"stronger", 1}, {"bullish", 1}, {"optimistic", 1}, {"rebound", 1}, {"rebounds", 1},
        {"recovers", 1}, {"recovery", 1}, {"expands", 1}, {"expansion", 1}, {"partnership", 1}, {"rally", 1},
        {"rallies", 1}, {"surge", 1}, {"upbeat", 1}, {"outperforms", 1}, {"boost", 1}, {"boosts", 1},
        {"record", 1}, {"exceeds", 1}, {"accelerates", 1}, {"advances", 1}, {"wins", 1}, {"raises", 1},
        // strong negative
        {"downgrade", -2}, {"downgraded", -2}, {"downgrades", -2}, {"plunges", -2}, {"plummets", -2},
        {"tumbles", -2}, {"crashes", -2}, {"sinks", -2}, {"layoffs", -2}, {"lawsuit", -2}, {"sued", -2},
        {"probe", -2}, {"investigation", -2}, {"recall", -2}, {"underperform", -2}, {"strike", -2},
        // negative
        {"miss", -1}, {"misses", -1}, {"missed", -1}, {"falls", -1}, {"fall", -1}, {"drops", -1}, {"drop", -1},
        {"declines", -1}, {"decline", -1}, {"lower", -1}, {"loss", -1}, {"losses", -1}, {"weak", -1},
        {"weaker", -1}, {"weakness", -1}, {"bearish", -1}, {"concern", -1}, {"concerns", -1}, {"slump", -1},
        {"slumps", -1}, {"slowdown", -1}, {"slows", -1}, {"cut", -1}, {"cuts", -1}, {"pressure", -1},
        {"headwinds", -1}, {"disappointing", -1}, {"disappoints", -1}, {"fears", -1}, {"worries", -1},
        {"delay", -1}, {"delays", -1}, {"halts", -1}, {"shortfall", -1}, {"warns", -1}, {"slides", -1},
        // -ing forms used in "why is X falling today" style headlines
        {"rising", 1}, {"gaining", 1}, {"climbing", 1}, {"rallying", 1}, {"surging", 2}, {"soaring", 2},
        {"jumping", 2}, {"undervalued", 1},
        {"falling", -1}, {"dropping", -1}, {"declining", -1}, {"sliding", -1}, {"slipping", -1}, {"sinking", -2},
        {"plunging", -2}, {"tumbling", -2}, {"crashing", -2}, {"selloff", -1}, {"overvalued", -1}, {"warn", -1},
        {"warning", -1},
    };
    return w;
}

// Red flags: strongly negative and they block buy (SUPPORT) alerts.
const std::vector<std::string>& red_flag_terms() {
    static const std::vector<std::string> r = {
        "bankruptcy", "bankrupt", "chapter 11", "fraud", "sec investigation", "sec probe", "doj probe",
        "delisting", "delisted", "going concern", "accounting irregularities", "restatement", "insolvency",
        "insolvent", "ponzi", "short seller report", "debt default",
    };
    return r;
}

const std::vector<std::string>& negators() {
    static const std::vector<std::string> n = {"not", "no", "never", "without", "fails", "failed", "fail", "isn't", "won't", "cannot"};
    return n;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '\'') {
            cur.push_back(static_cast<char>(std::tolower(c)));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool negated(const std::vector<std::string>& tokens, size_t at) {
    for (size_t k = at >= 3 ? at - 3 : 0; k < at; ++k) {
        if (std::find(negators().begin(), negators().end(), tokens[k]) != negators().end()) return true;
    }
    return false;
}

}  // namespace

HeadlineScore score_headline(const std::string& title) {
    HeadlineScore s;
    const auto tokens = tokenize(title);
    std::string joined = " ";
    for (const auto& t : tokens) joined += t + " ";
    for (const auto& term : red_flag_terms()) {
        if (joined.find(" " + term + " ") != std::string::npos) {
            s.red_flag = true;
            s.red_flag_term = term;
            break;
        }
    }

    double total = 0;
    for (size_t i = 0; i < tokens.size();) {
        bool matched = false;
        for (size_t len = 3; len >= 2 && !matched; --len) {
            if (i + len > tokens.size()) continue;
            std::string phrase = tokens[i];
            for (size_t k = 1; k < len; ++k) phrase += " " + tokens[i + k];
            const auto it = phrases().find(phrase);
            if (it != phrases().end()) {
                total += negated(tokens, i) ? -it->second : it->second;
                i += len;
                matched = true;
            }
        }
        if (matched) continue;
        const auto it = words().find(tokens[i]);
        if (it != words().end()) total += negated(tokens, i) ? -it->second : it->second;
        ++i;
    }
    // Analyst target changes usually name the company in between ("cuts Boeing price target"); the
    // verb alone counted ±1 above, this makes it a strong ±2. Adjacent forms are phrases already.
    static const std::vector<std::string> target_down = {"cuts", "cut", "lowers", "lowered", "trims", "slashes"};
    static const std::vector<std::string> target_up = {"raises", "raised", "lifts", "boosts", "hikes"};
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] != "target") continue;
        for (size_t k = i >= 4 ? i - 4 : 0; k + 1 < i; ++k) {
            if (k + 1 == i || (k + 2 == i && tokens[i - 1] == "price")) continue;  // adjacent: handled as a phrase
            if (std::find(target_down.begin(), target_down.end(), tokens[k]) != target_down.end()) total -= 1;
            if (std::find(target_up.begin(), target_up.end(), tokens[k]) != target_up.end()) total += 1;
        }
    }
    if (s.red_flag) total -= 3;
    double score = std::clamp(total / 3.0, -1.0, 1.0);
    if (trim(title).size() > 0 && trim(title).back() == '?') score *= 0.5;  // "Is X a buy now?" says little
    s.sentiment = score;
    return s;
}

std::string title_key(const std::string& title) {
    std::string key;
    for (char ch : title) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) key.push_back(static_cast<char>(std::tolower(c)));
        if (key.size() >= 60) break;
    }
    return key;
}

NewsSummary summarize(const std::vector<NewsItem>& items, int64_t now, int64_t window_seconds) {
    NewsSummary s;
    double weighted = 0, weights = 0, best = 0, worst = 0;
    for (const auto& item : items) {
        const int64_t age = now - iso_to_unix(item.published_at);
        if (age < -3600 || age > window_seconds) continue;
        const double w = std::pow(0.5, std::max<int64_t>(age, 0) / 86400.0);
        weighted += w * item.sentiment;
        weights += w;
        ++s.headlines;
        if (item.red_flag) s.red_flags.push_back(item.title);
        if (item.sentiment > best) {
            best = item.sentiment;
            s.most_positive = item.title;
        }
        if (item.sentiment < worst) {
            worst = item.sentiment;
            s.most_negative = item.title;
        }
    }
    if (weights > 0) {
        // A single headline should not swing the call; shrink toward neutral when news is thin.
        s.sentiment = (weighted / weights) * (s.headlines / (s.headlines + 2.0));
    }
    return s;
}

double adjust_news_weight(double weight, const std::vector<SignalRecord>& graded) {
    int agree = 0, agree_ok = 0, disagree = 0, disagree_ok = 0;
    for (const auto& s : graded) {
        if (!s.news_sentiment || s.outcome == SignalOutcome::Pending) continue;
        const double tone = *s.news_sentiment;
        if (std::fabs(tone) < 0.1) continue;
        const bool agrees = (s.side == SignalSide::Peak) == (tone < 0);
        const bool ok = s.outcome == SignalOutcome::Correct;
        if (agrees) {
            ++agree;
            agree_ok += ok;
        } else {
            ++disagree;
            disagree_ok += ok;
        }
    }
    if (agree < 10 || disagree < 10) return weight;
    const double diff = static_cast<double>(agree_ok) / agree - static_cast<double>(disagree_ok) / disagree;
    if (diff > 0.10) return std::min(kMaxNewsWeight, weight + 0.01);
    if (diff < 0.0) return std::max(0.0, weight - 0.01);
    return weight;
}

std::string describe(double sentiment) {
    if (sentiment > 0.25) return "positive";
    if (sentiment > 0.08) return "slightly positive";
    if (sentiment < -0.25) return "negative";
    if (sentiment < -0.08) return "slightly negative";
    return "neutral";
}

}  // namespace strojanka::news
