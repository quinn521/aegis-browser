// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_site_proxy_bubble_controller.h"
#include "chrome/browser/aegis/access/access_build_channel.h"
#include "chrome/browser/aegis/access/access_buildflags.h"

#include <algorithm>
#include <utility>
#include "base/task/thread_pool.h"
#include "base/command_line.h"
#include "base/functional/callback_helpers.h"
#include "chrome/browser/aegis/access/access_identity_generation_source.h"
#include "chrome/browser/aegis/access/access_published_request_runtime.h"
#include "chrome/browser/aegis/access/access_proxy_selection_generation_source.h"

#include "base/strings/string_number_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/aegis/access/access_network_context_transport.h"
#include "chrome/browser/aegis/access/access_service_coordinator.h"
#include "chrome/browser/aegis/aegis_profile_support.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/channel_info.h"
#include "chrome/browser/aegis/aegis_prefs.h"
#include "components/prefs/pref_service.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "chrome/browser/lifetime/termination_notification.h"
#include "chrome/browser/aegis/access/access_core_log_share.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#if BUILDFLAG(IS_MAC)
#include "chrome/browser/aegis/access/access_options_service.h"
#include "third_party/blink/public/mojom/webshare/webshare.mojom.h"
#else
#include "chrome/browser/aegis/access/access_log_export_win.h"
#endif
#endif
#include "components/aegis_access/access_proxy_route_adapter.h"
#include "components/version_info/channel.h"
#include "content/public/browser/browser_thread.h"

namespace aegis::access {
namespace {

const void* const kAccessSiteProxyBubbleControllerKey =
    &kAccessSiteProxyBubbleControllerKey;
#if BUILDFLAG(IS_MAC)
constexpr std::string_view kTestProfileGroupPrefix = "internal-test-profile:";
constexpr base::TimeDelta kTestProfilePublicationTimeout = base::Seconds(30);

const char* TestProfileSwitchCompletionMessage(
    AccessMutationTransactionStatus status) {
  switch (status) {
    case AccessMutationTransactionStatus::kNetworkPublicationFailed:
      return "The browser could not confirm the network policy for the test "
             "Profile switch.";
    case AccessMutationTransactionStatus::kCommitFailed:
      return "The test Profile switch could not be saved.";
    case AccessMutationTransactionStatus::kCommitted:
    case AccessMutationTransactionStatus::kCommittedUnavailable:
      return "The test Profile switch was saved. Its original connection "
             "could not be confirmed.";
    default:
      return "The test Profile switch failed.";
  }
}

std::string TestProfileGroup(const AccessLocalCoreReady& ready) {
  return std::string(kTestProfileGroupPrefix) + ready.profile_id + ":" +
         base::NumberToString(ready.profile_version);
}

std::optional<std::pair<std::string_view, uint64_t>> ParseTestProfileGroup(
    std::string_view group) {
  if (!group.starts_with(kTestProfileGroupPrefix)) return std::nullopt;
  group.remove_prefix(kTestProfileGroupPrefix.size());
  const auto separator = group.find(':');
  if (separator == std::string_view::npos) return std::nullopt;
  const auto id = group.substr(0, separator);
  const auto version = ParseAccessOptionsRevision(group.substr(separator + 1));
  if (!base::Uuid::ParseLowercase(id).is_valid() || !version || *version == 0) {
    return std::nullopt;
  }
  return std::pair(id, *version);
}

const StoredSiteGroup* FindCommittedBubbleSite(
    const StoredPolicySnapshot& committed,
    const AccessSiteHost& host,
    bool website_context) {
  const std::string toggle_id =
      (website_context ? "bubble-site:" : "bubble:") + host.value;
  const auto site =
      std::ranges::find_if(committed.site_groups, [&](const auto& row) {
        return row.group.owner == committed.owner &&
               row.group.website_context == website_context &&
               row.group.canonical_host == host.value &&
               row.group.site_toggle_id == toggle_id;
      });
  return site == committed.site_groups.end() ? nullptr : &*site;
}

const StoredSiteGroup* FindCommittedBubbleSite(
    const StoredPolicySnapshot& committed,
    const AccessSiteHost& host) {
  if (const auto* site = FindCommittedBubbleSite(committed, host, true)) {
    return site;
  }
  return FindCommittedBubbleSite(committed, host, false);
}

std::optional<aegis_access::GenerationTuple> CaptureCommittedPolicy(
    Profile* profile,
    const StoredPolicySnapshot& committed) {
  auto* runtime = AccessPublishedRequestRuntime::Get(profile);
  if (!runtime) return std::nullopt;
  const auto common =
      runtime->CaptureCommonGenerationTupleOnUiThread(committed.owner);
  const auto* published = runtime->GetPublishedPolicySnapshot(committed.owner);
  if (!common || !published || published->owner != committed.owner ||
      published->policy_generation != common->policy_generation ||
      published->policy_generation < committed.policy_generation) {
    return std::nullopt;
  }
  // Rollback can publish the committed rules at a newer runtime generation.
  const auto adapted = AccessRuleStore::AdaptMatcherSnapshot(committed);
  if (adapted.status != StoreStatus::kValid || !adapted.value ||
      published->rules != adapted.value->rules) {
    return std::nullopt;
  }
  return common;
}
#endif

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
const void* const kLogHistoryKey = &kLogHistoryKey;
// Keep both stores on the original profile. Reopening an incognito window
// reuses its own private store even while the previous core is shutting down.
class ProfileLogHistories : public base::SupportsUserData::Data {
 public:
  scoped_refptr<AccessCoreLogStore> regular;
  scoped_refptr<AccessCoreLogStore> private_history;
};
scoped_refptr<AccessCoreLogStore> GetCoreLogHistory(Profile* profile) {
  Profile* original = profile->GetOriginalProfile();
  auto* histories = static_cast<ProfileLogHistories*>(original->GetUserData(kLogHistoryKey));
  if (!histories) {
    auto owned = std::make_unique<ProfileLogHistories>();
    histories = owned.get();
    original->SetUserData(kLogHistoryKey, std::move(owned));
  }
  const bool is_private = profile->IsOffTheRecord();
  auto& target = is_private ? histories->private_history : histories->regular;
  if (!target) {
    const auto level = original->GetPrefs()->GetString(
        is_private ? kAccessPrivateCoreLogLevel : kAccessCoreLogLevel);
    target = AccessCoreLogStore::Open(original->GetPath(),
        is_private ? CoreLogProfile::kPrivate : CoreLogProfile::kRegular, level);
  }
  return target;
}
#endif

bool IsNumericLoopbackHost(std::string_view host) {
  return host == "127.0.0.1" || host == "::1" || host == "[::1]";
}

AccessSiteProxySessionError MutationStatusToSessionError(
    AccessMutationTransactionStatus status) {
  switch (status) {
    case AccessMutationTransactionStatus::kCommitted:
      return AccessSiteProxySessionError::kOk;
    case AccessMutationTransactionStatus::kCommittedUnavailable:
      return AccessSiteProxySessionError::kRuntimeFault;
    case AccessMutationTransactionStatus::kBusy:
      return AccessSiteProxySessionError::kBusy;
    case AccessMutationTransactionStatus::kInvalidRequest:
    case AccessMutationTransactionStatus::kUnsupportedTransportScope:
      return AccessSiteProxySessionError::kMutationFailed;
    case AccessMutationTransactionStatus::kStoreReadFailed:
    case AccessMutationTransactionStatus::kPrepareFailed:
    case AccessMutationTransactionStatus::kCandidateBuildFailed:
      return AccessSiteProxySessionError::kPrepareFailed;
    case AccessMutationTransactionStatus::kMissingRuntime:
    case AccessMutationTransactionStatus::kMissingDispatchState:
    case AccessMutationTransactionStatus::kMissingTransport:
      return AccessSiteProxySessionError::kServiceUnavailable;
    case AccessMutationTransactionStatus::kProxySelectionUnavailable:
      return AccessSiteProxySessionError::kLocalProxyNotReady;
    case AccessMutationTransactionStatus::kPublicationTrackerRejected:
    case AccessMutationTransactionStatus::kRuntimePublicationFailed:
      return AccessSiteProxySessionError::kPublishFailed;
    case AccessMutationTransactionStatus::kNetworkPublicationFailed:
      return AccessSiteProxySessionError::kAckFailed;
    case AccessMutationTransactionStatus::kCommitFailed:
      return AccessSiteProxySessionError::kMutationFailed;
  }
  return AccessSiteProxySessionError::kMutationFailed;
}

std::string MutationStatusReadableMessage(
    AccessMutationTransactionStatus status) {
  return AccessSiteProxySessionErrorReadableMessage(
      MutationStatusToSessionError(status),
      base::NumberToString(static_cast<int>(status)));
}

}  // namespace

AccessSiteProxyBubbleController::AccessSiteProxyBubbleController(
    Profile* profile)
    : profile_(profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  if ((profile_->IsRegularProfile() || profile_->IsPrimaryOTRProfile()) &&
      profile_->GetPrefs()->FindPreference(kAccessCoreLogLevel)) {
    app_terminating_ = browser_shutdown::AddAppTerminatingCallback(
        base::BindOnce(&CoreLogShareSession::ShutDown, base::Unretained(&log_share_)));
    core_logs_ = GetCoreLogHistory(profile_);
  }
#endif
}

AccessSiteProxyBubbleController::~AccessSiteProxyBubbleController() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

AccessSiteProxyBubbleController* AccessSiteProxyBubbleController::Get(
    Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile) {
    return nullptr;
  }
  return static_cast<AccessSiteProxyBubbleController*>(
      profile->GetUserData(kAccessSiteProxyBubbleControllerKey));
}

AccessSiteProxyBubbleController* AccessSiteProxyBubbleController::GetOrCreate(
    Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile || !aegis::IsAegisProfileSupported(profile)) {
    return nullptr;
  }
  if (AccessSiteProxyBubbleController* existing = Get(profile)) {
    return existing;
  }
  auto controller = std::unique_ptr<AccessSiteProxyBubbleController>(
      new AccessSiteProxyBubbleController(profile));
  AccessSiteProxyBubbleController* raw = controller.get();
  profile->SetUserData(kAccessSiteProxyBubbleControllerKey,
                       std::move(controller));
  return raw;
}

AccessSiteProxyBubbleSnapshot
AccessSiteProxyBubbleController::SnapshotForCommittedUrl(
    const GURL& committed_url) const {
  const auto host = ResolveTrustedNavigationHost(committed_url);
  const auto website = host.host ? CanonicalWebsiteHost(*host.host).host : std::nullopt;
  const bool website_context = website && website_hosts_.contains(*website);
  const auto key = website_context ? website : host.host;
  auto found = key ? host_states_.find(*key) : host_states_.end();
  if (publication_in_flight_ && website) {
    const auto pending = host_states_.find(*website);
    if (pending != host_states_.end() &&
        pending->second.connection == AccessSiteConnectionState::kConnecting) found = pending;
  }
  HostState state = found == host_states_.end() ? HostState() : found->second;
  const auto state_host =
      found == host_states_.end() ? key : std::make_optional(found->first);
  if (busy_toggle_notice_ && busy_toggle_notice_->revision == state_revision_ &&
      state_host == busy_toggle_notice_->host && !state.runtime_fault) {
    state.failure_reason = AccessSiteProxySessionErrorReadableMessage(
        AccessSiteProxySessionError::kBusy);
  }
  bool verify_legacy_core = state.connection == AccessSiteConnectionState::kReady;
#if BUILDFLAG(IS_MAC)
  const bool allowed = key && intent_store_.Get(*key) == AccessSiteIntent::kAllow;
  const auto manual = key ? test_profile_sites_.find(*key) : test_profile_sites_.end();
  if (manual != test_profile_sites_.end() && allowed &&
      !(publication_in_flight_ && state.connection == AccessSiteConnectionState::kConnecting)) {
    verify_legacy_core = false;
    auto* transport = AccessNetworkContextTransport::Get(profile_);
    const auto owner = transport
        ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), {})
        : std::nullopt;
    const auto common = owner ? CaptureTestProfilePolicy(*owner, manual->second)
                              : std::nullopt;
    if (common && IsActiveTestProfileGroup(*owner, manual->second, *common)) {
      state.connection = AccessSiteConnectionState::kReady;
      state.failure_reason.clear();
    } else if (common && saved_test_profile_restore_ &&
               saved_test_profile_restore_->owner == *owner &&
               saved_test_profile_restore_->group == manual->second &&
               saved_test_profile_restore_->common == *common) {
      state.connection = AccessSiteConnectionState::kConnecting;
      state.failure_reason.clear();
    } else if (state.connection != AccessSiteConnectionState::kUnavailable) {
      state.connection = AccessSiteConnectionState::kUnavailable;
      state.failure_reason = state.operation_failure_reason.empty()
                                 ? "The selected test Profile is unavailable."
                                 : "";
    }
  }
  const bool recheck_legacy = allowed && manual == test_profile_sites_.end() &&
      state.recheck_committed_legacy &&
      state.connection == AccessSiteConnectionState::kUnavailable &&
      !publication_in_flight_ && !restoring_core_;
  if (verify_legacy_core || recheck_legacy) {
    if (allowed && HasAcknowledgedCommittedLegacySite(*key, website_context)) {
      if (recheck_legacy) {
        state.connection = AccessSiteConnectionState::kReady;
        state.failure_reason.clear();
      }
    } else if (verify_legacy_core) {
      state.connection = AccessSiteConnectionState::kUnavailable;
      if (state.failure_reason.empty()) {
        state.failure_reason =
            "Proxy connection is unavailable. Reload to reconnect.";
      }
    }
  }
#else
  if (verify_legacy_core) {
    auto* transport = AccessNetworkContextTransport::Get(profile_);
    const auto owner = transport ? transport->OwnerForPartition(
        AccessChannelForCurrentBuild(), {}) : std::nullopt;
    auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
    const auto common = owner && runtime
        ? runtime->CaptureCommonGenerationTupleOnUiThread(*owner) : std::nullopt;
    if (!local_core_ready_ || !common || !transport->HasCurrentPrivatePolicy(*owner, *common)) {
      state.connection = AccessSiteConnectionState::kUnavailable;
      state.failure_reason = "Proxy connection is unavailable. Reload to reconnect.";
    }
  }
#endif
  AccessSiteIntentStore presentation;
  if (host.host && key) presentation.Set(*host.host, intent_store_.Get(*key));
  return BuildSiteProxyBubbleSnapshot(
      committed_url, presentation, state.connection,
      state.failure_reason.empty() ? state.operation_failure_reason
                                   : state.failure_reason);
}

bool AccessSiteProxyBubbleController::ApplyHostProxyToggle(
    const AccessSiteHost& host,
    bool enabled) {
  ++state_revision_;
  return ApplySiteProxyToggle(&intent_store_, host, enabled);
}

bool AccessSiteProxyBubbleController::IsDevAllowBlockPanelRegistered() const {
  const auto identity = AccessBuildIdentityForCurrentBuild();
  if (identity.conflict ||
      (identity.channel != aegis_access::ChannelNamespace::kDev &&
       identity.channel != aegis_access::ChannelNamespace::kAlpha))
    return false;
#if !BUILDFLAG(AEGIS_DEV_LOCAL_NODE_ALLOWED)
  if (identity.product == aegis_access::AccessProductChannel::kUnknown &&
      chrome::GetChannel() == version_info::Channel::UNKNOWN)
    return false;
#endif
  return IsAccessDevAllowBlockPanelAdmitted(chrome::GetChannel()) &&
         !IsGeneralAccessRuleEditorRegistered(chrome::GetChannel());
}

AccessDevAllowBlockPanelSnapshot
AccessSiteProxyBubbleController::DevAllowBlockSnapshotForCommittedUrl(
    const GURL& committed_url) const {
  if (!IsDevAllowBlockPanelRegistered())
    return {};
  return BuildAccessDevAllowBlockPanelSnapshot(chrome::GetChannel(),
                                               dev_intent_store_, committed_url);
}

AccessDevAllowBlockApplyResult
AccessSiteProxyBubbleController::ApplyDevAllowBlock(
    std::string_view raw_host,
    AccessDevAllowBlockAction action) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!IsDevAllowBlockPanelRegistered())
    return {.status = AccessDevAllowBlockApplyStatus::kChannelNotAdmitted};
  AccessDevAllowBlockApplyResult result = ApplyAccessDevAllowBlock(
      &dev_intent_store_, chrome::GetChannel(), raw_host, action);
  if (result.status != AccessDevAllowBlockApplyStatus::kApplied) {
    return result;
  }
  ++state_revision_;
  return result;
}

bool AccessSiteProxyBubbleController::RequestHostProxySession(
    const AccessSiteHost& host, bool enabled) {
  return RequestHostProxySession(host, enabled, {});
}

bool AccessSiteProxyBubbleController::RequestHostProxySession(
    const AccessSiteHost& requested_host,
    bool enabled, base::OnceClosure on_ready) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto website = CanonicalWebsiteHost(requested_host);
  if (!website.host) return false;
  const auto host = *website.host;
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  if (enabled) {
    auto* transport = AccessNetworkContextTransport::Get(profile_);
    if (transport && std::ranges::any_of(transport->AccountSites(), [&](const auto& paid) {
          return CanonicalWebsiteHost(AccessSiteHost{paid}).host == host;
        })) return false;
  }
#endif
  ++state_revision_;
  if (MakeAccessSiteHost(host.value).status != AccessSiteHostStatus::kOk) {
    host_states_[host].failure_reason = AccessSiteProxySessionErrorReadableMessage(
        AccessSiteProxySessionError::kInvalidHost);
    host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
    return false;
  }
  base::OnceCallback<void(bool)> superseded_retry;
  base::OnceCallback<void(bool)> superseded_queue;
  if (publication_in_flight_ && !enabled && verifying_host_ == host) {
    // The ON transaction is already committed. Its connectivity recheck may
    // finish later, but a newer OFF owns both the choice and the retry now.
    verifying_host_.reset();
    ++committed_verification_attempt_;
    publication_in_flight_ = false;
    on_enabled_.Reset();
    superseded_retry = std::move(retry_completion_);
    superseded_queue = std::move(queued_retry_);
    retry_url_ = GURL();
  }
  if (publication_in_flight_ || (enabled && restoring_core_)) {
    busy_toggle_notice_ = BusyToggleNotice{host, state_revision_};
    return false;
  }

  if (!enabled) {
    // DIRECT publication does not need a live proxy endpoint. Keep the
    // committed choice until the local transaction actually succeeds.
    host_states_[host] = {AccessSiteConnectionState::kConnecting, {}};
    BeginDisablePublication(host, std::move(on_ready));
    if (superseded_retry) std::move(superseded_retry).Run(false);
    if (superseded_queue) std::move(superseded_queue).Run(false);
    return true;
  }

  host_states_[host] = {AccessSiteConnectionState::kConnecting, {}};

  publication_in_flight_ = true;
  on_enabled_ = std::move(on_ready);
  PrepareLocalCore(host);
  return true;
}

void AccessSiteProxyBubbleController::RequestFailedNavigationProxy(
    const GURL& original_url, base::OnceCallback<void(bool)> completion) {
  const auto host = ResolveTrustedNavigationHost(original_url);
  // Stop revokes the document callback immediately. The old transaction may
  // still own the publisher while the user clicks Retry on that page again.
  if (publication_in_flight_ && retry_url_ == original_url &&
      retry_completion_ && retry_completion_.IsCancelled() &&
      (!queued_retry_ || queued_retry_.IsCancelled())) {
    queued_retry_ = std::move(completion);
    return;
  }
  if (!host.host || publication_in_flight_) {
    std::move(completion).Run(false);
    return;
  }
#if BUILDFLAG(IS_MAC)
  if (!ResolveAccessLocalCoreInput()) {
    // Without the legacy node input, only a site already bound to a manual
    // Test Profile may retry. Its exact route is checked after restoration.
    const auto website = CanonicalWebsiteHost(*host.host);
    if (!website.host || (!test_profile_sites_.contains(*website.host) &&
                          !HasCommittedTestProfileSite(&*website.host))) {
      std::move(completion).Run(false);
      return;
    }
  }
#endif
  retry_url_ = original_url;
  retry_completion_ = std::move(completion);
  if (!RequestHostProxySession(*host.host, true) && retry_completion_) {
    retry_url_ = GURL();
    std::move(retry_completion_).Run(false);
  }
}
base::RepeatingCallback<void(const GURL&, base::OnceCallback<void(bool)>)>
AccessSiteProxyBubbleController::GetFailedNavigationRetryHandler() {
  bool admitted = ResolveAccessLocalCoreInput().has_value();
#if BUILDFLAG(IS_MAC)
  admitted = admitted || HasManualTestProfileRetryBinding();
#endif
  if (!admitted) return {};
  return base::BindRepeating(
      &AccessSiteProxyBubbleController::RequestFailedNavigationProxy,
      weak_factory_.GetWeakPtr());
}
void AccessSiteProxyBubbleController::EnsureProfileRestored(const OwnershipKey& owner) {
  if (auto* coordinator = AccessServiceCoordinator::GetOrCreate(profile_))
    coordinator->InitializeForProfile(owner, base::BindOnce(
        &AccessSiteProxyBubbleController::RestoreSavedIntents,
        weak_factory_.GetWeakPtr(), owner));
}
void AccessSiteProxyBubbleController::RestoreSavedIntents(OwnershipKey owner,
                                                         bool restored) {
  if (!restored) return;
  const auto snapshot = AccessServiceCoordinator::Get(profile_)->ReadCommittedSnapshot(
      owner.storage_partition_token);
  if (!snapshot.value) return;
  for (const auto& group : snapshot.value->site_groups) {
    if (group.members.empty() ||
        group.group.site_toggle_id !=
            (group.group.website_context ? "bubble-site:" : "bubble:") +
                group.group.canonical_host) continue;
    const auto host = MakeAccessSiteHost(group.group.canonical_host);
    if (host.host) {
      intent_store_.Set(*host.host,
          group.members.front().policy.mode == AccessMode::kProxy
              ? AccessSiteIntent::kAllow : AccessSiteIntent::kOff);
      host_states_.try_emplace(*host.host);
      if (group.group.website_context) website_hosts_.insert(*host.host);
#if BUILDFLAG(IS_MAC)
      const auto& policy = group.members.front().policy;
      if (policy.mode == AccessMode::kProxy &&
          IsTestProfileProxyGroup(policy.proxy_group_id)) {
        test_profile_sites_.insert_or_assign(*host.host, policy.proxy_group_id);
      } else {
        test_profile_sites_.erase(*host.host);
      }
#endif
    }
  }
  ++state_revision_;
}
void AccessSiteProxyBubbleController::AdoptWebsiteIntent(
    const AccessSiteHost& host, bool enabled) {
  std::vector<AccessSiteHost> aliases;
  for (const auto& [key, intent] : intent_store_.All()) {
    if (key != host && CanonicalWebsiteHost(key).host == host) aliases.push_back(key);
  }
  for (const auto& alias : aliases) {
    intent_store_.Set(alias, AccessSiteIntent::kOff);
    host_states_.erase(alias);
  }
  website_hosts_.insert(host);
  ApplySiteProxyToggle(&intent_store_, host, enabled);
}
void AccessSiteProxyBubbleController::FailCorePreparation(
    const AccessSiteHost& host, std::string error) {
  publication_in_flight_ = false;
  local_core_ready_ = false;
  ++state_revision_;
  auto& state = host_states_[host];
  if (!state.runtime_fault) {
    state.connection = AccessSiteConnectionState::kUnavailable;
    state.failure_reason = std::move(error);
  }
  FinishNavigationPreparation(false);
}
void AccessSiteProxyBubbleController::PrepareLocalCore(const AccessSiteHost& host) {
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), base::FilePath())
      : std::nullopt;
  if (!owner || !transport->OwnsConfiguredPartition(*owner) ||
      (!IsAccessDevAllowBlockPanelAdmitted(chrome::GetChannel()) ||
       IsGeneralAccessRuleEditorRegistered(chrome::GetChannel()))) {
    FailCorePreparation(host, "The test proxy is unavailable in this build.");
    return;
  }
  auto* coordinator = AccessServiceCoordinator::GetOrCreate(profile_);
  if (!coordinator) {
    FailCorePreparation(host, "Access rule storage is unavailable.");
    return;
  }
  coordinator->InitializeForProfile(*owner, base::BindOnce(
      &AccessSiteProxyBubbleController::OnProfileRestoredForCore,
      weak_factory_.GetWeakPtr(), host, *owner));
}
void AccessSiteProxyBubbleController::OnProfileRestoredForCore(
    AccessSiteHost host, OwnershipKey owner, bool restored) {
  if (!restored || (retry_completion_ && retry_completion_.IsCancelled())) {
    FailCorePreparation(host, "Access startup restoration was not completed.");
    return;
  }
  RestoreSavedIntents(owner, true);
#if BUILDFLAG(IS_MAC)
  const auto test_profile = test_profile_sites_.find(host);
  if (test_profile != test_profile_sites_.end()) {
    const auto common = CaptureTestProfilePolicy(owner, test_profile->second);
    if (!common || !IsActiveTestProfileGroup(owner, test_profile->second, *common)) {
      publication_in_flight_ = false;
      ++state_revision_;
      host_states_[host] = {AccessSiteConnectionState::kUnavailable,
                            "The selected test Profile is unavailable."};
      FinishNavigationPreparation(false);
      return;
    }
    publication_in_flight_ = false;
    ++state_revision_;
    host_states_[host] = {AccessSiteConnectionState::kReady, {}};
    FinishNavigationPreparation(true);
    return;
  }
#endif
  CreateLocalCore();
  if (!local_core_) {
    FailCorePreparation(host, "The authorized proxy configuration is unavailable.");
    return;
  }
  local_core_->Prepare(base::BindOnce(
      &AccessSiteProxyBubbleController::OnLocalCorePrepared,
      weak_factory_.GetWeakPtr(), host, owner));
}
void AccessSiteProxyBubbleController::OnLocalCorePrepared(
    AccessSiteHost host, OwnershipKey owner, AccessLocalCoreReady result) {
  if (retry_completion_ && retry_completion_.IsCancelled()) {
    FailCorePreparation(host, "The failed navigation is no longer current.");
    return;
  }
  if (!result.ready) {
    FailCorePreparation(host, std::move(result.error));
    return;
  }
  if (!RegisterPreparedCore(owner, result)) {
    FailCorePreparation(host, "The prepared proxy registration could not be installed.");
    return;
  }
  AccessSiteProxyLocalProxyReadyInfo ready;
  if (EnsureLocalProxyReady(host, &ready) != AccessSiteProxySessionError::kOk) {
    FailCorePreparation(host, "The prepared proxy has not been registered with this Profile.");
    return;
  }
  BeginEnablePublication(host, std::move(ready));
}
std::string AccessSiteProxyBubbleController::LocalProxyGroup(
    const OwnershipKey& owner) {
  if (owner.channel == ChannelNamespace::kBeta) return "beta-authorized-test-node";
  return owner.channel == ChannelNamespace::kAlpha ? "alpha-authorized-test-node"
                                                  : "dev-authorized-local-node";
}
#if BUILDFLAG(IS_MAC)
bool AccessSiteProxyBubbleController::IsTestProfileProxyGroup(
    std::string_view group) {
  return ParseTestProfileGroup(group).has_value();
}

bool AccessSiteProxyBubbleController::HasPreparedTestNode() const {
  if (profile_->IsOffTheRecord() || publication_in_flight_ || restoring_core_ ||
      saved_test_profile_restore_)
    return false;
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  return options && options->PreparedTestNode().has_value();
}

bool AccessSiteProxyBubbleController::HasManualTestProfileRetryBinding() const {
  // The handler also suppresses Chromium auto-reload, so admit it only when a
  // manual binding actually exists, never for every DEV error page.
  if (profile_->IsOffTheRecord() || !CanResolveAccessInternalTestCoreInput())
    return false;
  if (!test_profile_sites_.empty()) return true;
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  if (options && options->test_profile_runtime().ready) return true;
  return HasCommittedTestProfileSite(nullptr);
}

bool AccessSiteProxyBubbleController::HasCommittedTestProfileSite(
    const AccessSiteHost* host) const {
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), {})
      : std::nullopt;
  auto* coordinator = owner ? AccessServiceCoordinator::Get(profile_) : nullptr;
  if (!coordinator) return false;
  const auto snapshot = coordinator->ReadCommittedSnapshot(owner->storage_partition_token);
  if (!snapshot.value || snapshot.value->owner != *owner) return false;
  const auto selects_test_profile = [](const StoredSiteGroup& site) {
    return !site.members.empty() &&
           site.members.front().policy.mode == AccessMode::kProxy &&
           IsTestProfileProxyGroup(site.members.front().policy.proxy_group_id);
  };
  if (host) {
    const auto* site = FindCommittedBubbleSite(*snapshot.value, *host);
    return site && selects_test_profile(*site);
  }
  return std::ranges::any_of(snapshot.value->site_groups, selects_test_profile);
}

std::optional<AccessLocalCoreReady>
AccessSiteProxyBubbleController::AcknowledgedTestProfileListener() const {
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  if (!options) return std::nullopt;
  auto active = options->test_profile_runtime();
  if (!active.ready) return std::nullopt;
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), {})
      : std::nullopt;
  const auto group = TestProfileGroup(active);
  const auto common = owner ? CaptureTestProfilePolicy(*owner, group) : std::nullopt;
  if (!common || !transport->HasAcknowledgedTestListener(
                     *owner, *common, group, active.registration_id)) {
    return std::nullopt;
  }
  return active;
}

bool AccessSiteProxyBubbleController::CanSelectTestProfileForSite(
    const AccessSiteHost& requested_host) const {
  const auto website = CanonicalWebsiteHost(requested_host);
  if (!website.host) return false;
  if (HasPreparedTestNode()) return true;
  if (profile_->IsOffTheRecord() || publication_in_flight_ || restoring_core_ ||
      saved_test_profile_restore_)
    return false;
  const auto active = AcknowledgedTestProfileListener();
  if (!active) return false;
  const auto bound = test_profile_sites_.find(*website.host);
  return bound == test_profile_sites_.end() ||
         bound->second != TestProfileGroup(*active);
}

bool AccessSiteProxyBubbleController::RequestTestProfileForSite(
    const AccessSiteHost& requested_host) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto website = CanonicalWebsiteHost(requested_host);
  if (!website.host || !CanSelectTestProfileForSite(requested_host)) return false;
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  const auto prepared = options ? options->PreparedTestNode() : std::nullopt;
  const auto active = prepared ? std::nullopt : AcknowledgedTestProfileListener();
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), {})
      : std::nullopt;
  auto* coordinator = AccessServiceCoordinator::GetOrCreate(profile_);
  if ((!prepared && !active) || !owner || !transport->OwnsConfiguredPartition(*owner) ||
      !coordinator ||
      std::ranges::any_of(transport->AccountSites(), [&](const auto& paid) {
        return CanonicalWebsiteHost(AccessSiteHost{paid}).host == website.host;
      })) {
    return false;
  }
  // Bind a retained fault to the original durable route at admission.
  std::optional<bool> original_legacy_website_context;
  const auto original =
      coordinator->ReadCommittedSnapshot(owner->storage_partition_token);
  if (original.status == StoreStatus::kValid && original.value &&
      original.value->owner == *owner) {
    const auto* site = FindCommittedBubbleSite(*original.value, *website.host);
    if (site && !site->members.empty()) {
      const auto& rule = site->members.front().policy;
      if (rule.owner == *owner && rule.mode == AccessMode::kProxy &&
          rule.proxy_group_id == LocalProxyGroup(*owner)) {
        original_legacy_website_context = site->group.website_context;
      }
    }
  }
  publication_in_flight_ = true;
  ++state_revision_;
  auto& state = host_states_[*website.host];
  state = {.connection = AccessSiteConnectionState::kConnecting,
           .failure_reason = {},
           .runtime_fault = original_legacy_website_context
                                ? state.runtime_fault
                                : std::nullopt};
  if (!prepared) {
    coordinator->InitializeForProfile(*owner, base::BindOnce(
        &AccessSiteProxyBubbleController::OnActiveTestProfileSiteRestored,
        weak_factory_.GetWeakPtr(), *website.host, *owner, active->registration_id,
        TestProfileGroup(*active), original_legacy_website_context));
    return true;
  }
  coordinator->InitializeForProfile(*owner, base::BindOnce(
      &AccessSiteProxyBubbleController::OnTestProfileSiteRestored,
      weak_factory_.GetWeakPtr(), *website.host, *owner, prepared->first,
      prepared->second, original_legacy_website_context));
  return true;
}

void AccessSiteProxyBubbleController::OnActiveTestProfileSiteRestored(
    AccessSiteHost host, OwnershipKey owner, std::string registration_id,
    std::string group, std::optional<bool> original_legacy_website_context,
    bool restored) {
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  const auto common = restored ? CaptureTestProfilePolicy(owner, group) : std::nullopt;
  if (!options || !common || !IsActiveTestProfileGroup(owner, group, *common) ||
      options->test_profile_runtime().registration_id != registration_id) {
    publication_in_flight_ = false;
    ++state_revision_;
    ReconcileCommittedSiteAfterTestPublication(
        host, owner, original_legacy_website_context,
        "The test Profile switch failed because the active listener changed.");
    return;
  }
  BeginEnablePublication(host, {owner, group}, base::BindOnce(
      [](base::WeakPtr<AccessSiteProxyBubbleController> self, AccessSiteHost host,
         OwnershipKey owner, std::string registration_id, std::string group,
         std::optional<bool> original_legacy_website_context,
         AccessMutationTransactionResult result) {
        if (!self) return;
        self->publication_in_flight_ = false;
        ++self->state_revision_;
        const bool committed =
            result.status == AccessMutationTransactionStatus::kCommitted;
        auto* options = AccessOptionsService::GetOrCreate(self->profile_);
        const auto common = self->CaptureTestProfilePolicy(owner, group);
        const bool acknowledged = committed && options && common &&
            options->test_profile_runtime().registration_id == registration_id &&
            self->IsActiveTestProfileGroup(owner, group, *common);
        if (acknowledged) {
          self->AdoptWebsiteIntent(host, true);
          self->test_profile_sites_.insert_or_assign(host, group);
          self->host_states_[host] = {.connection = AccessSiteConnectionState::kReady};
          return;
        }
        // Never revoke here: other sites keep using the same listener.
        self->ReconcileCommittedSiteAfterTestPublication(
            host, owner, original_legacy_website_context,
            TestProfileSwitchCompletionMessage(result.status));
      }, weak_factory_.GetWeakPtr(), host, owner, std::move(registration_id), group,
      original_legacy_website_context));
}

void AccessSiteProxyBubbleController::OnTestProfileSiteRestored(
    AccessSiteHost host, OwnershipKey owner, uint64_t revision,
    AccessLocalCoreReady ready,
    std::optional<bool> original_legacy_website_context, bool restored) {
  auto fail = [&](std::string message) {
    publication_in_flight_ = false;
    ++state_revision_;
    ReconcileCommittedSiteAfterTestPublication(
        host, owner, original_legacy_website_context, std::move(message));
  };
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  auto* coordinator = AccessServiceCoordinator::Get(profile_);
  const auto common = runtime
      ? runtime->CaptureCommonGenerationTupleOnUiThread(owner)
      : std::nullopt;
  if (!restored || !options || !transport || !coordinator || !common ||
      !coordinator->CanDispatch(owner) ||
      !transport->HasCurrentPrivatePolicy(owner, *common)) {
    fail("The test Profile switch failed because the website policy could not "
         "be confirmed.");
    return;
  }
  const auto prepared = options->PreparedTestNode();
  if (!prepared || prepared->first != revision ||
      prepared->second.registration_id != ready.registration_id) {
    fail("The test Profile switch failed because the prepared listener changed.");
    return;
  }
  auto attempt = base::MakeRefCounted<TestProfilePublicationAttempt>();
  if (!RegisterTestProfileForPublication(*options, owner, *common, revision,
                                         ready, attempt)) {
    fail("The test Profile switch failed because the listener could not be "
         "registered.");
    return;
  }

  BeginEnablePublication(host, {owner, TestProfileGroup(ready)}, base::BindOnce(
      [](base::WeakPtr<AccessSiteProxyBubbleController> self,
         base::WeakPtr<AccessOptionsService> options, AccessSiteHost host,
         OwnershipKey owner, uint64_t revision, AccessLocalCoreReady ready,
         std::optional<bool> original_legacy_website_context,
         scoped_refptr<TestProfilePublicationAttempt> attempt,
         AccessMutationTransactionResult result) {
        if (!self) return;
        const auto status = result.status;
        const bool committed =
            status == AccessMutationTransactionStatus::kCommitted;
        self->publication_in_flight_ = false;
        ++self->state_revision_;
        const bool matched = options &&
            options->CompleteTestNodePublication(revision, ready, committed);
        if (!matched) {
          // The transaction belongs to this site even if its runtime is gone.
          // Only the exact original registration may be completed or revoked.
          self->ReconcileCommittedSiteAfterTestPublication(
              host, owner, original_legacy_website_context,
              attempt->explicitly_revoked()
                  ? ""
                  : TestProfileSwitchCompletionMessage(status));
          return;
        }
        if (committed ||
            status == AccessMutationTransactionStatus::kCommittedUnavailable) {
          self->AdoptWebsiteIntent(host, true);
          self->test_profile_sites_.insert_or_assign(host, TestProfileGroup(ready));
          self->host_states_[host] = {
              .connection = committed ? AccessSiteConnectionState::kReady
                                      : AccessSiteConnectionState::kUnavailable,
              .operation_failure_reason =
                  committed ? "" : TestProfileSwitchCompletionMessage(status)};
        } else {
          self->ReconcileCommittedSiteAfterTestPublication(
              host, owner, original_legacy_website_context,
              TestProfileSwitchCompletionMessage(status));
        }
        if (!committed) options->RevokeTestNodeProfile(base::DoNothing());
      }, weak_factory_.GetWeakPtr(), options->GetWeakPtr(), host, owner,
      revision, ready, original_legacy_website_context, std::move(attempt)));
}

void AccessSiteProxyBubbleController::ReconcileCommittedSiteAfterTestPublication(
    const AccessSiteHost& host,
    const OwnershipKey& owner,
    std::optional<bool> original_legacy_website_context,
    std::string failure_reason) {
  auto* coordinator = AccessServiceCoordinator::Get(profile_);
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  auto& state = host_states_[host];
  const auto committed =
      coordinator && transport && transport->OwnsConfiguredPartition(owner)
          ? coordinator->ReadCommittedSnapshot(owner.storage_partition_token)
          : StoreResult<StoredPolicySnapshot>{.status = StoreStatus::kMissing};
  if (committed.status != StoreStatus::kValid || !committed.value ||
      committed.value->owner != owner) {
    if (failure_reason.empty() &&
        intent_store_.Get(host) == AccessSiteIntent::kAllow) {
      failure_reason = "The current website policy is unavailable.";
    }
    state = failure_reason.empty()
        ? HostState{}
        : HostState{AccessSiteConnectionState::kUnavailable,
                    std::move(failure_reason)};
    return;
  }
  const auto* site = FindCommittedBubbleSite(*committed.value, host);
  if (site && site->group.website_context) {
    website_hosts_.insert(host);
  } else {
    website_hosts_.erase(host);
  }
  const auto* rule = site && !site->members.empty()
      ? &site->members.front().policy : nullptr;
  const bool allowed = rule && rule->owner == owner && rule->mode == AccessMode::kProxy;
  intent_store_.Set(host, allowed ? AccessSiteIntent::kAllow : AccessSiteIntent::kOff);
  test_profile_sites_.erase(host);
  if (!allowed) {
    state = failure_reason.empty()
        ? HostState{}
        : HostState{AccessSiteConnectionState::kUnavailable,
                    std::move(failure_reason)};
    return;
  }

  const auto& group = rule->proxy_group_id;
  const bool test_profile = IsTestProfileProxyGroup(group);
  if (test_profile) test_profile_sites_.insert_or_assign(host, group);
  const bool legacy = group == LocalProxyGroup(owner);
  if (legacy && state.runtime_fault &&
      original_legacy_website_context == site->group.website_context) {
    // The original route still owns the fault. An install ACK only confirms
    // the policy, so it cannot clear the host fault.
    state = HostState::FromRuntimeFault(*state.runtime_fault);
    return;
  }
  const auto common = coordinator->CanDispatch(owner)
      ? CaptureCommittedPolicy(profile_, *committed.value) : std::nullopt;
  const bool acknowledged = common &&
      (test_profile
           ? IsActiveTestProfileGroup(owner, group, *common)
           : legacy && local_core_ready_ &&
                 core_owner_ == owner && transport->HasAcknowledgedLegacyProxy(
                     owner, *common, group, core_registration_));
  state = {.connection = acknowledged ? AccessSiteConnectionState::kReady
                                      : AccessSiteConnectionState::kUnavailable,
           .failure_reason = {},
           .recheck_committed_legacy = legacy && !acknowledged,
           .operation_failure_reason = std::move(failure_reason)};
  if (!acknowledged && state.operation_failure_reason.empty()) {
    state.failure_reason = test_profile
        ? "The selected test Profile is unavailable."
        : "Proxy connection is unavailable. Reload to reconnect.";
  }
}

bool AccessSiteProxyBubbleController::HasAcknowledgedCommittedLegacySite(
    const AccessSiteHost& host,
    bool website_context) const {
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), {})
      : std::nullopt;
  auto* coordinator = AccessServiceCoordinator::Get(profile_);
  if (!owner || !local_core_ready_ || core_owner_ != owner ||
      !transport->OwnsConfiguredPartition(*owner) ||
      !coordinator || !coordinator->CanDispatch(*owner)) {
    return false;
  }
  const auto committed =
      coordinator->ReadCommittedSnapshot(owner->storage_partition_token);
  if (committed.status != StoreStatus::kValid || !committed.value ||
      committed.value->owner != *owner) {
    return false;
  }
  const auto* site =
      FindCommittedBubbleSite(*committed.value, host, website_context);
  const auto* rule = site && !site->members.empty()
      ? &site->members.front().policy : nullptr;
  const std::string group = LocalProxyGroup(*owner);
  if (!rule || rule->owner != *owner || rule->mode != AccessMode::kProxy ||
      rule->proxy_group_id != group) {
    return false;
  }
  const auto common = CaptureCommittedPolicy(profile_, *committed.value);
  return common && transport->HasAcknowledgedLegacyProxy(
                       *owner, *common, group, core_registration_);
}

bool AccessSiteProxyBubbleController::RegisterTestProfileForPublication(
    AccessOptionsService& options, const OwnershipKey& owner,
    const aegis_access::GenerationTuple& common, uint64_t revision,
    const AccessLocalCoreReady& ready,
    scoped_refptr<TestProfilePublicationAttempt> attempt) {
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  if (!transport) return false;
  if (!test_profile_revoker_installed_) {
    test_profile_revoker_installed_ = options.SetTestNodeConsumerRevoker(
        base::BindRepeating(&AccessSiteProxyBubbleController::RetireTestProfileConsumer,
                           weak_factory_.GetWeakPtr()));
    if (!test_profile_revoker_installed_) return false;
    transport->SetTestListenerLostCallback(base::BindRepeating(
        &AccessSiteProxyBubbleController::OnTestProfileListenerLost,
        weak_factory_.GetWeakPtr()));
  }
  const std::string group = TestProfileGroup(ready);
  auto* selections = AccessProxySelectionGenerationSource::GetOrCreate(profile_);
  const auto* old = selections->binding_for_group(group);
  const auto selected = selections->CommitSelection(
      {group, ready.registration_id, "test-listener:" + ready.registration_id,
       "test-authorization:" + base::NumberToString(ready.authorization_generation),
       old ? old->binding_revision + 1 : 1});
  auto generations = common;
  generations.selection_generation = selected.generation;
  const aegis_access::AccessPreparedListenerBinding binding{
      aegis_access::kAccessNoAuthListenerWireVersion, ready.profile_id,
      ready.profile_version, ready.authorization_generation,
      ready.listener_socket_generation};
  if (selected.status !=
          aegis_access::ProxySelectionGenerationCommitStatus::kCommitted ||
      !transport->RegisterPreparedProxy(
          {{ready.registration_id, group, owner, generations, ready.transport,
            "127.0.0.1", ready.port}, std::nullopt, binding})) {
    return false;
  }
  if (!options.BeginTestNodePublication(revision, ready, std::move(attempt))) {
    transport->ForgetPreparedProxy(owner, group, ready.registration_id);
    return false;
  }
  return true;
}

void AccessSiteProxyBubbleController::RetireTestProfileConsumer(
    AccessLocalCoreReady ready, base::OnceCallback<void(bool)> completion) {
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  const auto owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), {})
      : std::nullopt;
  const auto* policy = runtime && owner ? runtime->GetPublishedPolicySnapshot(*owner) : nullptr;
  const auto common = runtime && owner
      ? runtime->CaptureCommonGenerationTupleOnUiThread(*owner)
      : std::nullopt;
  if (!policy || !common || ready.registration_id.empty()) {
    std::move(completion).Run(false);
    return;
  }
  transport->RetirePreparedTestListener(*policy, *common,
      TestProfileGroup(ready), ready.registration_id,
      std::move(completion));
}

std::optional<aegis_access::GenerationTuple>
AccessSiteProxyBubbleController::CaptureTestProfilePolicy(
    const OwnershipKey& owner, const std::string& group) const {
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  auto* coordinator = AccessServiceCoordinator::Get(profile_);
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  const auto expected_owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), {})
      : std::nullopt;
  if (!profile_->IsRegularProfile() || !expected_owner || *expected_owner != owner ||
      !transport->OwnsConfiguredPartition(owner) || !runtime ||
      !coordinator || !coordinator->CanDispatch(owner)) {
    return std::nullopt;
  }
  const auto* policy = runtime->GetPublishedPolicySnapshot(owner);
  if (!policy || policy->owner != owner ||
      !std::ranges::any_of(policy->rules, [&](const auto& rule) {
        return rule.mode == AccessMode::kProxy && rule.proxy_group_id == group;
      })) {
    return std::nullopt;
  }
  return runtime->CaptureCommonGenerationTupleOnUiThread(owner);
}

bool AccessSiteProxyBubbleController::IsActiveTestProfileGroup(
    const OwnershipKey& owner, const std::string& group,
    const aegis_access::GenerationTuple& common) const {
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  if (!options) return false;
  const auto active = options->test_profile_runtime();
  return active.ready && TestProfileGroup(active) == group &&
      AccessNetworkContextTransport::Get(profile_)->HasAcknowledgedTestListener(
          owner, common, group, active.registration_id);
}

void AccessSiteProxyBubbleController::RestoreTestProfileForRequest(
    const OwnershipKey& owner, const std::string& group,
    base::OnceCallback<void(bool)> completion) {
  const auto common = CaptureTestProfilePolicy(owner, group);
  const auto binding = ParseTestProfileGroup(group);
  if (!common || !binding) {
    std::move(completion).Run(false);
    return;
  }
  if (IsActiveTestProfileGroup(owner, group, *common)) {
    std::move(completion).Run(true);
    return;
  }
  if (saved_test_profile_restore_) {
    auto& pending = *saved_test_profile_restore_;
    if (pending.owner == owner && pending.group == group && pending.common == *common) {
      pending.waiters.push_back(std::move(completion));
    } else {
      std::move(completion).Run(false);
    }
    return;
  }
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  if (!options || !options->CanRestoreSavedTestNodeProfile()) {
    std::move(completion).Run(false);
    return;
  }
  saved_test_profile_restore_.emplace(SavedTestProfileRestore{owner, group, *common, {}, {}});
  saved_test_profile_restore_->waiters.push_back(std::move(completion));
  ++state_revision_;
  options->PrepareSavedTestNodeProfile(std::string(binding->first), binding->second,
      base::BindOnce(&AccessSiteProxyBubbleController::OnSavedTestProfilePrepared,
          weak_factory_.GetWeakPtr(), ++saved_test_profile_attempt_));
}

void AccessSiteProxyBubbleController::OnSavedTestProfilePrepared(
    uint64_t attempt, AccessLocalCoreReady ready) {
  if (!saved_test_profile_restore_ || attempt != saved_test_profile_attempt_) return;
  auto& restore = *saved_test_profile_restore_;
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  const auto prepared = options ? options->PreparedTestNode() : std::nullopt;
  if (!ready.ready || !prepared ||
      prepared->second.registration_id != ready.registration_id) {
    FinishSavedTestProfileRestore(false);
    return;
  }
  const uint64_t revision = prepared->first;
  if (TestProfileGroup(ready) != restore.group ||
      CaptureTestProfilePolicy(restore.owner, restore.group) != restore.common ||
      !RegisterTestProfileForPublication(*options, restore.owner, restore.common,
                                         revision, ready)) {
    options->RevokePreparedTestNodeIfCurrent(revision, ready);
    FinishSavedTestProfileRestore(false);
    return;
  }

  restore.publication_deadline = saved_test_profile_publication_clock_->NowTicks() +
                                 kTestProfilePublicationTimeout;
  saved_test_profile_publication_timer_.Start(FROM_HERE, kTestProfilePublicationTimeout,
      base::BindOnce(&AccessSiteProxyBubbleController::OnSavedTestProfilePublished,
          weak_factory_.GetWeakPtr(), attempt, revision, ready, false));
  const auto* policy = AccessPublishedRequestRuntime::Get(profile_)
      ->GetPublishedPolicySnapshot(restore.owner);
  AccessNetworkContextTransport::Get(profile_)->PublishPrivatePolicy(
      *policy, restore.common,
      base::BindOnce(&AccessSiteProxyBubbleController::OnSavedTestProfilePublished,
          weak_factory_.GetWeakPtr(), attempt, revision, std::move(ready)));
}

void AccessSiteProxyBubbleController::OnSavedTestProfilePublished(
    uint64_t attempt, uint64_t revision, AccessLocalCoreReady ready, bool acknowledged) {
  if (!saved_test_profile_restore_ || attempt != saved_test_profile_attempt_) return;
  const auto& restore = *saved_test_profile_restore_;
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  const bool current = acknowledged &&
      saved_test_profile_publication_clock_->NowTicks() < restore.publication_deadline &&
      CaptureTestProfilePolicy(restore.owner, restore.group) == restore.common &&
      AccessNetworkContextTransport::Get(profile_)->HasAcknowledgedTestListener(
          restore.owner, restore.common, restore.group, ready.registration_id);
  const bool matched = options &&
      options->CompleteTestNodePublication(revision, ready, current);
  if (matched && !current) {
    options->RevokeTestNodeProfile(base::DoNothing());
  } else if (options && !matched) {
    options->RevokePreparedTestNodeIfCurrent(revision, ready);
  }
  FinishSavedTestProfileRestore(matched && current);
}

void AccessSiteProxyBubbleController::FinishSavedTestProfileRestore(bool ready) {
  saved_test_profile_publication_timer_.Stop();
  auto callbacks = std::move(saved_test_profile_restore_->waiters);
  saved_test_profile_restore_.reset();
  ++state_revision_;
  for (auto& callback : callbacks) std::move(callback).Run(ready);
}

void AccessSiteProxyBubbleController::OnTestProfileListenerAuthFault(
    const OwnershipKey& owner, const aegis_access::GenerationTuple& observed,
    const std::string& registration_id) {
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  if (!options || !transport || registration_id.empty()) return;
  const auto current = options->test_profile_runtime();
  if (current.registration_id != registration_id || !current.no_local_auth ||
      current.transport != aegis_access::RegisteredProxyTransport::kHttp) return;
  const auto group = TestProfileGroup(current);
  const auto endpoint = transport->PreparedProxyForGroup(owner, group);
  if (!endpoint || endpoint->registration_id != registration_id) return;
  auto expected = endpoint->generations;
  expected.policy_generation = observed.policy_generation;
  if (expected != observed) return;
  for (const auto& [host, selection] : test_profile_sites_) {
    if (selection == group) {
      host_states_[host] = {AccessSiteConnectionState::kUnavailable,
                            "The local listener unexpectedly requested authentication."};
    }
  }
  ++state_revision_;
  options->RevokeTestNodeProfile(base::DoNothing());
}

void AccessSiteProxyBubbleController::OnTestProfileListenerLost(
    const OwnershipKey& owner, const std::string& group,
    const std::string& registration_id) {
  auto* options = AccessOptionsService::GetOrCreate(profile_);
  if (!options || registration_id.empty()) return;
  const auto current = options->test_profile_runtime();
  if (current.registration_id != registration_id || TestProfileGroup(current) != group)
    return;
  for (const auto& [host, selection] : test_profile_sites_) {
    if (selection == group) {
      host_states_[host] = {AccessSiteConnectionState::kUnavailable,
                            "The network process restarted. Reconnecting the test Profile."};
    }
  }
  ++state_revision_;
  // The next protected request re-reads the saved Profile binding and starts
  // from a new listener registration.
  options->RevokeTestNodeProfileAfterNetworkLoss(registration_id);
}
#endif
bool AccessSiteProxyBubbleController::CanViewCoreLogs() const {
  return core_logs_ != nullptr;
}
void AccessSiteProxyBubbleController::SetCoreLogLevel(std::string_view level) {
  if (!CanViewCoreLogs() ||
      std::find(kCoreLogLevels.begin(), kCoreLogLevels.end(), level) == kCoreLogLevels.end())
    return;
  profile_->GetOriginalProfile()->GetPrefs()->SetString(
      profile_->IsOffTheRecord() ? kAccessPrivateCoreLogLevel : kAccessCoreLogLevel,
      std::string(level));
  core_logs_->SetLevel(level);
  ++state_revision_;
}
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
void AccessSiteProxyBubbleController::ReadConfiguredNodeId(
    base::OnceCallback<void(std::string)> done) {
  auto input = ResolveAccessLocalCoreInput();
  if (!input) {
    std::move(done).Run(std::string());
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&ReadConfiguredAccessNodeId, std::move(*input)),
      std::move(done));
}

void AccessSiteProxyBubbleController::ShareCoreLogs(
    content::WebContents* tab,
    std::string preview,
    uint64_t version,
    base::OnceCallback<void(CoreLogShareResult)> done) {
  if (!CanViewCoreLogs() || !tab ||
      tab->GetBrowserContext() != profile_ || preview.empty() || !core_logs_->CanShare(version)) {
    std::move(done).Run(CoreLogShareResult::kFailed);
    return;
  }
  auto picker_succeeded = std::make_shared<bool>(false);
  log_share_.Start(profile_->GetOriginalProfile()->GetPath(), std::move(preview), base::BindOnce(
      [](base::WeakPtr<content::WebContents> tab, content::GlobalRenderFrameHostId frame,
         scoped_refptr<AccessCoreLogStore> store, uint64_t version,
         std::shared_ptr<bool> picker_succeeded, base::FilePath path,
         base::ScopedClosureRunner release) {
        if (!tab || path.empty() || !store->CanShare(version) ||
            tab->GetPrimaryMainFrame()->GetGlobalId() != frame ||
            tab->GetVisibility() != content::Visibility::VISIBLE ||
            !tab->GetRenderWidgetHostView()) {
          return;
        }
#if BUILDFLAG(IS_WIN)
        auto window = tab->GetTopLevelNativeWindow();
        if (!window) return;
        ExportAccessLogOnWindows(
            window, std::move(path), base::BindRepeating(
                [](base::WeakPtr<content::WebContents> tab,
                   content::GlobalRenderFrameHostId frame,
                   scoped_refptr<AccessCoreLogStore> store, uint64_t version) {
                  return tab && tab->GetPrimaryMainFrame()->GetGlobalId() == frame &&
                         tab->GetVisibility() == content::Visibility::VISIBLE &&
                         store->CanShare(version);
                }, tab, frame, std::move(store), version),
            base::BindOnce([](std::shared_ptr<bool> picker_succeeded,
                              base::ScopedClosureRunner release, bool ok) {
              *picker_succeeded = ok;
              release.RunAndReset();
            }, std::move(picker_succeeded), std::move(release)));
#else
        tab->GetRenderWidgetHostView()->ShowSharePicker("", "", GURL(), {path.value()},
            base::BindOnce([](base::ScopedClosureRunner release, std::shared_ptr<bool> picker_succeeded,
                              blink::mojom::ShareError result) {
              *picker_succeeded = result != blink::mojom::ShareError::INTERNAL_ERROR;
              release.RunAndReset();
            }, std::move(release), std::move(picker_succeeded)));
#endif
      }, tab->GetWeakPtr(), tab->GetPrimaryMainFrame()->GetGlobalId(), core_logs_,
      version, picker_succeeded),
      base::BindOnce([](std::shared_ptr<bool> picker_succeeded,
                        base::OnceCallback<void(CoreLogShareResult)> done,
                        bool cleaned) {
        auto result = CoreLogShareResult::kFailed;
        if (!cleaned) {
          result = CoreLogShareResult::kCleanupFailed;
        } else if (*picker_succeeded) {
          result = CoreLogShareResult::kCompleted;
        }
        std::move(done).Run(result);
      }, picker_succeeded, std::move(done)));
}
#endif
void AccessSiteProxyBubbleController::CreateLocalCore() {
  if (local_core_) return;
  auto input = ResolveAccessLocalCoreInput();
  if (!input) return;
  input->log_store = core_logs_;
  local_core_ = std::make_unique<AccessLocalCoreManager>(*input);
  local_core_->SetStoppedCallback(base::BindRepeating(
      &AccessSiteProxyBubbleController::OnLocalCoreStopped,
      weak_factory_.GetWeakPtr()));
}
bool AccessSiteProxyBubbleController::RegisterPreparedCore(
    const OwnershipKey& owner, const AccessLocalCoreReady& result) {
  if (!result.ready || !result.local_credentials) return false;
  // Rebinding a child does not change a saved rule or its revision.
  auto* identity = AccessIdentityGenerationSource::GetOrCreate(profile_);
  if (!identity->binding()) identity->CommitIdentity(
      {aegis_access::AccessIdentityKind::kSignedOut, {}, {}, LocalAccessIdentityChannel(owner.channel), "authorized-local-node"});
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  const auto common = runtime ? runtime->CaptureCommonGenerationTupleOnUiThread(owner)
                              : std::nullopt;
  if (!common || !result.local_credentials) {
    return false;
  }
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  auto* selections = AccessProxySelectionGenerationSource::GetOrCreate(profile_);
  const std::string group = LocalProxyGroup(owner);
  const auto* old = selections->binding_for_group(group);
  const bool same = old && old->endpoint_id == result.registration_id;
  const auto selected = selections->CommitSelection(
      {group, result.registration_id, "dev-process:" + result.registration_id,
       "dev-binding:" + result.registration_id,
       same ? old->binding_revision : old ? old->binding_revision + 1 : 1});
  if (selected.status != aegis_access::ProxySelectionGenerationCommitStatus::kCommitted &&
      selected.status != aegis_access::ProxySelectionGenerationCommitStatus::kUnchanged) {
    return false;
  }
  auto generations = *common;
  generations.selection_generation = selected.generation;
  if (!selected.generation || !transport || !transport->RegisterPreparedProxy({
          {result.registration_id, group, owner, generations,
           aegis_access::RegisteredProxyTransport::kHttp, "127.0.0.1", result.port},
          result.local_credentials})) {
    return false;
  }
  core_owner_ = owner;
  core_registration_ = result.registration_id;
  local_core_ready_ = false;
  return true;
}
void AccessSiteProxyBubbleController::OnCommittedCoreVerified(
    AccessSiteHost host, uint64_t attempt, AccessLocalCoreReady result) {
  if (attempt != committed_verification_attempt_ || verifying_host_ != host)
    return;
  verifying_host_.reset();
  if (!result.ready) {
    OnLocalCoreStopped(core_registration_);
    FailCorePreparation(host, std::move(result.error));
    return;
  }
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), base::FilePath())
      : std::nullopt;
  const auto endpoint = owner ? transport->CurrentEndpoint(*owner) : std::nullopt;
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  auto live = owner && runtime ? runtime->CaptureCommonGenerationTupleOnUiThread(*owner)
                               : std::nullopt;
  auto registered_common = endpoint ? std::make_optional(endpoint->generations) : std::nullopt;
  if (registered_common) registered_common->selection_generation = 0;
  if (!endpoint || endpoint->registration_id != result.registration_id ||
      endpoint->port != result.port ||
      endpoint->generations.network_epoch != transport->network_epoch() ||
      !transport->HasReadyPrivateContext(*owner) || live != registered_common) {
    FailCorePreparation(host, "The proxy binding changed during verification.");
    return;
  }
  publication_in_flight_ = false;
  ++state_revision_;
  host_states_[host] = {AccessSiteConnectionState::kReady, {}};
  local_core_ready_ = true;
  FinishNavigationPreparation(true);
}

bool AccessSiteProxyBubbleController::IsCurrentPreparedCore(
    const OwnershipKey& owner, const AccessLocalCoreReady& result) const {
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  const auto endpoint = transport ? transport->CurrentEndpoint(owner) : std::nullopt;
  const auto live = runtime
      ? runtime->CaptureCommonGenerationTupleOnUiThread(owner) : std::nullopt;
  auto bound = endpoint ? std::make_optional(endpoint->generations) : std::nullopt;
  if (bound) bound->selection_generation = 0;
  return result.ready && endpoint && live && bound == live &&
      endpoint->proxy_group_id == LocalProxyGroup(owner) &&
      endpoint->registration_id == result.registration_id &&
      endpoint->port == result.port && transport->HasCurrentPrivatePolicy(owner, *live);
}

void AccessSiteProxyBubbleController::EnsureLocalCoreForRequest(
    const OwnershipKey& owner, const std::string& group,
    base::OnceCallback<void(bool)> completion) {
#if BUILDFLAG(IS_MAC)
  if (IsTestProfileProxyGroup(group)) {
    RestoreTestProfileForRequest(owner, group, std::move(completion));
    return;
  }
#endif
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto expected_owner = transport ? transport->OwnerForPartition(
      AccessChannelForCurrentBuild(), {}) : std::nullopt;
  auto* coordinator = AccessServiceCoordinator::Get(profile_);
  if (!expected_owner || *expected_owner != owner ||
      !transport->OwnsConfiguredPartition(owner) || group != LocalProxyGroup(owner) ||
      (!IsAccessDevAllowBlockPanelAdmitted(chrome::GetChannel()) ||
       IsGeneralAccessRuleEditorRegistered(chrome::GetChannel())) ||
      !coordinator || !coordinator->CanDispatch(owner)) {
    std::move(completion).Run(false);
    return;
  }
  const auto endpoint = transport->CurrentEndpoint(owner);
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  const auto common = runtime
      ? runtime->CaptureCommonGenerationTupleOnUiThread(owner) : std::nullopt;
  if (local_core_ready_ && core_owner_ == owner && endpoint && common &&
      endpoint->registration_id == core_registration_ &&
      endpoint->generations.identity_generation == common->identity_generation &&
      endpoint->generations.network_epoch == common->network_epoch &&
      endpoint->generations.base_proxy_config_generation == common->base_proxy_config_generation) {
    std::move(completion).Run(true);
    return;
  }
  core_request_waiters_.push_back(std::move(completion));
  if (restoring_core_ || publication_in_flight_) return;
  CreateLocalCore();
  if (!local_core_) {
    FinishCoreRequests(false);
    return;
  }
  local_core_ready_ = false;
  restoring_core_ = true;
  const auto attempt = ++core_attempt_;
  for (auto& [host, state] : host_states_) {
#if BUILDFLAG(IS_MAC)
    if (test_profile_sites_.contains(host)) continue;
#endif
    if (intent_store_.Get(host) == AccessSiteIntent::kAllow &&
        !state.runtime_fault) {
      state.connection = AccessSiteConnectionState::kConnecting;
      state.failure_reason.clear();
    }
  }
  ++state_revision_;
  local_core_->Prepare(base::BindOnce(
      &AccessSiteProxyBubbleController::OnRestoredCorePrepared,
      weak_factory_.GetWeakPtr(), owner, attempt));
}

void AccessSiteProxyBubbleController::OnRestoredCorePrepared(
    OwnershipKey owner, uint64_t attempt, AccessLocalCoreReady result) {
  if (attempt != core_attempt_ || !restoring_core_) return;
  if (!RegisterPreparedCore(owner, result)) {
    OnLocalCoreStopped(core_registration_);
    FinishCoreRequests(false);
    return;
  }
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto* policy = runtime ? runtime->GetPublishedPolicySnapshot(owner) : nullptr;
  const auto common = runtime
      ? runtime->CaptureCommonGenerationTupleOnUiThread(owner) : std::nullopt;
  if (!transport || !policy || !common) {
    FinishCoreRequests(false);
    return;
  }
  transport->PublishPrivatePolicy(*policy, *common, base::BindOnce(
      &AccessSiteProxyBubbleController::OnRestoredCorePolicyReady,
      weak_factory_.GetWeakPtr(), owner, attempt, *common));
}
void AccessSiteProxyBubbleController::OnRestoredCorePolicyReady(
    OwnershipKey owner, uint64_t attempt,
    aegis_access::GenerationTuple expected, bool accepted) {
  if (attempt != core_attempt_ || !restoring_core_) return;
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  const auto common = runtime
      ? runtime->CaptureCommonGenerationTupleOnUiThread(owner) : std::nullopt;
  if (!accepted || !common || *common != expected) {
    FinishCoreRequests(false);
    return;
  }
  local_core_->Verify(base::BindOnce(
      &AccessSiteProxyBubbleController::OnRestoredCoreVerified,
      weak_factory_.GetWeakPtr(), owner, attempt));
}
void AccessSiteProxyBubbleController::OnRestoredCoreVerified(
    OwnershipKey owner, uint64_t attempt, AccessLocalCoreReady result) {
  if (attempt != core_attempt_ || !restoring_core_) return;
  if (!result.ready) {
    OnLocalCoreStopped(core_registration_);
    return;
  }
  local_core_ready_ = IsCurrentPreparedCore(owner, result);
  if (!local_core_ready_) {
    FinishCoreRequests(false);
    return;
  }
  for (auto& [host, state] : host_states_) {
#if BUILDFLAG(IS_MAC)
    if (test_profile_sites_.contains(host)) continue;
#endif
    if (intent_store_.Get(host) == AccessSiteIntent::kAllow) {
      state = {AccessSiteConnectionState::kReady, {}};
    }
  }
  ++state_revision_;
  FinishCoreRequests(true);
}
void AccessSiteProxyBubbleController::FinishNavigationPreparation(bool ready) {
  auto on_enabled = std::move(on_enabled_);
  auto retry = std::move(retry_completion_);
  auto next = std::move(queued_retry_);
  GURL url = std::exchange(retry_url_, GURL());
  if (retry) std::move(retry).Run(ready);
  FinishCoreRequests(ready);
  // Never reuse the first attempt's result. The new document ticket needs its
  // own core verification and acknowledgement before it can reload.
  if (next && !next.IsCancelled()) {
    RequestFailedNavigationProxy(url, std::move(next));
  }
  if (ready && on_enabled) std::move(on_enabled).Run();
}

void AccessSiteProxyBubbleController::FinishCoreRequests(bool ready) {
  if (restoring_core_ && !ready) {
    for (auto& [host, state] : host_states_) {
#if BUILDFLAG(IS_MAC)
      if (test_profile_sites_.contains(host)) continue;
#endif
      if (intent_store_.Get(host) == AccessSiteIntent::kAllow &&
          !state.runtime_fault) {
        state.connection = AccessSiteConnectionState::kUnavailable;
        state.failure_reason = "The saved proxy binding is not ready.";
      }
    }
    ++state_revision_;
  }
  restoring_core_ = false;
  auto callbacks = std::move(core_request_waiters_);
  core_request_waiters_.clear();
  for (auto& callback : callbacks) std::move(callback).Run(ready);
}
void AccessSiteProxyBubbleController::OnLocalCoreStopped(std::string registration_id) {
  if (registration_id.empty() || registration_id != core_registration_ || !core_owner_) return;
  local_core_ready_ = false;
  ++core_attempt_;
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  auto* runtime = AccessPublishedRequestRuntime::Get(profile_);
  if (transport && transport->ForgetPreparedProxy(*core_owner_, LocalProxyGroup(*core_owner_),
                                                 registration_id) && runtime) {
    const auto* policy = runtime->GetPublishedPolicySnapshot(*core_owner_);
    const auto common = runtime->CaptureCommonGenerationTupleOnUiThread(*core_owner_);
    if (policy && common)
      transport->PublishPrivatePolicy(*policy, *common, base::BindOnce([](bool) {}));
  }
  for (auto& [host, state] : host_states_) {
#if BUILDFLAG(IS_MAC)
    if (test_profile_sites_.contains(host)) continue;
#endif
    if (intent_store_.Get(host) == AccessSiteIntent::kAllow &&
        !state.runtime_fault) {
      state = {.connection = AccessSiteConnectionState::kUnavailable,
               .failure_reason = "The local proxy has stopped.",
               .recheck_committed_legacy = true};
    }
  }
  ++state_revision_;
  FinishCoreRequests(false);
}

AccessSiteProxyBubbleController::HostState
AccessSiteProxyBubbleController::HostState::FromRuntimeFault(
    RuntimeFault fault) {
  const bool wait = fault == RuntimeFault::kWait;
  return {
      .connection = wait ? AccessSiteConnectionState::kConnecting
                         : AccessSiteConnectionState::kUnavailable,
      .failure_reason = AccessSiteProxySessionErrorReadableMessage(
          AccessSiteProxySessionError::kRuntimeFault,
          wait ? "waiting for node recovery" : "node or runtime unavailable"),
      .runtime_fault = fault};
}

bool AccessSiteProxyBubbleController::NotifyHostProxyRuntimeFault(
    const AccessSiteHost& host,
    bool wait) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  ++state_revision_;
  if (MakeAccessSiteHost(host.value).status != AccessSiteHostStatus::kOk) {
    host_states_[host].failure_reason = AccessSiteProxySessionErrorReadableMessage(
        AccessSiteProxySessionError::kInvalidHost);
    host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
    return false;
  }
  if (intent_store_.Get(host) != AccessSiteIntent::kAllow) {
    host_states_[host].failure_reason = AccessSiteProxySessionErrorReadableMessage(
        AccessSiteProxySessionError::kRuntimeFault,
        "site proxy is not enabled");
    return false;
  }
  // Keep allow intent and the published PROXY selection. Do not ClearProxy
  // or publish DIRECT — new requests fail or wait against the PROXY policy.
  host_states_[host] = HostState::FromRuntimeFault(
      wait ? RuntimeFault::kWait : RuntimeFault::kUnavailable);
  return true;
}

bool AccessSiteProxyBubbleController::RecoverHostProxySession(
    const AccessSiteHost& host) {
  return RequestHostProxySession(host, true);
}

bool AccessSiteProxyBubbleController::
    RebuildHostProxyAfterNetworkServiceRestart(const AccessSiteHost& host) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (intent_store_.Get(host) != AccessSiteIntent::kAllow) {
    return false;
  }
  // Republish PROXY for the allow host. RequestHostProxySession never admits
  // DIRECT on the enable path.
  return RequestHostProxySession(host, true);
}

AccessSiteProxySessionError
AccessSiteProxyBubbleController::EnsureLocalProxyReady(
    const AccessSiteHost& host,
    AccessSiteProxyLocalProxyReadyInfo* out) const {
  if (!out) {
    return AccessSiteProxySessionError::kLocalProxyUnavailable;
  }
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  if (!transport) {
    return AccessSiteProxySessionError::kLocalProxyUnavailable;
  }
  const std::optional<OwnershipKey> owner =
      transport->OwnerForPartition(AccessChannelForCurrentBuild(), base::FilePath());
  if (!owner || !transport->OwnsConfiguredPartition(*owner)) {
    return AccessSiteProxySessionError::kMissingOwner;
  }
  const std::optional<aegis_access::RegisteredProxyEndpoint> endpoint =
      transport->CurrentEndpoint(*owner);
  if (!endpoint) {
    return AccessSiteProxySessionError::kLocalProxyNotReady;
  }
  if (!IsNumericLoopbackHost(endpoint->host) || endpoint->port == 0 ||
      endpoint->proxy_group_id.empty() ||
      endpoint->generations.selection_generation == 0) {
    return AccessSiteProxySessionError::kLocalProxyNotReady;
  }
  if (endpoint->transport != aegis_access::RegisteredProxyTransport::kHttp &&
      endpoint->transport != aegis_access::RegisteredProxyTransport::kSocks5) {
    return AccessSiteProxySessionError::kLocalProxyNotReady;
  }
  (void)host;
  out->owner = *owner;
  out->proxy_group_id = endpoint->proxy_group_id;
  return AccessSiteProxySessionError::kOk;
}

std::unique_ptr<AccessRuleStore>
AccessSiteProxyBubbleController::OpenSessionStore(
    const OwnershipKey& owner) const {
  auto binding = TrustedAccessStoreBindingFactory::CreateEphemeral(
      owner.channel, "aegis-access-bubble-session", owner.profile_token);
  if (!binding) {
    return nullptr;
  }
  auto store = std::make_unique<AccessRuleStore>(*binding);
  if (store->Open() != StoreStatus::kValid) {
    return nullptr;
  }
  return store;
}

void AccessSiteProxyBubbleController::BeginEnablePublication(
    const AccessSiteHost& host,
    AccessSiteProxyLocalProxyReadyInfo ready,
    base::OnceCallback<void(AccessMutationTransactionResult)> completion) {
  auto fail = [&](std::string message) {
    if (completion) {
      std::move(completion).Run({AccessMutationTransactionStatus::kPrepareFailed});
    } else {
      FailCorePreparation(host, std::move(message));
    }
  };
  auto* coordinator = AccessServiceCoordinator::GetOrCreate(profile_);
  if (!coordinator) {
    host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
    host_states_[host].failure_reason = AccessSiteProxySessionErrorReadableMessage(
        AccessSiteProxySessionError::kMissingCoordinator);
    fail(host_states_[host].failure_reason);
    return;
  }

  std::unique_ptr<AccessRuleStore> store;
  if (!coordinator->has_retained_store()) {
    store = OpenSessionStore(ready.owner);
    if (!store) {
      host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
      host_states_[host].failure_reason = AccessSiteProxySessionErrorReadableMessage(
          AccessSiteProxySessionError::kMissingStore);
      fail(host_states_[host].failure_reason);
    return;
    }
  }

  const auto committed = store
      ? store->ReadCommittedSnapshot(ready.owner.storage_partition_token)
      : coordinator->ReadCommittedSnapshot(ready.owner.storage_partition_token);
  if (committed.status != StoreStatus::kMissing &&
      (committed.status != StoreStatus::kValid || !committed.value)) {
    host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
    host_states_[host].failure_reason = "Access rule store requires recovery.";
    fail(host_states_[host].failure_reason);
    return;
  }
  uint64_t expected_revision = 0;
  if (committed.value) {
    for (const auto& group : committed.value->site_groups) {
      if (group.group.site_toggle_id == "bubble-site:" + host.value) {
        expected_revision = group.group.revision;
        break;
      }
    }
  }
  const AccessSiteProxySessionMutationPlan plan =
      BuildSiteProxySessionMutationPlan(host, AccessSiteIntent::kAllow,
                                        ready.owner, ready.proxy_group_id,
                                        base::Uuid::GenerateRandomV4().AsLowercaseString(),
                                        expected_revision, /*website_context=*/true);
  if (plan.mutation.status != AccessSiteIntentMutationStatus::kReady ||
      !plan.mutation.request) {
    host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
    host_states_[host].failure_reason = AccessSiteProxySessionErrorReadableMessage(
        plan.mutation.status ==
                AccessSiteIntentMutationStatus::kMissingProxyGroup
            ? AccessSiteProxySessionError::kMissingProxyGroup
            : AccessSiteProxySessionError::kPrepareFailed);
    fail(host_states_[host].failure_reason);
    return;
  }

  publication_in_flight_ = true;
  if (!completion) {
    completion = base::BindOnce(&AccessSiteProxyBubbleController::OnPublicationComplete,
                                weak_factory_.GetWeakPtr(), host, /*enabling=*/true);
  }
  coordinator->CommitSiteGroupMutation(
      std::move(store), std::move(*plan.mutation.request), plan.selector,
      std::move(completion));
}

void AccessSiteProxyBubbleController::BeginDisablePublication(
    const AccessSiteHost& host, base::OnceClosure on_disabled) {
  auto* transport = AccessNetworkContextTransport::Get(profile_);
  const auto owner = transport
      ? transport->OwnerForPartition(AccessChannelForCurrentBuild(), base::FilePath())
      : std::nullopt;
  auto* coordinator = AccessServiceCoordinator::GetOrCreate(profile_);
  if (!owner || !transport->OwnsConfiguredPartition(*owner) || !coordinator) {
    host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
    host_states_[host].failure_reason = AccessSiteProxySessionErrorReadableMessage(
        AccessSiteProxySessionError::kServiceUnavailable);
    return;
  }
  AccessSiteProxyLocalProxyReadyInfo ready;
  ready.owner = *owner;

  std::unique_ptr<AccessRuleStore> store;
  if (!coordinator->has_retained_store()) {
    store = OpenSessionStore(ready.owner);
    if (!store) {
      return;
    }
  }

  const auto committed = store
      ? store->ReadCommittedSnapshot(ready.owner.storage_partition_token)
      : coordinator->ReadCommittedSnapshot(ready.owner.storage_partition_token);
  if (committed.status != StoreStatus::kMissing &&
      (committed.status != StoreStatus::kValid || !committed.value)) {
    host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
    host_states_[host].failure_reason = "Access rule store requires recovery.";
    return;
  }
  uint64_t expected_revision = 0;
  if (committed.value) {
    for (const auto& group : committed.value->site_groups) {
      if (group.group.site_toggle_id == "bubble-site:" + host.value) {
        expected_revision = group.group.revision;
        break;
      }
    }
  }
  const AccessSiteProxySessionMutationPlan plan =
      BuildSiteProxySessionMutationPlan(host, AccessSiteIntent::kOff,
                                        ready.owner, /*proxy_group_id=*/"",
                                        base::Uuid::GenerateRandomV4().AsLowercaseString(),
                                        expected_revision, /*website_context=*/true);
  if (plan.mutation.status != AccessSiteIntentMutationStatus::kReady ||
      !plan.mutation.request) {
    return;
  }

  publication_in_flight_ = true;
  coordinator->CommitSiteGroupMutation(
      std::move(store), std::move(*plan.mutation.request), plan.selector,
      base::BindOnce([](base::WeakPtr<AccessSiteProxyBubbleController> self,
                        AccessSiteHost host, base::OnceClosure on_disabled,
                        AccessMutationTransactionResult result) {
        if (!self) return;
        const bool committed = result.status == AccessMutationTransactionStatus::kCommitted;
        self->OnPublicationComplete(host, false, std::move(result));
        if (self && committed && on_disabled) std::move(on_disabled).Run();
      }, weak_factory_.GetWeakPtr(), host, std::move(on_disabled)));
}

void AccessSiteProxyBubbleController::OnPublicationComplete(
    AccessSiteHost host,
    bool enabling,
    AccessMutationTransactionResult result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  publication_in_flight_ = false;
  ++state_revision_;
  if (result.status == AccessMutationTransactionStatus::kCommitted ||
      result.status == AccessMutationTransactionStatus::kCommittedUnavailable) {
    AdoptWebsiteIntent(host, enabling);
#if BUILDFLAG(IS_MAC)
    test_profile_sites_.erase(host);
#endif
  }

  if (enabling) {
    if (result.status == AccessMutationTransactionStatus::kCommitted) {
      publication_in_flight_ = true;
      verifying_host_ = host;
      local_core_->Verify(base::BindOnce(
          &AccessSiteProxyBubbleController::OnCommittedCoreVerified,
          weak_factory_.GetWeakPtr(), host, ++committed_verification_attempt_));
      return;
    }
    // Preserve the last committed choice; preparation is not a commit.
    host_states_[host].connection = AccessSiteConnectionState::kUnavailable;
    host_states_[host].failure_reason = MutationStatusReadableMessage(result.status);
    FinishNavigationPreparation(false);
    return;
  }

  // A failed disable must not display an uncommitted OFF choice.
  host_states_[host].connection = result.status == AccessMutationTransactionStatus::kCommitted
      ? AccessSiteConnectionState::kIdle : AccessSiteConnectionState::kUnavailable;
  if (result.status != AccessMutationTransactionStatus::kCommitted) {
    host_states_[host].failure_reason = MutationStatusReadableMessage(result.status);
  } else {
    host_states_[host].failure_reason.clear();
  }
  (void)host;
  if (!restoring_core_) FinishCoreRequests(false);
}

}  // namespace aegis::access
