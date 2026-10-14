#pragma once

#include <string>

// Minimal, dependency-free HTML/XML text helpers shared by anything that reads a web page
// (RSS feeds, scraped government filings...). Not a real parser: good enough for pulling plain
// text out of simple tags and entities, not for anything that needs to understand structure.
namespace strojanka::html {

// Expands named (&amp; &lt; &gt; &quot; &apos; &nbsp;) and numeric (&#39; &#x27;...) entities.
std::string decode_entities(std::string s);

// Removes everything between < and >, including the brackets. Leaves entities untouched.
std::string strip_tags(const std::string& s);

// strip_tags + decode_entities + trim, the common case for "give me the text of this element".
std::string text_of(const std::string& s);

}  // namespace strojanka::html
