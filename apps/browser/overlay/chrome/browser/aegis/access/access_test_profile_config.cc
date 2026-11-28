// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_test_profile_config.h"

#include <limits>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"

namespace aegis::access {
namespace {
constexpr uint64_t kMaximumRevision = std::numeric_limits<int64_t>::max();

bool ValidId(std::string_view value) {
  return base::Uuid::ParseLowercase(value).is_valid();
}

std::optional<AccessTestProfileDraft> ReadDraftFields(const base::DictValue& value) {
  const auto* id = value.FindString("id");
  const auto* name = value.FindString("name");
  const auto* reference = value.FindString("secretReference");
  const auto* inbound = value.FindDict("inbound");
  if (!id || !name || !reference || !inbound || inbound->size() != 3)
    return std::nullopt;
  const auto* protocol = inbound->FindString("protocol");
  const auto* mode = inbound->FindString("portMode");
  const auto port = inbound->FindInt("requestedPort");
  if (!protocol || !mode || !port || *port < 0) return std::nullopt;
  AccessTestProfileDraft result{
      *id, *name, *reference,
      {aegis_access::ParseAccessInboundProtocolPreference(*protocol),
       aegis_access::ParseAccessInboundPortMode(*mode), static_cast<uint32_t>(*port)}};
  return IsValidAccessTestProfileDraft(result) ? std::make_optional(std::move(result))
                                              : std::nullopt;
}
}  // namespace

std::optional<uint64_t> ParseAccessOptionsRevision(std::string_view text) {
  if (text.empty() || text.size() > 19 ||
      (text.size() > 1 && text.front() == '0')) return std::nullopt;
  uint64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') return std::nullopt;
    const auto number = static_cast<uint64_t>(digit - '0');
    if (value > (kMaximumRevision - number) / 10) return std::nullopt;
    value = value * 10 + number;
  }
  return value;
}

bool IsValidAccessTestProfileDraft(const AccessTestProfileDraft& profile) {
  if (!ValidId(profile.id) || !ValidId(profile.secret_reference) ||
      profile.name.empty() || profile.name.size() > 96 ||
      !base::IsStringUTF8(profile.name) || profile.name.find("://") != std::string::npos ||
      aegis_access::ValidateAccessInboundPreferences(profile.inbound) !=
          aegis_access::AccessInboundPreferenceError::kNone) return false;
  for (const unsigned char byte : profile.name) {
    if (byte < 0x20 || byte == 0x7f) return false;
  }
  return true;
}

std::optional<base::DictValue> EncodeAccessTestProfileDraft(
    const AccessTestProfileDraft& profile) {
  if (!IsValidAccessTestProfileDraft(profile)) return std::nullopt;
  using Protocol = aegis_access::AccessInboundProtocolPreference;
  const char* protocol = profile.inbound.protocol == Protocol::kAuto ? "auto"
      : profile.inbound.protocol == Protocol::kHttp ? "http" : "socks5";
  const char* mode = profile.inbound.port_mode ==
      aegis_access::AccessInboundPortMode::kSystemAllocated ? "random" : "fixed";
  return base::DictValue().Set("id", profile.id).Set("name", profile.name)
      .Set("secretReference", profile.secret_reference)
      .Set("inbound", base::DictValue().Set("protocol", protocol)
          .Set("portMode", mode).Set("requestedPort", static_cast<int>(profile.inbound.requested_port)));
}

std::optional<AccessTestProfileDraft> DecodeAccessTestProfileDraft(
    const base::DictValue& value) {
  return value.size() == 4 ? ReadDraftFields(value) : std::nullopt;
}

std::optional<base::DictValue> EncodeAccessStoredTestProfile(
    const AccessStoredTestProfile& profile) {
  if (profile.version == 0 || profile.version > profile.committed_revision ||
      profile.committed_revision > kMaximumRevision || !ValidId(profile.operation_id))
    return std::nullopt;
  auto encoded = EncodeAccessTestProfileDraft(profile.configuration);
  if (!encoded) return std::nullopt;
  encoded->Set("schemaVersion", 1);
  encoded->Set("version", base::NumberToString(profile.version));
  encoded->Set("committedRevision", base::NumberToString(profile.committed_revision));
  encoded->Set("operationID", profile.operation_id);
  return encoded;
}

std::optional<AccessStoredTestProfile> DecodeAccessStoredTestProfile(
    const base::DictValue& value) {
  if (value.size() != 8 || value.FindInt("schemaVersion") != 1) return std::nullopt;
  auto draft = ReadDraftFields(value);
  const auto* version_text = value.FindString("version");
  const auto* revision_text = value.FindString("committedRevision");
  const auto* operation = value.FindString("operationID");
  if (!draft || !version_text || !revision_text || !operation || !ValidId(*operation))
    return std::nullopt;
  const auto version = ParseAccessOptionsRevision(*version_text);
  const auto revision = ParseAccessOptionsRevision(*revision_text);
  if (!version || !revision || *version == 0 || *version > *revision) return std::nullopt;
  return AccessStoredTestProfile{std::move(*draft), *version, *revision, *operation};
}
}  // namespace aegis::access
