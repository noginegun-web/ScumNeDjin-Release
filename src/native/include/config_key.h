#pragma once

#include <string>
#include <vector>

namespace warden {

// Produces a file-system-safe config/state key without accepting path syntax.
std::string SafeConfigKey(std::string value);

// Keeps known historical plugin-key spellings on one runtime authority path.
// It deliberately does not rename, merge, or delete any existing files.
std::string CanonicalPluginConfigKey(std::string value);

// Historical spellings that must be diagnosed and migrated deliberately, never
// overwritten or merged implicitly.  The list is empty for ordinary keys.
std::vector<std::string> LegacyPluginConfigAliases(const std::string& value);

// Public host packages must not enable the HTTP API with a blank, too-short,
// or intentionally non-secret template key.  Returns a trimmed usable key or
// an empty string when the supplied value must fail closed.
std::string PublicReleaseApiKeyOrEmpty(std::string value);

// Validates a complete JSON object before it can become a live module config
// authority. Scalars and top-level arrays are intentionally not accepted.
bool IsValidJsonObject(const std::string& value);

}
