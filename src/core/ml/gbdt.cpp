#include "core/ml/gbdt.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

#include <nlohmann/json.hpp>

namespace strojanka::ml {

namespace {

double sigmoid(double z) { return 1.0 / (1.0 + std::exp(-z)); }

struct Hist {
    double g = 0, h = 0;
    int n = 0;
};

}  // namespace

void Gbdt::train(const std::vector<std::vector<float>>& X, const std::vector<uint8_t>& y, const GbdtOptions& o) {
    trees_.clear();
    const size_t n = X.size();
    if (n == 0) return;
    const size_t f = X[0].size();

    // Quantile bin edges per feature; NaN gets its own bin 0.
    std::vector<std::vector<float>> edges(f);
    std::vector<std::vector<uint8_t>> bin(f, std::vector<uint8_t>(n, 0));
    for (size_t j = 0; j < f; ++j) {
        std::vector<float> values;
        values.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (!std::isnan(X[i][j])) values.push_back(X[i][j]);
        }
        std::sort(values.begin(), values.end());
        for (int b = 1; b < o.bins && !values.empty(); ++b) {
            const float e = values[values.size() * b / o.bins];
            if (edges[j].empty() || e > edges[j].back()) edges[j].push_back(e);
        }
        for (size_t i = 0; i < n; ++i) {
            const float v = X[i][j];
            if (std::isnan(v)) continue;
            bin[j][i] = static_cast<uint8_t>(1 + (std::lower_bound(edges[j].begin(), edges[j].end(), v) - edges[j].begin()));
        }
    }

    const double rate = std::clamp(std::accumulate(y.begin(), y.end(), 0.0) / n, 1e-4, 1 - 1e-4);
    base_ = std::log(rate / (1 - rate));
    std::vector<double> score(n, base_), g(n), h(n);
    std::mt19937 rng(o.seed);
    std::vector<size_t> all(n);
    std::iota(all.begin(), all.end(), 0);
    const int nbins = o.bins + 1;

    for (int t = 0; t < o.trees; ++t) {
        for (size_t i = 0; i < n; ++i) {
            const double p = sigmoid(score[i]);
            g[i] = p - y[i];
            h[i] = std::max(p * (1 - p), 1e-6);
        }
        std::shuffle(all.begin(), all.end(), rng);
        const size_t sampled = std::max<size_t>(1, static_cast<size_t>(static_cast<double>(n) * o.row_fraction));
        std::vector<size_t> rows(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(sampled));
        std::vector<size_t> feats(f);
        std::iota(feats.begin(), feats.end(), 0);
        std::shuffle(feats.begin(), feats.end(), rng);
        feats.resize(std::max<size_t>(1, static_cast<size_t>(f * o.feature_fraction)));

        std::vector<Node> tree;
        struct Work {
            int node;
            std::vector<size_t> rows;
            int depth;
        };
        tree.push_back({});
        std::vector<Work> stack;
        stack.push_back({0, std::move(rows), 0});
        while (!stack.empty()) {
            Work w = std::move(stack.back());
            stack.pop_back();
            double G = 0, H = 0;
            for (size_t i : w.rows) {
                G += g[i];
                H += h[i];
            }
            tree[w.node].value = -G / (H + o.l2) * o.learning_rate;
            if (w.depth >= o.depth || static_cast<int>(w.rows.size()) < 2 * o.min_leaf) continue;

            double best_gain = 1e-9;
            int best_f = -1, best_b = -1;
            const double parent = G * G / (H + o.l2);
            std::vector<Hist> hist(nbins);
            for (size_t j : feats) {
                std::fill(hist.begin(), hist.end(), Hist{});
                for (size_t i : w.rows) {
                    Hist& c = hist[bin[j][i]];
                    c.g += g[i];
                    c.h += h[i];
                    ++c.n;
                }
                double gl = 0, hl = 0;
                int nl = 0;
                for (int b = 0; b < nbins - 1; ++b) {
                    gl += hist[b].g;
                    hl += hist[b].h;
                    nl += hist[b].n;
                    const int nr = static_cast<int>(w.rows.size()) - nl;
                    if (nl < o.min_leaf || nr < o.min_leaf) continue;
                    const double gr = G - gl, hr = H - hl;
                    const double gain = gl * gl / (hl + o.l2) + gr * gr / (hr + o.l2) - parent;
                    if (gain > best_gain) {
                        best_gain = gain;
                        best_f = static_cast<int>(j);
                        best_b = b;
                    }
                }
            }
            if (best_f < 0) continue;
            std::vector<size_t> left, right;
            for (size_t i : w.rows) (bin[best_f][i] <= best_b ? left : right).push_back(i);
            // Capture the child indices as plain ints before touching the vector again: the two
            // push_back calls below may reallocate tree's storage, which would turn a reference
            // held across them into a dangling one (undefined behavior -- a crash observed in
            // the field, see GitHub issue #2, because it depends on whether something else has
            // since reused the freed memory, which is far more likely with many threads
            // allocating concurrently than in a quiet single-threaded test).
            const int left_idx = static_cast<int>(tree.size());
            const int right_idx = left_idx + 1;
            {
                Node& node = tree[w.node];
                node.feature = best_f;
                // bin b covers values <= edges[b-1]; bin 0 is NaN (always left)
                node.threshold = best_b == 0 ? -std::numeric_limits<float>::infinity() : edges[best_f][best_b - 1];
                node.left = left_idx;
                node.right = right_idx;
            }
            tree.push_back({});
            tree.push_back({});
            stack.push_back({left_idx, std::move(left), w.depth + 1});
            stack.push_back({right_idx, std::move(right), w.depth + 1});
        }
        // Update all scores with the new tree (rows outside the sample too).
        for (size_t i = 0; i < n; ++i) {
            int k = 0;
            while (tree[k].feature >= 0) {
                const float v = X[i][tree[k].feature];
                k = (std::isnan(v) || v <= tree[k].threshold) ? tree[k].left : tree[k].right;
            }
            score[i] += tree[k].value;
        }
        trees_.push_back(std::move(tree));
    }
}

double Gbdt::predict(const std::vector<float>& x) const {
    double s = base_;
    for (const auto& tree : trees_) {
        int k = 0;
        while (tree[k].feature >= 0) {
            const float v = x[tree[k].feature];
            k = (std::isnan(v) || v <= tree[k].threshold) ? tree[k].left : tree[k].right;
        }
        s += tree[k].value;
    }
    return sigmoid(s);
}

std::string Gbdt::to_json() const {
    nlohmann::json j;
    j["base"] = base_;
    j["trees"] = nlohmann::json::array();
    for (const auto& tree : trees_) {
        nlohmann::json t = nlohmann::json::array();
        for (const auto& n : tree) t.push_back({n.feature, n.threshold, n.left, n.right, n.value});
        j["trees"].push_back(t);
    }
    return j.dump();
}

bool Gbdt::from_json(const std::string& text) {
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("trees")) return false;
    trees_.clear();
    base_ = j.value("base", 0.0);
    for (const auto& t : j["trees"]) {
        std::vector<Node> tree;
        for (const auto& n : t) {
            Node node;
            node.feature = n[0].get<int>();
            node.threshold = n[1].is_number() ? n[1].get<float>() : -std::numeric_limits<float>::infinity();
            node.left = n[2].get<int>();
            node.right = n[3].get<int>();
            node.value = n[4].get<double>();
            tree.push_back(node);
        }
        trees_.push_back(std::move(tree));
    }
    return true;
}

double auc(const std::vector<double>& scores, const std::vector<uint8_t>& labels) {
    std::vector<size_t> idx(scores.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return scores[a] < scores[b]; });
    double rank_sum = 0, pos = 0;
    for (size_t r = 0; r < idx.size(); ++r) {
        if (labels[idx[r]]) {
            rank_sum += static_cast<double>(r + 1);
            ++pos;
        }
    }
    const double neg = static_cast<double>(scores.size()) - pos;
    if (pos == 0 || neg == 0) return 0.5;
    return (rank_sum - pos * (pos + 1) / 2) / (pos * neg);
}

}  // namespace strojanka::ml
