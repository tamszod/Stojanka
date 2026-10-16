#pragma once

#include <string>

#include "common/config.h"

// Plain-text email through the user's own mail account (SMTP over TLS, libcurl), for backup
// reminders. Free: it uses whatever mailbox the user already has; Gmail needs an app password.
namespace strojanka {

// The mail server for an address when the settings don't name one: Gmail, Outlook/Hotmail/Live,
// Yahoo and iCloud are known. "" if unknown.
std::string guess_smtp_server(const std::string& address);

// Sends to config.backup_email from that same address. False with *error if it isn't set up or fails.
bool send_email(const Config& config, const std::string& subject, const std::string& body, std::string* error);

}  // namespace strojanka
