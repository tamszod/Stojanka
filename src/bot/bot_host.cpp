#include "bot/bot_host.h"

#include <curl/curl.h>

#include <thread>

#include "bot/discord_bot.h"
#include "bot/tls_trust.h"
#include "common/crash_handler.h"
#include "common/platform.h"
#include "common/strings.h"
#include "common/utf8_path.h"
#include "core/log.h"
#include "core/migrations.h"
#include "core/run_state.h"
#include "core/version.h"

namespace strojanka {

namespace {

size_t discard_body(char*, size_t size, size_t nmemb, void*) { return size * nmemb; }

// A run that never reached stop() leaves no trace of why in the log; say what's known.
void report_previous_run(Repository& repo) {
    const int64_t boot = system_boot_unix();
    if (boot > 0) log_info("this computer has been running since " + unix_to_iso(boot));
    const PreviousRun prev = describe_previous_run(repo.meta(kRunStateKey), repo.meta(kRunStartedKey), boot);
    if (prev.text.empty()) return;
    if (prev.warn) log_warn(prev.text);
    else log_info(prev.text);
}

}  // namespace

long check_discord_token(const std::string& token) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    const std::string auth = "Authorization: Bot " + token;
    curl_slist* headers = curl_slist_append(nullptr, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, "https://discord.com/api/v10/users/@me");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DiscordBot (https://github.com, " STROJANKA_VERSION ")");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    long status = 0;
    if (curl_easy_perform(curl) == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

BotHost::BotHost(std::string config_path) : config_path_(std::move(config_path)) {}

BotHost::~BotHost() { stop(); }

bool BotHost::start(std::string* error) {
    if (running()) return true;
    abort_wait_ = false;
    auto fail = [&](const std::string& message) {
        log_error(message);
        if (error) *error = message;
        stop();
        return false;
    };
    std::string load_error;
    auto loaded = load_config(config_path_, &load_error);
    if (!loaded) return fail("Could not load the settings file: " + load_error);
    config_ = *loaded;
    log_set_level(parse_log_level(config_.log_level));
    if (!config_.log_file.empty()) {
        const std::string log_path = resolve_config_path(config_path_, config_.log_file);
        log_set_file(log_path);
        set_crash_log_mirror(log_path);
    }

    if (config_.discord_token.empty()) return fail("discord_token is empty in " + config_path_ + ". Run the installer again or edit the file.");
    long token_status = check_discord_token(config_.discord_token);
    if (token_status == 0 && network_wait_.count() > 0) {
        log_info("waiting for the network to reach Discord (up to " + std::to_string(network_wait_.count()) + " s)");
        const auto deadline = std::chrono::steady_clock::now() + network_wait_;
        auto next_try = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (token_status == 0 && std::chrono::steady_clock::now() < deadline) {
            if (abort_wait_) return fail("Stopped while waiting for the network.");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (std::chrono::steady_clock::now() < next_try) continue;
            token_status = check_discord_token(config_.discord_token);
            next_try = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        if (token_status != 0) log_info("the network is up");
    }
    if (token_status == 401) {
        return fail("Discord rejected the bot token in " + config_path_ +
                    ". Copy a fresh token from the Discord developer portal (Bot -> Reset Token) into the file.");
    }
    if (token_status == 0) log_warn("could not reach Discord to check the token; trying to connect anyway");

    const std::string db_path = resolve_config_path(config_path_, config_.database_path);
    lock_ = std::make_unique<InstanceLock>(db_path + ".lock");
    if (!lock_->acquired()) {
        return fail("Strojanka is already running with this database (" + db_path +
                    "). Close the other copy first: the server window, or a start_strojanka console.");
    }
    try {
        db_ = std::make_unique<Database>(db_path);
        migrate_database(*db_, true);
    } catch (const std::exception& e) {
        return fail(std::string("Database problem: ") + e.what());
    }
    repo_ = std::make_unique<Repository>(*db_);
    prices_ = std::make_unique<YahooProvider>();
    news_ = std::make_unique<news::RssNewsProvider>();
    monitor_ = std::make_unique<MonitorService>(*repo_, *prices_, config_, news_.get());
    screener_ = std::make_unique<Screener>(*repo_, *prices_, config_, news_.get());
    commands_ = std::make_unique<CommandService>(*repo_, *monitor_, screener_.get());
    admin_ = std::make_unique<AdminService>(*db_, *repo_, *monitor_, config_, config_path_);
    commands_->set_admin(admin_.get());
    pythia_ = std::make_unique<ml::Pythia>(*repo_);
    if (pythia_->load()) log_info("pythia: loaded the model trained " + pythia_->report().trained_at.substr(0, 10));
    monitor_->set_pythia(pythia_.get());
    commands_->set_pythia(pythia_.get());
    arena_ = std::make_unique<arena::ArenaService>(*repo_, *prices_, config_);
    arena_->set_pythia(pythia_.get());
    commands_->set_arena(arena_.get());
    senate_ = std::make_unique<politicians::SenateEfdClient>();
    politicians_ = std::make_unique<politicians::PoliticianService>(*repo_, *senate_, config_);
    console_ = std::make_unique<ServerConsole>(*repo_, *monitor_, *screener_, *admin_, *commands_);
    console_->set_arena(arena_.get());
    console_->set_politicians(politicians_.get());

    log_info("Strojanka " + std::string(STROJANKA_VERSION) + " starting, database " + db_path);
    report_previous_run(*repo_);
    started_at_ = utc_now_iso();
    repo_->set_meta(kRunStateKey, "running");
    repo_->set_meta(kRunStartedKey, started_at_);
    log_info(configure_tls_trust(path_utf8(utf8_path(db_path).parent_path() / "trusted_roots.pem")));
    bot_ = std::make_unique<DiscordBot>(config_, *repo_, *monitor_, *commands_, *screener_, *admin_, *arena_, *politicians_);
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        stop_requested_ = false;
    }
    try {
        bot_->start();
    } catch (const std::exception& e) {
        return fail(std::string("Could not connect to Discord (") + e.what() + "). Check the internet connection, then press Try again.");
    }
    return true;
}

void BotHost::stop() {
    const bool was_running = bot_ != nullptr;
    if (was_running) log_info("stopping Strojanka");
    bot_.reset();  // disconnects and stops the monitor and screener
    if (was_running && repo_) {
        try {
            repo_->set_meta(kRunStateKey, "stopped");
        } catch (const std::exception& e) {
            log_warn(std::string("could not record the clean stop: ") + e.what());
        }
    }
    console_.reset();
    politicians_.reset();
    senate_.reset();
    arena_.reset();
    pythia_.reset();
    admin_.reset();
    commands_.reset();
    screener_.reset();
    monitor_.reset();
    news_.reset();
    prices_.reset();
    repo_.reset();
    db_.reset();
    lock_.reset();
    started_at_.clear();
    if (was_running) log_info("Strojanka stopped");
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        stop_requested_ = true;
    }
    wait_cv_.notify_all();
}

void BotHost::wait() {
    std::unique_lock<std::mutex> lock(wait_mutex_);
    wait_cv_.wait(lock, [this] { return stop_requested_; });
}

void BotHost::request_stop() {
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        stop_requested_ = true;
    }
    wait_cv_.notify_all();
}

void BotHost::note_session_end(const std::string& reason) {
    if (!repo_) return;
    try {
        repo_->set_meta(kRunStateKey, "ended: " + reason);
    } catch (const std::exception& e) {
        log_warn(std::string("could not record why the program is ending: ") + e.what());
    }
}

bool BotHost::online() const { return bot_ && bot_->online(); }

std::string BotHost::bot_name() const { return bot_ ? bot_->name() : std::string(); }

}  // namespace strojanka
