#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/models.h"

namespace strojanka::news {

struct HeadlineScore {
    double sentiment = 0;  // -1 (very negative) .. +1 (very positive)
    bool red_flag = false; // bankruptcy, fraud, regulator probe, delisting...
    std::string red_flag_term;
};

// Scores one headline with a finance word list: phrases ("beats estimates", "cuts guidance"),
// single words, negation ("fails to beat"), red-flag terms, and clickbait questions counted at half.
HeadlineScore score_headline(const std::string& title);

// Key used to recognise the same story from different feeds (lowercase letters/digits, 60 chars).
std::string title_key(const std::string& title);

struct NewsSummary {
    int headlines = 0;           // in the window
    double sentiment = 0;        // recency-weighted, shrunk toward 0 when there are few headlines
    std::vector<std::string> red_flags;  // titles of red-flag headlines in the window
    std::string most_positive;
    std::string most_negative;
};

// Summarises the headlines published in the last `window_seconds` before `now`
// (each headline counts half as much per day of age).
NewsSummary summarize(const std::vector<NewsItem>& items, int64_t now, int64_t window_seconds = 72 * 3600);

// "positive", "slightly negative", "neutral"...
std::string describe(double sentiment);

// How much news moves a score: score ± weight × sentiment. Starts small because the effect of
// news cannot be back-tested (free feeds only reach a few weeks back); it is learned live.
constexpr double kDefaultNewsWeight = 0.05;
constexpr double kMaxNewsWeight = 0.15;

// Learning step. Splits graded alerts into "news agreed with the call" (bad news before a PEAK
// call, good news before a SUPPORT call) and "news disagreed". With at least 10 of each: if the
// agreeing ones were right clearly more often, news gets more weight; if not, less (down to 0).
double adjust_news_weight(double weight, const std::vector<SignalRecord>& graded);

}  // namespace strojanka::news
