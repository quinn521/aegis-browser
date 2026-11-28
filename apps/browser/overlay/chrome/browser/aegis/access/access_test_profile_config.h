// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_PROFILE_CONFIG_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_PROFILE_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"
#include "components/aegis_access/access_inbound_preferences.h"

namespace aegis::access {

// A connection Profile inside an actual Browser Profile. Only an opaque
// Keychain reference is serializable; node fields and runtime endpoints are not.
struct AccessTestProfileDraft {
  std::string id;
  std::string name;
  std::string secret_reference;
  aegis_access::AccessInboundPreferences inbound;
  bool operator==(const AccessTestProfileDraft&) const = default;
};

struct AccessStoredTestProfile {
  AccessTestProfileDraft configuration;
  uint64_t version = 0;
  uint64_t committed_revision = 0;
  std::string operation_id;
  bool operator==(const AccessStoredTestProfile&) const = default;
};

// Decimal strings preserve exact SQLite revisions across WebUI/JSON. Parsing
// never passes an untrusted value through a JavaScript double or signed cast.
std::optional<uint64_t> ParseAccessOptionsRevision(std::string_view value);
bool IsValidAccessTestProfileDraft(const AccessTestProfileDraft& profile);
std::optional<base::DictValue> EncodeAccessTestProfileDraft(
    const AccessTestProfileDraft& profile);
std::optional<AccessTestProfileDraft> DecodeAccessTestProfileDraft(
    const base::DictValue& value);
std::optional<base::DictValue> EncodeAccessStoredTestProfile(
    const AccessStoredTestProfile& profile);
std::optional<AccessStoredTestProfile> DecodeAccessStoredTestProfile(
    const base::DictValue& value);

}  // namespace aegis::access
#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_PROFILE_CONFIG_H_
