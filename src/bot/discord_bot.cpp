#include "bot/discord_bot.h"

#include "core/log.h"

namespace strojanka {

namespace {

constexpr size_t kDiscordMessageLimit = 2000;

std::string fit_message(const std::string& text) {
    if (text.size() <= kDiscordMessageLimit) return text;
    std::string cut = text.substr(0, kDiscordMessageLimit - 4);
    while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();  // keep UTF-8 valid
    if (!cut.empty() && static_cast<unsigned char>(cut.back()) >= 0xC0) cut.pop_back();
    return cut + "\n...";
}

dpp::snowflake to_snowflake(const std::string& id) {
    try {
        return dpp::snowflake(static_cast<uint64_t>(std::stoull(id)));
    } catch (...) {
        return dpp::snowflake(static_cast<uint64_t>(0));
    }
}

dpp::command_option stock_option(bool required) {
    return dpp::command_option(dpp::co_string, "stock", "Stock or ETF ticker, for example FCX, SPY or CSPX.L", required);
}

dpp::command_option politician_option() {
    return dpp::command_option(dpp::co_string, "politician", "A politician's name instead of a stock, for example Pelosi", false);
}

}  // namespace

DiscordBot::DiscordBot(const Config& config, Repository& repo, MonitorService& monitor, CommandService& commands,
                       Screener& screener, AdminService& admin, arena::ArenaService& arena,
                       politicians::PoliticianService& politicians)
    : config_(config),
      repo_(repo),
      monitor_(monitor),
      commands_(commands),
      screener_(screener),
      admin_(admin),
      arena_(arena),
      politicians_(politicians),
      cluster_(config.discord_token, dpp::i_default_intents),
      workers_(4) {
    cluster_.on_log([](const dpp::log_t& event) {
        switch (event.severity) {
            case dpp::ll_critical:
            case dpp::ll_error: log_error("discord: " + event.message); break;
            case dpp::ll_warning: log_warn("discord: " + event.message); break;
            case dpp::ll_info: log_info("discord: " + event.message); break;
            default: log_debug("discord: " + event.message); break;
        }
    });
    cluster_.on_ready([this](const dpp::ready_t&) {
        online_ = true;
        {
            std::lock_guard<std::mutex> lock(name_mutex_);
            name_ = cluster_.me.format_username();
        }
        if (!started_) {
            started_ = true;
            log_info("connected to Discord as " + cluster_.me.format_username());
            register_commands();
            monitor_.set_notifier(this);
            monitor_.start();
            screener_.start();
            arena_.set_announcer([this](const std::string& message) { post_to_public_channel(message); });
            arena_.start();
            politicians_.set_announcer([this](const std::string& message) { post_to_public_channel(message); });
            politicians_.set_notifier(this);
            politicians_.start();
            load_owners();
            cluster_.start_timer([this](dpp::timer) { workers_.submit([this] { admin_.check_health(*this); }); }, 3600);
        }
    });
    cluster_.on_slashcommand([this](const dpp::slashcommand_t& event) { on_command(event); });
    cluster_.on_message_create([this](const dpp::message_create_t& event) { on_message(event); });
}

DiscordBot::~DiscordBot() {
    politicians_.stop();
    politicians_.set_announcer({});
    politicians_.set_notifier(nullptr);
    arena_.stop();
    arena_.set_announcer({});
    screener_.stop();
    monitor_.stop();
    monitor_.set_notifier(nullptr);
}

void DiscordBot::run() { cluster_.start(dpp::st_wait); }

void DiscordBot::start() { cluster_.start(dpp::st_return); }

std::string DiscordBot::name() {
    std::lock_guard<std::mutex> lock(name_mutex_);
    return name_;
}

void DiscordBot::load_owners() {
    cluster_.current_application_get([this](const dpp::confirmation_callback_t& cb) {
        std::vector<std::string> owners;
        if (cb.is_error()) {
            log_warn("could not read the bot application's owner: " + cb.get_error().message);
        } else {
            const auto app = cb.get<dpp::application>();
            auto add = [&owners](dpp::snowflake id) {
                const std::string s = std::to_string(static_cast<uint64_t>(id));
                if (id != 0 && std::find(owners.begin(), owners.end(), s) == owners.end()) owners.push_back(s);
            };
            add(app.owner.id);
            for (const auto& member : app.team.members) add(member.member_user.id);
        }
        admin_.set_owners(owners);
        log_info("admins: " + std::to_string(admin_.admins().size()) + " (bot owner included automatically)");
        workers_.submit([this] { admin_.check_health(*this); });
    });
}

void DiscordBot::post_to_public_channel(const std::string& message) {
    const std::string channel = arena_.channel_id();
    if (channel.empty()) {
        log_warn("arena: no public channel set (admin channel <channel ID>), message not posted: " + message.substr(0, 80));
        return;
    }
    cluster_.message_create(dpp::message(to_snowflake(channel), fit_message(message)), [channel](const dpp::confirmation_callback_t& cb) {
        if (cb.is_error()) log_warn("arena: could not post in channel " + channel + ": " + cb.get_error().message);
    });
}

void DiscordBot::send_direct(const std::string& user_id, const std::string& message) {
    cluster_.direct_message_create(
        to_snowflake(user_id), dpp::message(fit_message(message)), [this, user_id, message](const dpp::confirmation_callback_t& cb) {
            if (!cb.is_error()) return;
            // DMs can be disabled by the user; fall back to the channel they last used a command in.
            const std::string dm_error = cb.get_error().message;
            const std::string channel = repo_.user_notify_channel(user_id);
            if (channel.empty()) {
                log_warn("could not DM user " + user_id + ": " + dm_error);
                return;
            }
            cluster_.message_create(dpp::message(to_snowflake(channel), fit_message("<@" + user_id + "> " + message)),
                                    [user_id, dm_error](const dpp::confirmation_callback_t& cb2) {
                                        if (cb2.is_error()) log_warn("could not DM user " + user_id + ": " + dm_error);
                                    });
        });
}

void DiscordBot::register_commands() {
    const auto app = cluster_.me.id;
    std::vector<dpp::slashcommand> cmds;
    cmds.emplace_back("help", "List the commands and how to use them", app);
    cmds.emplace_back("status", "Your watched stocks: price, trend and balance", app);

    dpp::slashcommand profit("profit", "Your profit per stock (selling price - buying price)", app);
    profit.add_option(stock_option(false));
    cmds.push_back(profit);

    cmds.emplace_back("portfolio", "What you hold now: shares, average cost, value, weight and profit", app);

    dpp::slashcommand market("market", "Stock exchanges: ticker ending, opening hours, open or closed now", app);
    market.add_option(dpp::command_option(dpp::co_string, "market", "Only this one, for example London or .DE", false));
    cmds.push_back(market);

    dpp::slashcommand history("history", "Every buy and sell you recorded, with the profit of each sale", app);
    history.add_option(stock_option(false));
    cmds.push_back(history);

    dpp::slashcommand monitor("monitor", "Start watching a stock, or a politician's disclosed trades", app);
    monitor.add_option(stock_option(false));
    monitor.add_option(politician_option());
    cmds.push_back(monitor);

    dpp::slashcommand remove("remove", "Stop watching a stock (only if you hold no shares) or a politician", app);
    remove.add_option(stock_option(false));
    remove.add_option(politician_option());
    cmds.push_back(remove);

    dpp::slashcommand watch("watch", "A message when a stock reaches a price; leave empty to list your price alerts", app);
    watch.add_option(stock_option(false));
    watch.add_option(dpp::command_option(dpp::co_string, "price", "The price to tell you at, e.g. 250 or 249.90$", false));
    cmds.push_back(watch);

    dpp::slashcommand unwatch("unwatch", "Remove your price alerts for a stock", app);
    unwatch.add_option(stock_option(true));
    unwatch.add_option(dpp::command_option(dpp::co_string, "price", "Only the alert at this price", false));
    cmds.push_back(unwatch);

    dpp::slashcommand safety("safety", "Warnings before a stock you hold turns into a loss: levels, on/off, sale fee", app);
    safety.add_option(dpp::command_option(dpp::co_string, "alerts", "Turn safety alerts on or off", false)
                          .add_choice(dpp::command_option_choice("on", std::string("on")))
                          .add_choice(dpp::command_option_choice("off", std::string("off"))));
    safety.add_option(dpp::command_option(dpp::co_string, "fee", "What one sale costs you, e.g. 0.20 (the default)", false));
    safety.add_option(dpp::command_option(dpp::co_string, "stock", "Turn them on or off for this stock only", false));
    cmds.push_back(safety);

    dpp::slashcommand politicians("politicians", "Recent US Congress (House and Senate) stock trades; empty for everyone", app);
    politicians.add_option(dpp::command_option(dpp::co_string, "name", "Only trades matching this name", false));
    cmds.push_back(politicians);

    dpp::slashcommand buy("buy", "Record shares you bought", app);
    buy.add_option(stock_option(true));
    buy.add_option(dpp::command_option(dpp::co_string, "shares", "Number of shares, e.g. 0.858291457", true));
    buy.add_option(dpp::command_option(dpp::co_string, "paid", "Total amount paid, e.g. 59.78$", true));
    cmds.push_back(buy);

    dpp::slashcommand sell("sell", "Record shares you sold", app);
    sell.add_option(stock_option(true));
    sell.add_option(dpp::command_option(dpp::co_string, "shares", "Number of shares, e.g. 0.858291457", true));
    sell.add_option(dpp::command_option(dpp::co_string, "received", "Total amount received, e.g. 67.87$", true));
    cmds.push_back(sell);

    dpp::slashcommand report("report", "Analysis report for a stock", app);
    report.add_option(stock_option(true));
    cmds.push_back(report);

    dpp::slashcommand news("news", "Recent headlines for a stock and how they affect its signals", app);
    news.add_option(stock_option(true));
    cmds.push_back(news);

    dpp::slashcommand suggest("suggest", "Stocks whose PEAK/SUPPORT signals have proven reliable", app);
    suggest.add_option(dpp::command_option(dpp::co_string, "side", "Only stocks good for buying or for selling", false)
                           .add_choice(dpp::command_option_choice("buy (SUPPORT)", std::string("buy")))
                           .add_choice(dpp::command_option_choice("sell (PEAK)", std::string("sell"))));
    cmds.push_back(suggest);

    dpp::slashcommand arena("arena", "The paper-trading bots ($1,000 each) and how they are doing", app);
    dpp::command_option bot_option(dpp::co_string, "bot", "Show one bot in detail", false);
    for (const auto& spec : arena::roster()) bot_option.add_choice(dpp::command_option_choice(spec.name, spec.name));
    arena.add_option(bot_option);
    cmds.push_back(arena);

    cmds.emplace_back("pythia", "The built-in AI model: its tested track record and today's strongest candidates", app);

    dpp::slashcommand admin("admin", "Admin panel: disk space, database location, admins (admins only)", app);
    admin.add_option(dpp::command_option(dpp::co_sub_command, "status", "Show the admin panel"));
    admin.add_option(dpp::command_option(dpp::co_sub_command, "add", "Make someone an admin (they get warnings)")
                         .add_option(dpp::command_option(dpp::co_user, "user", "The person to add", true)));
    admin.add_option(dpp::command_option(dpp::co_sub_command, "remove", "Remove an admin")
                         .add_option(dpp::command_option(dpp::co_user, "user", "The admin to remove", true)));
    admin.add_option(dpp::command_option(dpp::co_sub_command, "movedb", "Move the database to another folder or drive")
                         .add_option(dpp::command_option(dpp::co_string, "folder", "Full folder path, e.g. D:\\StrojankaData", true)));
    cmds.push_back(admin);

    // The bot is used in its direct-message chat only.
    for (auto& c : cmds) c.set_interaction_contexts({dpp::itc_bot_dm});
    cluster_.global_bulk_command_create(cmds, [](const dpp::confirmation_callback_t& cb) {
        if (cb.is_error()) log_error("registering slash commands failed: " + cb.get_error().message);
        else log_info("slash commands registered for direct messages");
    });
    // Older versions registered commands on one server (guild_id); remove those copies.
    if (!config_.guild_id.empty()) {
        cluster_.guild_bulk_command_create({}, to_snowflake(config_.guild_id), [](const dpp::confirmation_callback_t& cb) {
            if (cb.is_error()) log_warn("could not remove old server commands: " + cb.get_error().message);
        });
    }
}

void DiscordBot::on_message(const dpp::message_create_t& event) {
    const dpp::message& msg = event.msg;
    if (msg.author.is_bot() || msg.guild_id != 0 || msg.content.empty()) return;  // direct messages only
    const std::string user = std::to_string(static_cast<uint64_t>(msg.author.id));
    const std::string channel = std::to_string(static_cast<uint64_t>(msg.channel_id));
    const std::string text = msg.content;
    const dpp::snowflake channel_id = msg.channel_id;
    log_info("message command from user " + user + ": " + text.substr(0, 60));
    workers_.submit([this, user, channel, text, channel_id] {
        CommandReply reply;
        try {
            repo_.ensure_user(user, channel);
            reply = commands_.run_text(user, text);
        } catch (const std::exception& e) {
            log_error("text command failed: " + std::string(e.what()));
            reply = {false, "Something went wrong while running this command. The error was logged."};
        }
        cluster_.message_create(dpp::message(channel_id, fit_message(reply.text)));
    });
}

void DiscordBot::on_command(const dpp::slashcommand_t& event) {
    const std::string name = event.command.get_command_name();
    const std::string user = std::to_string(static_cast<uint64_t>(event.command.get_issuing_user().id));
    const std::string channel = std::to_string(static_cast<uint64_t>(event.command.channel_id));
    auto param = [&event](const char* key) -> std::string {
        const auto v = event.get_parameter(key);
        return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string();
    };
    // `side` (suggest) and `bot` (arena) travel in the stock slot.
    const std::string stock = name == "suggest" ? param("side") : name == "arena" ? param("bot") : name == "market" ? param("market") : param("stock");
    // `name` (politicians) and `politician` (monitor/remove) travel in the same slot.
    const std::string politician = name == "politicians" ? param("name") : param("politician");
    // `alerts` (safety) travels in the shares slot; `price` (watch) and `fee` (safety) in the amount slot.
    const std::string shares = name == "safety" ? param("alerts") : param("shares");
    const std::string amount = name == "sell"                         ? param("received")
                               : name == "watch" || name == "unwatch" ? param("price")
                               : name == "safety"                     ? param("fee")
                                                                      : param("paid");
    const std::string token = event.command.token;
    // /admin uses sub-commands: args = {sub-command, its option values...}
    std::vector<std::string> admin_args;
    if (name == "admin") {
        const auto data = event.command.get_command_interaction();
        if (!data.options.empty()) {
            admin_args.push_back(data.options[0].name);
            for (const auto& o : data.options[0].options) {
                if (std::holds_alternative<dpp::snowflake>(o.value)) {
                    admin_args.push_back(std::to_string(static_cast<uint64_t>(std::get<dpp::snowflake>(o.value))));
                } else if (std::holds_alternative<std::string>(o.value)) {
                    admin_args.push_back(std::get<std::string>(o.value));
                }
            }
        }
    }
    log_info("/" + name + " from user " + user + (stock.empty() ? "" : " stock=" + stock));

    // Replies are private (ephemeral): portfolios are personal. Work runs off the gateway
    // thread because some commands download price history.
    event.thinking(true, [this, name, user, channel, stock, politician, shares, amount, token, admin_args](const dpp::confirmation_callback_t& ack) {
        if (ack.is_error()) {
            log_warn("could not acknowledge /" + name + ": " + ack.get_error().message);
            return;
        }
        workers_.submit([this, name, user, channel, stock, politician, shares, amount, token, admin_args] {
            CommandReply reply;
            try {
                repo_.ensure_user(user, channel);
                reply = dispatch(name, user, stock, politician, shares, amount, admin_args);
            } catch (const std::exception& e) {
                log_error("/" + name + " failed: " + e.what());
                reply = {false, "Something went wrong while running this command. The error was logged."};
            }
            cluster_.interaction_response_edit(token, dpp::message(fit_message(reply.text)));
        });
    });
}

CommandReply DiscordBot::dispatch(const std::string& name, const std::string& user, const std::string& stock,
                                  const std::string& politician, const std::string& shares, const std::string& amount,
                                  const std::vector<std::string>& admin_args) {
    if (name == "admin") return commands_.admin(user, admin_args);
    if (name == "help") return commands_.help();
    if (name == "status") return commands_.status(user);
    if (name == "profit") return commands_.profit(user, stock);
    if (name == "portfolio") return commands_.portfolio(user);
    if (name == "market") return commands_.market(user, stock);
    if (name == "history") return commands_.history(user, stock);
    if (name == "monitor" || name == "remove") {
        if (!stock.empty() && !politician.empty()) return {false, "Give either a stock or a politician, not both."};
        if (stock.empty() && politician.empty()) return {false, "Give a stock ticker or a politician's name."};
        if (name == "monitor") return politician.empty() ? commands_.monitor(user, stock) : commands_.monitor_politician(user, politician);
        return politician.empty() ? commands_.remove(user, stock) : commands_.remove_politician(user, politician);
    }
    if (name == "watch") return commands_.watch(user, stock, amount);
    if (name == "unwatch") return commands_.unwatch(user, stock, amount);
    if (name == "safety") return commands_.safety(user, shares, amount, stock);
    if (name == "buy") return commands_.buy(user, stock, shares, amount);
    if (name == "sell") return commands_.sell(user, stock, shares, amount);
    if (name == "report") return commands_.report(user, stock);
    if (name == "suggest") return commands_.suggest(user, stock);
    if (name == "news") return commands_.news(user, stock);
    if (name == "arena") return commands_.arena(user, stock);
    if (name == "pythia") return commands_.pythia(user);
    if (name == "politicians") return commands_.politicians(user, politician);
    return {false, "Unknown command."};
}

void DiscordBot::send(int64_t notification_id, const std::string& user_id, const std::string& message) {
    cluster_.direct_message_create(
        to_snowflake(user_id), dpp::message(fit_message(message)),
        [this, notification_id, user_id, message](const dpp::confirmation_callback_t& cb) {
            if (!cb.is_error()) {
                repo_.mark_notification(notification_id, true, "");
                return;
            }
            // DMs can be disabled by the user; fall back to the channel they last used a command in.
            const std::string dm_error = cb.get_error().message;
            const std::string channel = repo_.user_notify_channel(user_id);
            if (channel.empty()) {
                repo_.mark_notification(notification_id, false, dm_error);
                log_warn("could not DM user " + user_id + ": " + dm_error);
                return;
            }
            cluster_.message_create(
                dpp::message(to_snowflake(channel), fit_message("<@" + user_id + "> " + message)),
                [this, notification_id, dm_error](const dpp::confirmation_callback_t& cb2) {
                    const bool ok = !cb2.is_error();
                    repo_.mark_notification(notification_id, ok, ok ? "" : dm_error + "; " + cb2.get_error().message);
                });
        });
}

}  // namespace strojanka
