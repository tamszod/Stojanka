#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace strojanka {

std::string trim(const std::string& s);
std::string to_upper(std::string s);
std::string to_lower(std::string s);
std::vector<std::string> split(const std::string& s, char sep);
bool starts_with(const std::string& s, const std::string& prefix);

// Parses user-typed amounts: "59.78", "59.78$", "$1,234.50", "12,5 USD" (a single comma
// followed by at most two digits is read as a decimal comma).
std::optional<double> parse_amount(const std::string& text);

std::string currency_symbol(const std::string& currency);
std::string format_double(double v, int decimals);
std::string format_money(double v, const std::string& currency = "USD");
std::string format_signed_money(double v, const std::string& currency = "USD");
std::string format_shares(double shares);
std::string format_percent(double fraction, int decimals = 1);

std::string utc_now_iso();
std::string unix_to_iso(int64_t ts);
std::string unix_to_date(int64_t ts);
int64_t iso_to_unix(const std::string& iso);  // accepts "YYYY-MM-DD" or "YYYY-MM-DDTHH:MM:SSZ"
int64_t unix_now();

// Ticker symbols: letters, digits and . - ^ = (e.g. BRK-B, ^GSPC, EURUSD=X), max 15 chars.
std::string normalize_symbol(const std::string& s);
bool is_valid_symbol(const std::string& symbol);

}  // namespace strojanka
