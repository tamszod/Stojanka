#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A small gradient-boosted decision tree classifier (the method behind XGBoost/LightGBM),
// written for laptop CPUs: features are bucketed into at most 32 bins, trees are shallow, and
// each tree sees a random half of the rows. No GPU or external library is needed.
namespace strojanka::ml {

struct GbdtOptions {
    int trees = 120;
    int depth = 3;
    double learning_rate = 0.06;
    int min_leaf = 150;           // rows a leaf needs; large values resist overfitting
    double l2 = 10.0;             // leaf value shrinkage
    double row_fraction = 0.5;    // rows sampled per tree
    double feature_fraction = 0.8;
    int bins = 32;
    unsigned seed = 7;
};

class Gbdt {
public:
    // X: rows of equal length (row-major). y: 0 or 1. Missing values may be NaN.
    void train(const std::vector<std::vector<float>>& X, const std::vector<uint8_t>& y, const GbdtOptions& options = {});
    double predict(const std::vector<float>& x) const;  // probability of y = 1
    bool trained() const { return !trees_.empty(); }

    std::string to_json() const;
    bool from_json(const std::string& json);

private:
    struct Node {
        int feature = -1;      // -1 = leaf
        float threshold = 0;   // go left when x <= threshold (NaN goes left)
        int left = -1, right = -1;
        double value = 0;      // leaf output (already multiplied by the learning rate)
    };
    std::vector<std::vector<Node>> trees_;
    double base_ = 0;  // log-odds of the training base rate
};

// Area under the ROC curve: 0.5 = no skill, 1.0 = perfect ranking.
double auc(const std::vector<double>& scores, const std::vector<uint8_t>& labels);

}  // namespace strojanka::ml
