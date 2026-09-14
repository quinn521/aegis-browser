// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_rule_store.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

#include "sql/statement.h"
#include "sql/transaction.h"

namespace aegis::access {
namespace {

constexpr int kCurrentVersion = 1;
constexpr int kCompatibleVersion = 1;
constexpr int kCompleteSchemeMask = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3);
constexpr size_t kMaxOpaqueIdBytes = 256;
constexpr size_t kMaxSchemefulSiteBytes = 512;

constexpr char kCreateIdentitySql[] = R"(
  CREATE TABLE IF NOT EXISTS access_store_identity(
    singleton INTEGER PRIMARY KEY NOT NULL CHECK(singleton=1),
    channel INTEGER NOT NULL,
    profile_token TEXT NOT NULL
  ))";

constexpr char kCreateCountersSql[] = R"(
  CREATE TABLE IF NOT EXISTS access_store_counters(
    singleton INTEGER PRIMARY KEY NOT NULL CHECK(singleton=1),
    operation_sequence INTEGER NOT NULL,
    policy_generation INTEGER NOT NULL
  ))";

constexpr char kCreateGroupsSql[] = R"(
  CREATE TABLE IF NOT EXISTS access_site_groups(
    storage_partition_token TEXT NOT NULL,
    site_toggle_id TEXT NOT NULL,
    canonical_host TEXT NOT NULL,
    http_top_level_site TEXT NOT NULL,
    https_top_level_site TEXT NOT NULL,
    http_member_rule_id TEXT NOT NULL,
    https_member_rule_id TEXT NOT NULL,
    revision INTEGER NOT NULL,
    last_operation_sequence INTEGER NOT NULL,
    policy_generation INTEGER NOT NULL,
    PRIMARY KEY(storage_partition_token, site_toggle_id),
    UNIQUE(storage_partition_token, canonical_host)
  ))";

constexpr char kCreateRulesSql[] = R"(
  CREATE TABLE IF NOT EXISTS access_rules(
    storage_partition_token TEXT NOT NULL,
    rule_id TEXT NOT NULL,
    site_toggle_id TEXT NOT NULL DEFAULT '',
    group_revision INTEGER NOT NULL DEFAULT 0,
    top_level_site TEXT NOT NULL,
    exact_host TEXT NOT NULL,
    schemes_mask INTEGER NOT NULL,
    port_scope INTEGER NOT NULL,
    include_subdomains INTEGER NOT NULL,
    mode INTEGER NOT NULL,
    proxy_group_id TEXT NOT NULL DEFAULT '',
    protection_override INTEGER NOT NULL,
    row_revision INTEGER NOT NULL,
    last_operation_sequence INTEGER NOT NULL,
    PRIMARY KEY(storage_partition_token, rule_id)
  ))";

constexpr char kCreateOperationsSql[] = R"(
  CREATE TABLE IF NOT EXISTS access_pending_operations(
    operation_id TEXT PRIMARY KEY NOT NULL,
    request_fingerprint TEXT NOT NULL,
    state INTEGER NOT NULL,
    storage_partition_token TEXT NOT NULL,
    site_toggle_id TEXT NOT NULL,
    canonical_host TEXT NOT NULL,
    http_top_level_site TEXT NOT NULL,
    https_top_level_site TEXT NOT NULL,
    http_member_rule_id TEXT NOT NULL,
    https_member_rule_id TEXT NOT NULL,
    target_mode INTEGER NOT NULL,
    proxy_group_id TEXT NOT NULL DEFAULT '',
    expected_revision INTEGER NOT NULL,
    target_revision INTEGER NOT NULL,
    operation_sequence INTEGER NOT NULL,
    policy_generation INTEGER NOT NULL,
    service_incarnation INTEGER NOT NULL,
    route_ready INTEGER NOT NULL DEFAULT 0,
    publish_started INTEGER NOT NULL DEFAULT 0
  ))";

constexpr char kCreateContextsSql[] = R"(
  CREATE TABLE IF NOT EXISTS access_operation_contexts(
    operation_id TEXT NOT NULL,
    context_id TEXT NOT NULL,
    context_incarnation INTEGER NOT NULL,
    acknowledged INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(operation_id, context_id)
  ))";

bool IsKnownChannel(aegis_access::ChannelNamespace channel) {
  return channel == aegis_access::ChannelNamespace::kDev ||
         channel == aegis_access::ChannelNamespace::kAlpha ||
         channel == aegis_access::ChannelNamespace::kBeta ||
         channel == aegis_access::ChannelNamespace::kRelease;
}

bool FitsSqlInt64(uint64_t value) {
  return value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
}

bool IsBounded(std::string_view value, size_t maximum) {
  return !value.empty() && value.size() <= maximum;
}

bool IsCanonicalHostLike(std::string_view host) {
  if (!IsBounded(host, 253) || host.back() == '.') {
    return false;
  }
  const bool bracketed_ipv6 = host.front() == '[' && host.back() == ']';
  for (unsigned char character : host) {
    if (character >= 0x80 || std::isspace(character) ||
        std::isupper(character) || character == '/' || character == '\\' ||
        character == '@' || character == '?' || character == '#' ||
        character == '%') {
      return false;
    }
    if (character == ':' && !bracketed_ipv6) {
      return false;
    }
  }
  if (bracketed_ipv6) {
    if (host.size() < 4 || host.find(':') == std::string_view::npos) {
      return false;
    }
    return std::ranges::all_of(host.substr(1, host.size() - 2),
                               [](unsigned char character) {
                                 return std::isdigit(character) ||
                                        (character >= 'a' &&
                                         character <= 'f') ||
                                        character == ':' || character == '.';
                               });
  }

  size_t label_start = 0;
  while (label_start < host.size()) {
    const size_t label_end = host.find('.', label_start);
    const size_t resolved_end =
        label_end == std::string_view::npos ? host.size() : label_end;
    const std::string_view label =
        host.substr(label_start, resolved_end - label_start);
    if (label.empty() || label.size() > 63 || label.front() == '-' ||
        label.back() == '-' ||
        !std::ranges::all_of(label, [](unsigned char character) {
          return std::islower(character) || std::isdigit(character) ||
                 character == '-';
        })) {
      return false;
    }
    label_start = resolved_end + 1;
  }
  return true;
}

bool IsSchemefulSiteLike(std::string_view value, std::string_view scheme) {
  if (!IsBounded(value, kMaxSchemefulSiteBytes) ||
      !value.starts_with(scheme)) {
    return false;
  }
  const std::string_view host = value.substr(scheme.size());
  return IsCanonicalHostLike(host);
}

int SchemeMask(const std::vector<aegis_access::RequestScheme>& schemes) {
  int mask = 0;
  for (aegis_access::RequestScheme scheme : schemes) {
    switch (scheme) {
      case aegis_access::RequestScheme::kHttp:
        mask |= 1 << 0;
        break;
      case aegis_access::RequestScheme::kHttps:
        mask |= 1 << 1;
        break;
      case aegis_access::RequestScheme::kWs:
        mask |= 1 << 2;
        break;
      case aegis_access::RequestScheme::kWss:
        mask |= 1 << 3;
        break;
      case aegis_access::RequestScheme::kInvalid:
        return 0;
    }
  }
  return mask;
}

std::vector<aegis_access::RequestScheme> CompleteSchemes() {
  return {aegis_access::RequestScheme::kHttp,
          aegis_access::RequestScheme::kHttps, aegis_access::RequestScheme::kWs,
          aegis_access::RequestScheme::kWss};
}

std::vector<aegis_access::RequestScheme> SchemesFromMask(int mask) {
  std::vector<aegis_access::RequestScheme> schemes;
  if (mask & (1 << 0)) {
    schemes.push_back(aegis_access::RequestScheme::kHttp);
  }
  if (mask & (1 << 1)) {
    schemes.push_back(aegis_access::RequestScheme::kHttps);
  }
  if (mask & (1 << 2)) {
    schemes.push_back(aegis_access::RequestScheme::kWs);
  }
  if (mask & (1 << 3)) {
    schemes.push_back(aegis_access::RequestScheme::kWss);
  }
  return schemes;
}

aegis_access::OwnershipKey RowOwner(
    const aegis_access::OwnershipKey& store_owner,
    const std::string& storage_partition_token) {
  return {store_owner.channel, store_owner.profile_token,
          storage_partition_token};
}

bool SameRequest(const SiteToggleRequest& left,
                 const SiteToggleRequest& right) {
  std::set<std::pair<std::string, uint64_t>> left_contexts;
  std::set<std::pair<std::string, uint64_t>> right_contexts;
  for (const auto& context : left.required_contexts) {
    left_contexts.emplace(context.context_id, context.incarnation);
  }
  for (const auto& context : right.required_contexts) {
    right_contexts.emplace(context.context_id, context.incarnation);
  }
  return left.operation_id == right.operation_id &&
         left.request_fingerprint == right.request_fingerprint &&
         left.site_toggle_id == right.site_toggle_id &&
         left.site == right.site && left.enabled == right.enabled &&
         left.proxy_group_id == right.proxy_group_id &&
         left.expected_revision == right.expected_revision &&
         left.service_incarnation == right.service_incarnation &&
         left_contexts == right_contexts;
}

bool IsValidIndependentRule(const IndependentAccessRule& rule) {
  if (rule.rule_id.empty() || rule.storage_partition_token.empty() ||
      rule.top_level_site.empty() || rule.exact_host.empty() ||
      SchemeMask(rule.schemes) == 0 ||
      rule.ports == aegis_access::PortScope::kInvalid ||
      rule.mode == aegis_access::AccessMode::kNone ||
      rule.mode == aegis_access::AccessMode::kInvalid ||
      rule.protection_override == aegis_access::ProtectionOverride::kInvalid ||
      rule.row_revision == 0 || rule.last_operation_sequence == 0 ||
      !FitsSqlInt64(rule.row_revision) ||
      !FitsSqlInt64(rule.last_operation_sequence)) {
    return false;
  }
  if (rule.mode == aegis_access::AccessMode::kProxy) {
    return !rule.proxy_group_id.empty();
  }
  return rule.proxy_group_id.empty() &&
         rule.protection_override == aegis_access::ProtectionOverride::kNone;
}

}  // namespace

bool BrowserConfirmedSite::IsComplete() const {
  return IsBounded(storage_partition_token, kMaxOpaqueIdBytes) &&
         IsCanonicalHostLike(canonical_host) &&
         IsSchemefulSiteLike(http_top_level_site, "http://") &&
         IsSchemefulSiteLike(https_top_level_site, "https://") &&
         http_top_level_site.substr(7) == https_top_level_site.substr(8);
}

AccessRuleStore::AccessRuleStore(base::FilePath database_path,
                                 aegis_access::OwnershipKey owner,
                                 bool in_memory)
    : database_path_(std::move(database_path)),
      owner_(std::move(owner)),
      in_memory_(in_memory),
      database_("AegisAccess") {}

AccessRuleStore::~AccessRuleStore() = default;

bool AccessRuleStore::IsBoundOwnerValid() const {
  return IsKnownChannel(owner_.channel) && !owner_.profile_token.empty() &&
         owner_.storage_partition_token.empty();
}

bool AccessRuleStore::ShouldFail(FailurePointForTesting point) const {
  return failure_point_for_testing_ == point;
}

bool AccessRuleStore::Initialize() {
  if (initialized_) {
    return true;
  }
  if (!IsBoundOwnerValid() || !(in_memory_ ? database_.OpenInMemory()
                                           : database_.Open(database_path_))) {
    return false;
  }

  sql::Transaction transaction(&database_);
  if (!transaction.Begin() ||
      !meta_table_.Init(&database_, kCurrentVersion, kCompatibleVersion) ||
      meta_table_.GetVersionNumber() != kCurrentVersion ||
      meta_table_.GetCompatibleVersionNumber() > kCompatibleVersion) {
    database_.Close();
    return false;
  }

  const bool has_identity =
      database_.DoesTableExist("access_store_identity");
  const bool has_counters =
      database_.DoesTableExist("access_store_counters");
  const bool has_groups = database_.DoesTableExist("access_site_groups");
  const bool has_rules = database_.DoesTableExist("access_rules");
  const bool has_operations =
      database_.DoesTableExist("access_pending_operations");
  const bool has_contexts =
      database_.DoesTableExist("access_operation_contexts");
  const bool has_any_access_table =
      has_identity || has_counters || has_groups || has_rules ||
      has_operations || has_contexts;
  const bool has_complete_schema = has_identity && has_counters && has_groups &&
                                   has_rules && has_operations && has_contexts;
  if (has_any_access_table && !has_complete_schema) {
    database_.Close();
    return false;
  }

  if (!database_.Execute(kCreateIdentitySql) ||
      !database_.Execute(kCreateCountersSql) ||
      !database_.Execute(kCreateGroupsSql) ||
      !database_.Execute(kCreateRulesSql) ||
      !database_.Execute(kCreateOperationsSql) ||
      !database_.Execute(kCreateContextsSql)) {
    database_.Close();
    return false;
  }

  sql::Statement identity(database_.GetUniqueStatement(
      "SELECT channel,profile_token FROM access_store_identity "
      "WHERE singleton=1"));
  if (identity.Step()) {
    if (identity.ColumnInt(0) != static_cast<int>(owner_.channel) ||
        identity.ColumnString(1) != owner_.profile_token) {
      database_.Close();
      return false;
    }
  } else if (!identity.Succeeded()) {
    database_.Close();
    return false;
  } else {
    sql::Statement insert_identity(database_.GetUniqueStatement(
        "INSERT INTO access_store_identity(singleton,channel,profile_token) "
        "VALUES(1,?,?)"));
    insert_identity.BindInt(0, static_cast<int>(owner_.channel));
    insert_identity.BindString(1, owner_.profile_token);
    if (!insert_identity.Run()) {
      database_.Close();
      return false;
    }
  }

  if (!database_.Execute(
          "INSERT OR IGNORE INTO access_store_counters"
          "(singleton,operation_sequence,policy_generation) VALUES(1,0,0)") ||
      !transaction.Commit()) {
    database_.Close();
    return false;
  }
  initialized_ = true;
  return true;
}

SiteSelectionRecord AccessRuleStore::ReadSiteSelection(
    const BrowserConfirmedSite& site) {
  if (!initialized_ || !site.IsComplete()) {
    return {};
  }

  sql::Statement pending(database_.GetUniqueStatement(
      "SELECT 1 FROM access_pending_operations WHERE state=0 AND "
      "storage_partition_token=? AND canonical_host=? LIMIT 1"));
  pending.BindString(0, site.storage_partition_token);
  pending.BindString(1, site.canonical_host);
  if (pending.Step()) {
    SiteSelectionRecord result = ReadDurableSiteSelection(site);
    result.state = SiteSelectionReadState::kRestoring;
    result.selection = aegis_access::GroupSelection::kUnknown;
    return result;
  }
  if (!pending.Succeeded()) {
    return {};
  }
  return ReadDurableSiteSelection(site);
}

SiteSelectionRecord AccessRuleStore::ReadDurableSiteSelection(
    const BrowserConfirmedSite& site) {
  SiteSelectionRecord result;
  if (!initialized_ || !site.IsComplete()) {
    return result;
  }

  sql::Statement independent_statement(database_.GetUniqueStatement(
      "SELECT 1 FROM access_rules WHERE storage_partition_token=? AND "
      "exact_host=? AND site_toggle_id='' LIMIT 1"));
  independent_statement.BindString(0, site.storage_partition_token);
  independent_statement.BindString(1, site.canonical_host);
  result.has_independent_rule = independent_statement.Step();
  if (!independent_statement.Succeeded()) {
    return {};
  }

  sql::Statement group_statement(database_.GetUniqueStatement(
      "SELECT site_toggle_id,http_top_level_site,https_top_level_site,"
      "http_member_rule_id,https_member_rule_id,revision,"
      "last_operation_sequence,policy_generation FROM access_site_groups "
      "WHERE storage_partition_token=? AND canonical_host=?"));
  group_statement.BindString(0, site.storage_partition_token);
  group_statement.BindString(1, site.canonical_host);
  if (!group_statement.Step()) {
    if (group_statement.Succeeded()) {
      sql::Statement orphan_statement(database_.GetUniqueStatement(
          "SELECT 1 FROM access_rules WHERE storage_partition_token=? AND "
          "exact_host=? AND site_toggle_id<>'' LIMIT 1"));
      orphan_statement.BindString(0, site.storage_partition_token);
      orphan_statement.BindString(1, site.canonical_host);
      if (orphan_statement.Step()) {
        result.state = SiteSelectionReadState::kCorrupt;
        result.selection = aegis_access::GroupSelection::kUnknown;
        result.validation_error =
            aegis_access::GroupValidationError::kMissingGroup;
      } else if (orphan_statement.Succeeded()) {
        result.state = SiteSelectionReadState::kMissing;
        result.selection = aegis_access::GroupSelection::kUnknown;
        result.validation_error =
            aegis_access::GroupValidationError::kMissingGroup;
      }
    }
    return result;
  }

  aegis_access::SiteProxyRuleGroup group;
  group.site_toggle_id = group_statement.ColumnString(0);
  group.canonical_host = site.canonical_host;
  group.owner = RowOwner(owner_, site.storage_partition_token);
  group.http_top_level_site = group_statement.ColumnString(1);
  group.https_top_level_site = group_statement.ColumnString(2);
  group.member_rule_ids = {group_statement.ColumnString(3),
                           group_statement.ColumnString(4)};
  const int64_t revision = group_statement.ColumnInt64(5);
  const int64_t operation_sequence = group_statement.ColumnInt64(6);
  const int64_t policy_generation = group_statement.ColumnInt64(7);
  group.revision = revision > 0 ? static_cast<uint64_t>(revision) : 0;
  group.last_operation_sequence =
      operation_sequence > 0 ? static_cast<uint64_t>(operation_sequence) : 0;
  result.policy_generation =
      policy_generation > 0 ? static_cast<uint64_t>(policy_generation) : 0;

  if (group.http_top_level_site != site.http_top_level_site ||
      group.https_top_level_site != site.https_top_level_site ||
      group.revision == 0 || group.last_operation_sequence == 0 ||
      result.policy_generation == 0) {
    result.state = SiteSelectionReadState::kCorrupt;
    result.selection = aegis_access::GroupSelection::kUnknown;
    result.validation_error = aegis_access::GroupValidationError::kInvalidGroup;
    return result;
  }

  std::vector<aegis_access::SiteProxyRuleMember> members;
  sql::Statement member_statement(database_.GetUniqueStatement(
      "SELECT rule_id,group_revision,top_level_site,exact_host,schemes_mask,"
      "port_scope,include_subdomains,mode,proxy_group_id,"
      "protection_override,last_operation_sequence FROM access_rules "
      "WHERE storage_partition_token=? AND site_toggle_id=? ORDER BY rule_id"));
  member_statement.BindString(0, site.storage_partition_token);
  member_statement.BindString(1, group.site_toggle_id);
  while (member_statement.Step()) {
    const int schemes_mask = member_statement.ColumnInt(4);
    if (schemes_mask != kCompleteSchemeMask) {
      result.state = SiteSelectionReadState::kCorrupt;
      result.selection = aegis_access::GroupSelection::kUnknown;
      result.validation_error =
          aegis_access::GroupValidationError::kSchemesMismatch;
      return result;
    }
    aegis_access::SiteProxyRuleMember member;
    member.rule_id = member_statement.ColumnString(0);
    member.owner = group.owner;
    member.site_toggle_id = group.site_toggle_id;
    member.group_revision = member_statement.ColumnInt64(1);
    member.top_level_site = member_statement.ColumnString(2);
    member.exact_host = member_statement.ColumnString(3);
    member.schemes = SchemesFromMask(schemes_mask);
    member.ports =
        static_cast<aegis_access::PortScope>(member_statement.ColumnInt(5));
    member.include_subdomains = member_statement.ColumnBool(6);
    member.mode =
        static_cast<aegis_access::AccessMode>(member_statement.ColumnInt(7));
    member.proxy_group_id = member_statement.ColumnString(8);
    member.protection_override = static_cast<aegis_access::ProtectionOverride>(
        member_statement.ColumnInt(9));
    member.last_operation_sequence = member_statement.ColumnInt64(10);
    members.push_back(std::move(member));
  }
  if (!member_statement.Succeeded()) {
    return {};
  }

  const aegis_access::GroupValidation validation =
      aegis_access::ValidateSiteProxyRuleGroup(group.owner, &group, members);
  result.state = validation.error == aegis_access::GroupValidationError::kNone
                     ? SiteSelectionReadState::kValid
                     : SiteSelectionReadState::kCorrupt;
  result.selection = validation.selection;
  result.validation_error = validation.error;
  result.revision = group.revision;
  result.last_operation_sequence = group.last_operation_sequence;
  if (validation.selection == aegis_access::GroupSelection::kEnabled &&
      !members.empty()) {
    result.proxy_group_id = members[0].proxy_group_id;
  }
  return result;
}

PrepareOperationResult AccessRuleStore::PrepareSiteToggleOperation(
    const SiteToggleRequest& request) {
  if (ShouldFail(FailurePointForTesting::kPrepareOperation)) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }
  if (!initialized_ ||
      !IsBounded(request.operation_id, kMaxOpaqueIdBytes) ||
      !IsBounded(request.request_fingerprint, kMaxOpaqueIdBytes) ||
      !IsBounded(request.site_toggle_id, kMaxOpaqueIdBytes) ||
      !request.site.IsComplete() || request.service_incarnation == 0 ||
      request.required_contexts.empty() ||
      (request.enabled &&
       !IsBounded(request.proxy_group_id, kMaxOpaqueIdBytes)) ||
      (!request.enabled && !request.proxy_group_id.empty()) ||
      !FitsSqlInt64(request.expected_revision) ||
      !FitsSqlInt64(request.service_incarnation)) {
    return {PrepareOperationStatus::kInvalidRequest, std::nullopt};
  }
  std::set<std::string> context_ids;
  for (const auto& context : request.required_contexts) {
    if (!IsBounded(context.context_id, kMaxOpaqueIdBytes) ||
        context.incarnation == 0 ||
        !FitsSqlInt64(context.incarnation) ||
        !context_ids.insert(context.context_id).second) {
      return {PrepareOperationStatus::kInvalidRequest, std::nullopt};
    }
  }

  if (std::optional<PendingSiteOperation> duplicate =
          LoadOperation(request.operation_id, false)) {
    return {
        SameRequest(duplicate->request, request)
            ? PrepareOperationStatus::kDuplicate
            : PrepareOperationStatus::kDuplicateConflict,
        SameRequest(duplicate->request, request) ? duplicate : std::nullopt};
  }

  sql::Transaction transaction(&database_);
  if (!transaction.Begin()) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }

  const SiteSelectionRecord current = ReadDurableSiteSelection(request.site);
  if (current.state == SiteSelectionReadState::kCorrupt) {
    return {PrepareOperationStatus::kCorruptState, std::nullopt};
  }
  if (current.state == SiteSelectionReadState::kStorageError) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }
  if (current.revision != request.expected_revision) {
    return {PrepareOperationStatus::kRevisionConflict, std::nullopt};
  }

  if (current.state == SiteSelectionReadState::kValid) {
    sql::Statement id_statement(database_.GetUniqueStatement(
        "SELECT site_toggle_id FROM access_site_groups WHERE "
        "storage_partition_token=? AND canonical_host=?"));
    id_statement.BindString(0, request.site.storage_partition_token);
    id_statement.BindString(1, request.site.canonical_host);
    if (!id_statement.Step() ||
        id_statement.ColumnString(0) != request.site_toggle_id) {
      return {id_statement.Succeeded() ? PrepareOperationStatus::kInvalidRequest
                                       : PrepareOperationStatus::kStorageError,
              std::nullopt};
    }
  }

  sql::Statement toggle_owner(database_.GetUniqueStatement(
      "SELECT canonical_host FROM access_site_groups WHERE "
      "storage_partition_token=? AND site_toggle_id=?"));
  toggle_owner.BindString(0, request.site.storage_partition_token);
  toggle_owner.BindString(1, request.site_toggle_id);
  if (toggle_owner.Step() &&
      toggle_owner.ColumnString(0) != request.site.canonical_host) {
    return {PrepareOperationStatus::kInvalidRequest, std::nullopt};
  }
  if (!toggle_owner.Succeeded()) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }

  sql::Statement pending_toggle_owner(database_.GetUniqueStatement(
      "SELECT canonical_host FROM access_pending_operations WHERE state=0 "
      "AND storage_partition_token=? AND site_toggle_id=? LIMIT 1"));
  pending_toggle_owner.BindString(0, request.site.storage_partition_token);
  pending_toggle_owner.BindString(1, request.site_toggle_id);
  if (pending_toggle_owner.Step() &&
      pending_toggle_owner.ColumnString(0) != request.site.canonical_host) {
    return {PrepareOperationStatus::kInvalidRequest, std::nullopt};
  }
  if (!pending_toggle_owner.Succeeded()) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }

  sql::Statement counters(database_.GetUniqueStatement(
      "SELECT operation_sequence,policy_generation FROM "
      "access_store_counters WHERE singleton=1"));
  if (!counters.Step()) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }
  const int64_t stored_sequence = counters.ColumnInt64(0);
  const int64_t stored_generation = counters.ColumnInt64(1);
  if (stored_sequence < 0 || stored_generation < 0) {
    return {PrepareOperationStatus::kCorruptState, std::nullopt};
  }
  const uint64_t previous_sequence = stored_sequence;
  const uint64_t previous_generation = stored_generation;
  if (previous_sequence ==
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      previous_generation ==
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      current.revision ==
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }

  PendingSiteOperation operation;
  operation.request = request;
  operation.owner = RowOwner(owner_, request.site.storage_partition_token);
  operation.operation_sequence = previous_sequence + 1;
  operation.policy_generation = previous_generation + 1;
  operation.required_contexts = request.required_contexts;
  operation.after_group = {
      .site_toggle_id = request.site_toggle_id,
      .canonical_host = request.site.canonical_host,
      .owner = operation.owner,
      .http_top_level_site = request.site.http_top_level_site,
      .https_top_level_site = request.site.https_top_level_site,
      .member_rule_ids = {request.site_toggle_id + ":http",
                          request.site_toggle_id + ":https"},
      .revision = current.revision + 1,
      .last_operation_sequence = operation.operation_sequence,
  };
  const aegis_access::AccessMode target_mode =
      request.enabled ? aegis_access::AccessMode::kProxy
                      : aegis_access::AccessMode::kDirect;
  for (size_t index = 0; index < 2; ++index) {
    operation.after_members.push_back({
        .rule_id = operation.after_group.member_rule_ids[index],
        .owner = operation.owner,
        .site_toggle_id = request.site_toggle_id,
        .group_revision = operation.after_group.revision,
        .top_level_site = index == 0 ? request.site.http_top_level_site
                                     : request.site.https_top_level_site,
        .exact_host = request.site.canonical_host,
        .schemes = CompleteSchemes(),
        .ports = aegis_access::PortScope::kAllBrowserPermitted,
        .include_subdomains = false,
        .mode = target_mode,
        .proxy_group_id = request.enabled ? request.proxy_group_id : "",
        .protection_override = aegis_access::ProtectionOverride::kNone,
        .last_operation_sequence = operation.operation_sequence,
    });
  }

  sql::Statement supersede(database_.GetUniqueStatement(
      "UPDATE access_pending_operations SET state=1 WHERE state=0 AND "
      "storage_partition_token=? AND canonical_host=?"));
  supersede.BindString(0, request.site.storage_partition_token);
  supersede.BindString(1, request.site.canonical_host);
  if (!supersede.Run()) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }

  sql::Statement insert(database_.GetUniqueStatement(
      "INSERT INTO access_pending_operations("
      "operation_id,request_fingerprint,state,storage_partition_token,"
      "site_toggle_id,canonical_host,http_top_level_site,"
      "https_top_level_site,http_member_rule_id,https_member_rule_id,"
      "target_mode,proxy_group_id,expected_revision,target_revision,"
      "operation_sequence,policy_generation,service_incarnation,route_ready)"
      " VALUES(?,?,0,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0)"));
  insert.BindString(0, request.operation_id);
  insert.BindString(1, request.request_fingerprint);
  insert.BindString(2, request.site.storage_partition_token);
  insert.BindString(3, request.site_toggle_id);
  insert.BindString(4, request.site.canonical_host);
  insert.BindString(5, request.site.http_top_level_site);
  insert.BindString(6, request.site.https_top_level_site);
  insert.BindString(7, operation.after_group.member_rule_ids[0]);
  insert.BindString(8, operation.after_group.member_rule_ids[1]);
  insert.BindInt(9, static_cast<int>(target_mode));
  insert.BindString(10, request.proxy_group_id);
  insert.BindInt64(11, request.expected_revision);
  insert.BindInt64(12, operation.after_group.revision);
  insert.BindInt64(13, operation.operation_sequence);
  insert.BindInt64(14, operation.policy_generation);
  insert.BindInt64(15, request.service_incarnation);
  if (!insert.Run()) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }

  for (const auto& context : request.required_contexts) {
    sql::Statement insert_context(database_.GetUniqueStatement(
        "INSERT INTO access_operation_contexts("
        "operation_id,context_id,context_incarnation,acknowledged) "
        "VALUES(?,?,?,0)"));
    insert_context.BindString(0, request.operation_id);
    insert_context.BindString(1, context.context_id);
    insert_context.BindInt64(2, context.incarnation);
    if (!insert_context.Run()) {
      return {PrepareOperationStatus::kStorageError, std::nullopt};
    }
  }

  sql::Statement update_counters(database_.GetUniqueStatement(
      "UPDATE access_store_counters SET operation_sequence=?,"
      "policy_generation=? WHERE singleton=1"));
  update_counters.BindInt64(0, operation.operation_sequence);
  update_counters.BindInt64(1, operation.policy_generation);
  if (!update_counters.Run() || database_.GetLastChangeCount() != 1 ||
      !transaction.Commit()) {
    return {PrepareOperationStatus::kStorageError, std::nullopt};
  }
  return {PrepareOperationStatus::kPrepared, std::move(operation)};
}

std::optional<PendingSiteOperation> AccessRuleStore::LoadPendingOperation(
    const std::string& operation_id) {
  return LoadOperation(operation_id, true);
}

std::optional<PendingSiteOperation> AccessRuleStore::LoadOperation(
    const std::string& operation_id,
    bool active_only) {
  if (!initialized_ || operation_id.empty()) {
    return std::nullopt;
  }
  const char* sql =
      active_only ? "SELECT request_fingerprint,storage_partition_token,"
                    "site_toggle_id,canonical_host,http_top_level_site,"
                    "https_top_level_site,http_member_rule_id,"
                    "https_member_rule_id,target_mode,proxy_group_id,"
                    "expected_revision,target_revision,operation_sequence,"
                    "policy_generation,service_incarnation,route_ready,"
                    "publish_started "
                    "FROM access_pending_operations WHERE operation_id=? "
                    "AND state=0"
                  : "SELECT request_fingerprint,storage_partition_token,"
                    "site_toggle_id,canonical_host,http_top_level_site,"
                    "https_top_level_site,http_member_rule_id,"
                    "https_member_rule_id,target_mode,proxy_group_id,"
                    "expected_revision,target_revision,operation_sequence,"
                    "policy_generation,service_incarnation,route_ready,"
                    "publish_started "
                    "FROM access_pending_operations WHERE operation_id=?";
  sql::Statement statement(database_.GetUniqueStatement(sql));
  statement.BindString(0, operation_id);
  if (!statement.Step()) {
    return std::nullopt;
  }

  PendingSiteOperation operation;
  operation.request.operation_id = operation_id;
  operation.request.request_fingerprint = statement.ColumnString(0);
  operation.request.site.storage_partition_token = statement.ColumnString(1);
  operation.request.site_toggle_id = statement.ColumnString(2);
  operation.request.site.canonical_host = statement.ColumnString(3);
  operation.request.site.http_top_level_site = statement.ColumnString(4);
  operation.request.site.https_top_level_site = statement.ColumnString(5);
  const auto target_mode =
      static_cast<aegis_access::AccessMode>(statement.ColumnInt(8));
  if (target_mode != aegis_access::AccessMode::kDirect &&
      target_mode != aegis_access::AccessMode::kProxy) {
    return std::nullopt;
  }
  operation.request.enabled = target_mode == aegis_access::AccessMode::kProxy;
  operation.request.proxy_group_id = statement.ColumnString(9);
  const int64_t expected_revision = statement.ColumnInt64(10);
  const int64_t target_revision = statement.ColumnInt64(11);
  const int64_t operation_sequence = statement.ColumnInt64(12);
  const int64_t policy_generation = statement.ColumnInt64(13);
  const int64_t service_incarnation = statement.ColumnInt64(14);
  if (expected_revision < 0 || target_revision <= 0 ||
      operation_sequence <= 0 || policy_generation <= 0 ||
      service_incarnation <= 0 ||
      (operation.request.enabled && operation.request.proxy_group_id.empty()) ||
      (!operation.request.enabled &&
       !operation.request.proxy_group_id.empty())) {
    return std::nullopt;
  }
  operation.request.expected_revision = expected_revision;
  operation.request.service_incarnation = service_incarnation;
  operation.operation_sequence = operation_sequence;
  operation.policy_generation = policy_generation;
  operation.route_ready = statement.ColumnBool(15);
  operation.publish_started = statement.ColumnBool(16);
  operation.owner =
      RowOwner(owner_, operation.request.site.storage_partition_token);
  operation.after_group = {
      .site_toggle_id = operation.request.site_toggle_id,
      .canonical_host = operation.request.site.canonical_host,
      .owner = operation.owner,
      .http_top_level_site = operation.request.site.http_top_level_site,
      .https_top_level_site = operation.request.site.https_top_level_site,
      .member_rule_ids = {statement.ColumnString(6), statement.ColumnString(7)},
      .revision = static_cast<uint64_t>(target_revision),
      .last_operation_sequence = operation.operation_sequence,
  };
  for (size_t index = 0; index < 2; ++index) {
    operation.after_members.push_back({
        .rule_id = operation.after_group.member_rule_ids[index],
        .owner = operation.owner,
        .site_toggle_id = operation.request.site_toggle_id,
        .group_revision = operation.after_group.revision,
        .top_level_site = index == 0
                              ? operation.request.site.http_top_level_site
                              : operation.request.site.https_top_level_site,
        .exact_host = operation.request.site.canonical_host,
        .schemes = CompleteSchemes(),
        .ports = aegis_access::PortScope::kAllBrowserPermitted,
        .include_subdomains = false,
        .mode = target_mode,
        .proxy_group_id =
            operation.request.enabled ? operation.request.proxy_group_id : "",
        .protection_override = aegis_access::ProtectionOverride::kNone,
        .last_operation_sequence = operation.operation_sequence,
    });
  }

  sql::Statement contexts(database_.GetUniqueStatement(
      "SELECT context_id,context_incarnation,acknowledged FROM "
      "access_operation_contexts WHERE operation_id=? ORDER BY context_id"));
  contexts.BindString(0, operation_id);
  while (contexts.Step()) {
    const int64_t context_incarnation = contexts.ColumnInt64(1);
    if (context_incarnation <= 0) {
      return std::nullopt;
    }
    ExecutionContextBinding context{contexts.ColumnString(0),
                                    static_cast<uint64_t>(context_incarnation)};
    if (!IsBounded(context.context_id, kMaxOpaqueIdBytes)) {
      return std::nullopt;
    }
    operation.required_contexts.push_back(context);
    operation.request.required_contexts.push_back(context);
    if (contexts.ColumnBool(2)) {
      operation.acknowledged_context_ids.push_back(context.context_id);
    }
  }
  if (!contexts.Succeeded()) {
    return std::nullopt;
  }
  if (!operation.request.site.IsComplete() ||
      !IsBounded(operation.request.operation_id, kMaxOpaqueIdBytes) ||
      !IsBounded(operation.request.request_fingerprint, kMaxOpaqueIdBytes) ||
      !IsBounded(operation.request.site_toggle_id, kMaxOpaqueIdBytes) ||
      operation.required_contexts.empty() ||
      aegis_access::ValidateSiteProxyRuleGroup(
          operation.owner, &operation.after_group, operation.after_members)
              .error != aegis_access::GroupValidationError::kNone) {
    return std::nullopt;
  }
  return operation;
}

std::vector<PendingSiteOperation> AccessRuleStore::LoadPendingOperations() {
  std::vector<PendingSiteOperation> operations;
  last_pending_load_had_error_ = false;
  if (!initialized_) {
    last_pending_load_had_error_ = true;
    return operations;
  }
  sql::Statement statement(database_.GetUniqueStatement(
      "SELECT operation_id FROM access_pending_operations WHERE state=0 "
      "ORDER BY operation_sequence"));
  while (statement.Step()) {
    auto operation = LoadOperation(statement.ColumnString(0), true);
    if (!operation) {
      operations.clear();
      last_pending_load_had_error_ = true;
      return operations;
    }
    operations.push_back(std::move(*operation));
  }
  if (!statement.Succeeded()) {
    operations.clear();
    last_pending_load_had_error_ = true;
  }
  return operations;
}

std::optional<PendingSiteOperation>
AccessRuleStore::LoadPendingOperationForSite(
    const BrowserConfirmedSite& site) {
  last_pending_load_had_error_ = false;
  if (!initialized_ || !site.IsComplete()) {
    return std::nullopt;
  }
  sql::Statement statement(database_.GetUniqueStatement(
      "SELECT operation_id FROM access_pending_operations WHERE state=0 AND "
      "storage_partition_token=? AND canonical_host=? ORDER BY "
      "operation_sequence DESC LIMIT 1"));
  statement.BindString(0, site.storage_partition_token);
  statement.BindString(1, site.canonical_host);
  if (!statement.Step()) {
    last_pending_load_had_error_ = !statement.Succeeded();
    return std::nullopt;
  }
  std::optional<PendingSiteOperation> operation =
      LoadOperation(statement.ColumnString(0), true);
  last_pending_load_had_error_ = !operation;
  return operation;
}

bool AccessRuleStore::MarkRouteReady(const PendingSiteOperation& operation) {
  if (ShouldFail(FailurePointForTesting::kMarkRouteReady) || !initialized_) {
    return false;
  }
  sql::Statement statement(database_.GetUniqueStatement(
      "UPDATE access_pending_operations SET route_ready=1 WHERE "
      "operation_id=? AND state=0 AND operation_sequence=? AND "
      "policy_generation=?"));
  statement.BindString(0, operation.request.operation_id);
  statement.BindInt64(1, operation.operation_sequence);
  statement.BindInt64(2, operation.policy_generation);
  return statement.Run() && database_.GetLastChangeCount() == 1;
}

bool AccessRuleStore::MarkPublishStarted(
    const PendingSiteOperation& operation) {
  if (ShouldFail(FailurePointForTesting::kMarkPublishStarted) ||
      !initialized_) {
    return false;
  }
  sql::Statement statement(database_.GetUniqueStatement(
      "UPDATE access_pending_operations SET publish_started=1 WHERE "
      "operation_id=? AND state=0 AND operation_sequence=? AND "
      "policy_generation=? AND publish_started=0 AND "
      "(target_mode=? OR route_ready=1)"));
  statement.BindString(0, operation.request.operation_id);
  statement.BindInt64(1, operation.operation_sequence);
  statement.BindInt64(2, operation.policy_generation);
  statement.BindInt(3,
                    static_cast<int>(aegis_access::AccessMode::kDirect));
  return statement.Run() && database_.GetLastChangeCount() == 1;
}

bool AccessRuleStore::MarkOperationFailed(
    const PendingSiteOperation& operation) {
  if (!initialized_) {
    return false;
  }
  sql::Statement statement(database_.GetUniqueStatement(
      "UPDATE access_pending_operations SET state=3 WHERE operation_id=? "
      "AND state=0 AND operation_sequence=? AND policy_generation=? AND "
      "publish_started=0"));
  statement.BindString(0, operation.request.operation_id);
  statement.BindInt64(1, operation.operation_sequence);
  statement.BindInt64(2, operation.policy_generation);
  return statement.Run() && database_.GetLastChangeCount() == 1;
}

bool AccessRuleStore::MarkContextAcknowledged(
    const PendingSiteOperation& operation,
    const ExecutionContextBinding& context) {
  if (ShouldFail(FailurePointForTesting::kMarkAcknowledged) || !initialized_) {
    return false;
  }
  sql::Statement statement(database_.GetUniqueStatement(
      "UPDATE access_operation_contexts SET acknowledged=1 WHERE "
      "operation_id=? AND context_id=? AND context_incarnation=? AND "
      "EXISTS(SELECT 1 FROM access_pending_operations WHERE operation_id=? "
      "AND state=0 AND operation_sequence=? AND policy_generation=? AND "
      "publish_started=1)"));
  statement.BindString(0, operation.request.operation_id);
  statement.BindString(1, context.context_id);
  statement.BindInt64(2, context.incarnation);
  statement.BindString(3, operation.request.operation_id);
  statement.BindInt64(4, operation.operation_sequence);
  statement.BindInt64(5, operation.policy_generation);
  return statement.Run() && database_.GetLastChangeCount() == 1;
}

std::optional<PendingSiteOperation>
AccessRuleStore::RebindOperationForRecovery(
    const PendingSiteOperation& operation,
    uint64_t service_incarnation,
    const std::vector<ExecutionContextBinding>& required_contexts) {
  if (ShouldFail(FailurePointForTesting::kRebindRecovery) || !initialized_ ||
      service_incarnation == 0 || !FitsSqlInt64(service_incarnation) ||
      required_contexts.empty()) {
    return std::nullopt;
  }
  std::set<std::string> context_ids;
  for (const auto& context : required_contexts) {
    if (!IsBounded(context.context_id, kMaxOpaqueIdBytes) ||
        context.incarnation == 0 ||
        !FitsSqlInt64(context.incarnation) ||
        !context_ids.insert(context.context_id).second) {
      return std::nullopt;
    }
  }

  sql::Transaction transaction(&database_);
  if (!transaction.Begin()) {
    return std::nullopt;
  }
  sql::Statement update(database_.GetUniqueStatement(
      "UPDATE access_pending_operations SET service_incarnation=?,"
      "route_ready=0,publish_started=0 WHERE operation_id=? AND state=0 AND "
      "operation_sequence=? AND policy_generation=?"));
  update.BindInt64(0, service_incarnation);
  update.BindString(1, operation.request.operation_id);
  update.BindInt64(2, operation.operation_sequence);
  update.BindInt64(3, operation.policy_generation);
  if (!update.Run() || database_.GetLastChangeCount() != 1) {
    return std::nullopt;
  }

  sql::Statement delete_contexts(database_.GetUniqueStatement(
      "DELETE FROM access_operation_contexts WHERE operation_id=?"));
  delete_contexts.BindString(0, operation.request.operation_id);
  if (!delete_contexts.Run()) {
    return std::nullopt;
  }
  for (const auto& context : required_contexts) {
    sql::Statement insert_context(database_.GetUniqueStatement(
        "INSERT INTO access_operation_contexts("
        "operation_id,context_id,context_incarnation,acknowledged) "
        "VALUES(?,?,?,0)"));
    insert_context.BindString(0, operation.request.operation_id);
    insert_context.BindString(1, context.context_id);
    insert_context.BindInt64(2, context.incarnation);
    if (!insert_context.Run()) {
      return std::nullopt;
    }
  }
  if (!transaction.Commit()) {
    return std::nullopt;
  }
  return LoadOperation(operation.request.operation_id, true);
}

CommitOperationStatus AccessRuleStore::CommitOperation(
    const PendingSiteOperation& expected_operation) {
  if (ShouldFail(FailurePointForTesting::kCommitOperation) || !initialized_) {
    return CommitOperationStatus::kStorageError;
  }
  sql::Transaction transaction(&database_);
  if (!transaction.Begin()) {
    return CommitOperationStatus::kStorageError;
  }
  auto loaded = LoadOperation(expected_operation.request.operation_id, true);
  if (!loaded) {
    return CommitOperationStatus::kSuperseded;
  }
  if (loaded->operation_sequence != expected_operation.operation_sequence ||
      loaded->policy_generation != expected_operation.policy_generation) {
    return CommitOperationStatus::kSuperseded;
  }
  if (!loaded->publish_started ||
      (loaded->request.enabled && !loaded->route_ready) ||
      loaded->required_contexts.empty() ||
      loaded->acknowledged_context_ids.size() !=
          loaded->required_contexts.size()) {
    return CommitOperationStatus::kNotReady;
  }

  sql::Statement delete_rules(database_.GetUniqueStatement(
      "DELETE FROM access_rules WHERE storage_partition_token=? AND "
      "site_toggle_id=?"));
  delete_rules.BindString(0, loaded->request.site.storage_partition_token);
  delete_rules.BindString(1, loaded->after_group.site_toggle_id);
  if (!delete_rules.Run()) {
    return CommitOperationStatus::kStorageError;
  }
  sql::Statement delete_group(database_.GetUniqueStatement(
      "DELETE FROM access_site_groups WHERE storage_partition_token=? AND "
      "canonical_host=?"));
  delete_group.BindString(0, loaded->request.site.storage_partition_token);
  delete_group.BindString(1, loaded->request.site.canonical_host);
  if (!delete_group.Run()) {
    return CommitOperationStatus::kStorageError;
  }

  sql::Statement insert_group(database_.GetUniqueStatement(
      "INSERT INTO access_site_groups("
      "site_toggle_id,storage_partition_token,canonical_host,"
      "http_top_level_site,https_top_level_site,http_member_rule_id,"
      "https_member_rule_id,revision,last_operation_sequence,"
      "policy_generation) VALUES(?,?,?,?,?,?,?,?,?,?)"));
  insert_group.BindString(0, loaded->after_group.site_toggle_id);
  insert_group.BindString(1, loaded->request.site.storage_partition_token);
  insert_group.BindString(2, loaded->after_group.canonical_host);
  insert_group.BindString(3, loaded->after_group.http_top_level_site);
  insert_group.BindString(4, loaded->after_group.https_top_level_site);
  insert_group.BindString(5, loaded->after_group.member_rule_ids[0]);
  insert_group.BindString(6, loaded->after_group.member_rule_ids[1]);
  insert_group.BindInt64(7, loaded->after_group.revision);
  insert_group.BindInt64(8, loaded->operation_sequence);
  insert_group.BindInt64(9, loaded->policy_generation);
  if (!insert_group.Run()) {
    return CommitOperationStatus::kStorageError;
  }

  for (const auto& member : loaded->after_members) {
    sql::Statement insert_rule(database_.GetUniqueStatement(
        "INSERT INTO access_rules("
        "rule_id,storage_partition_token,site_toggle_id,group_revision,"
        "top_level_site,exact_host,schemes_mask,port_scope,"
        "include_subdomains,mode,proxy_group_id,protection_override,"
        "row_revision,last_operation_sequence) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    insert_rule.BindString(0, member.rule_id);
    insert_rule.BindString(1, loaded->request.site.storage_partition_token);
    insert_rule.BindString(2, member.site_toggle_id);
    insert_rule.BindInt64(3, member.group_revision);
    insert_rule.BindString(4, member.top_level_site);
    insert_rule.BindString(5, member.exact_host);
    insert_rule.BindInt(6, kCompleteSchemeMask);
    insert_rule.BindInt(7, static_cast<int>(member.ports));
    insert_rule.BindBool(8, member.include_subdomains);
    insert_rule.BindInt(9, static_cast<int>(member.mode));
    insert_rule.BindString(10, member.proxy_group_id);
    insert_rule.BindInt(11, static_cast<int>(member.protection_override));
    insert_rule.BindInt64(12, member.group_revision);
    insert_rule.BindInt64(13, member.last_operation_sequence);
    if (!insert_rule.Run()) {
      return CommitOperationStatus::kStorageError;
    }
  }

  sql::Statement commit_operation(database_.GetUniqueStatement(
      "UPDATE access_pending_operations SET state=2 WHERE operation_id=? "
      "AND state=0 AND operation_sequence=? AND policy_generation=?"));
  commit_operation.BindString(0, loaded->request.operation_id);
  commit_operation.BindInt64(1, loaded->operation_sequence);
  commit_operation.BindInt64(2, loaded->policy_generation);
  if (!commit_operation.Run() || database_.GetLastChangeCount() != 1 ||
      !transaction.Commit()) {
    return CommitOperationStatus::kStorageError;
  }
  return CommitOperationStatus::kCommitted;
}

bool AccessRuleStore::SaveIndependentRule(const IndependentAccessRule& rule) {
  if (!initialized_ || !IsValidIndependentRule(rule)) {
    return false;
  }
  sql::Statement ownership(database_.GetUniqueStatement(
      "SELECT site_toggle_id FROM access_rules WHERE "
      "storage_partition_token=? AND rule_id=?"));
  ownership.BindString(0, rule.storage_partition_token);
  ownership.BindString(1, rule.rule_id);
  if (ownership.Step() && !ownership.ColumnString(0).empty()) {
    return false;
  }
  if (!ownership.Succeeded()) {
    return false;
  }
  sql::Statement statement(database_.GetUniqueStatement(
      "INSERT OR REPLACE INTO access_rules("
      "rule_id,storage_partition_token,site_toggle_id,group_revision,"
      "top_level_site,exact_host,schemes_mask,port_scope,"
      "include_subdomains,mode,proxy_group_id,protection_override,"
      "row_revision,last_operation_sequence) "
      "VALUES(?,?,'',0,?,?,?,?,?,?,?,?,?,?)"));
  statement.BindString(0, rule.rule_id);
  statement.BindString(1, rule.storage_partition_token);
  statement.BindString(2, rule.top_level_site);
  statement.BindString(3, rule.exact_host);
  statement.BindInt(4, SchemeMask(rule.schemes));
  statement.BindInt(5, static_cast<int>(rule.ports));
  statement.BindBool(6, rule.include_subdomains);
  statement.BindInt(7, static_cast<int>(rule.mode));
  statement.BindString(8, rule.proxy_group_id);
  statement.BindInt(9, static_cast<int>(rule.protection_override));
  statement.BindInt64(10, rule.row_revision);
  statement.BindInt64(11, rule.last_operation_sequence);
  return statement.Run();
}

std::optional<IndependentAccessRule> AccessRuleStore::LoadIndependentRule(
    const std::string& storage_partition_token,
    const std::string& rule_id) {
  if (!initialized_ || storage_partition_token.empty() || rule_id.empty()) {
    return std::nullopt;
  }
  sql::Statement statement(database_.GetUniqueStatement(
      "SELECT storage_partition_token,top_level_site,exact_host,schemes_mask,"
      "port_scope,include_subdomains,mode,proxy_group_id,"
      "protection_override,row_revision,last_operation_sequence FROM "
      "access_rules WHERE storage_partition_token=? AND rule_id=? AND "
      "site_toggle_id=''"));
  statement.BindString(0, storage_partition_token);
  statement.BindString(1, rule_id);
  if (!statement.Step()) {
    return std::nullopt;
  }
  IndependentAccessRule rule;
  rule.rule_id = rule_id;
  rule.storage_partition_token = statement.ColumnString(0);
  rule.top_level_site = statement.ColumnString(1);
  rule.exact_host = statement.ColumnString(2);
  rule.schemes = SchemesFromMask(statement.ColumnInt(3));
  rule.ports = static_cast<aegis_access::PortScope>(statement.ColumnInt(4));
  rule.include_subdomains = statement.ColumnBool(5);
  rule.mode = static_cast<aegis_access::AccessMode>(statement.ColumnInt(6));
  rule.proxy_group_id = statement.ColumnString(7);
  rule.protection_override =
      static_cast<aegis_access::ProtectionOverride>(statement.ColumnInt(8));
  rule.row_revision = statement.ColumnInt64(9);
  rule.last_operation_sequence = statement.ColumnInt64(10);
  return IsValidIndependentRule(rule) ? std::make_optional(std::move(rule))
                                      : std::nullopt;
}

}  // namespace aegis::access
