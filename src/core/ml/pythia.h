#pragma once

#include <array>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/arena/engine.h"
#include "core/ml/gbdt.h"
#include "core/repository.h"

// Pythia, the built-in AI model. For every stock and day it estimates two chances, using only
// information available that day:
//   up   - the price rises by `move` before it falls by move/2, within 20 trading days
//   down - the price falls by `move` before it rises by move/2, within 20 trading days
// where move = 2.5 x the stock's recent daily volatility (1.5% to 15%), the same rule the
// PEAK/SUPPORT alerts are graded by. It learns from all stocks of the arena's list at once.
// Everything runs on the CPU; training takes seconds to a minute once a week.
namespace strojanka::ml {

constexpr int kHorizonDays = 20;

struct Prediction {
    bool valid = false;
    double up = 0, down = 0;          // probabilities
    double base_up = 0, base_down = 0;  // how often that happens on an average day
    double move = 0;
    std::string date;
};

// How Pythia did on years it never saw during training.
struct PythiaReport {
    std::string trained_at, data_from, data_to, tested_from;
    int samples = 0, oos_samples = 0, folds = 0;
    double base_up = 0, base_down = 0;  // how often the event happened
    double auc_up = 0.5, auc_down = 0.5;  // ranking skill: 0.5 = none, 1 = perfect
    double top_up = 0, top_down = 0;      // hit rate of its most confident 10% of calls
    double seconds = 0;
};

class Pythia {
public:
    explicit Pythia(Repository& repo) : repo_(repo) {}

    bool load();  // a model saved by an earlier run
    bool ready();

    // Yearly walk-forward test (each year predicted by models trained only on earlier years, with a
    // gap so no 20-day outcome leaks in), then final models on all data. Writes the out-of-sample
    // predictions into `u` (oracle_up/down) for the arena's historical test, and saves the models.
    void train(arena::Universe& u);

    // Fills recent days that have no prediction yet with the final models (live trading days),
    // and remembers the market context and today's strongest candidates.
    void annotate(arena::Universe& u);

    // Today's prediction for any stock with enough history (uses the last known market context).
    Prediction predict(const std::string& symbol, const std::vector<Bar>& bars);

    PythiaReport report();
    std::string status_text();  // for the `pythia` command

    static const std::vector<std::string>& feature_names();

    using Context = std::map<std::string, std::array<float, 3>>;  // date -> market return 20d, 60d, breadth
    static Context market_context(const arena::Universe& u);
    // One row per bar of `s`; row i uses only bars 0..i.
    static std::vector<std::vector<float>> features(const arena::SymbolData& s, const Context& context);

private:

    std::mutex mutex_;
    Repository& repo_;
    Gbdt up_, down_;
    PythiaReport report_;
    Context context_;
    std::vector<std::pair<std::string, Prediction>> picks_;  // latest day, all stocks
};

}  // namespace strojanka::ml
