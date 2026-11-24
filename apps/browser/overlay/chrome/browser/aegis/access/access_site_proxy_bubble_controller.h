// Copyright 2026 GCSA

#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_SITE_PROXY_BUBBLE_CONTROLLER_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_SITE_PROXY_BUBBLE_CONTROLLER_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "build/build_config.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/aegis/access/access_local_core_manager.h"
#include "base/supports_user_data.h"
#include "chrome/browser/aegis/access/access_rule_store.h"
#include "chrome/browser/aegis/access/access_service_coordinator.h"
#include "chrome/browser/aegis/access/access_site_dev_allow_block_panel.h"
#include "chrome/browser/aegis/access/access_site_proxy_bubble_model.h"
#include "chrome/browser/aegis/access/access_site_proxy_session.h"
#include "url/gurl.h"

class Profile;
namespace content { class WebContents; }
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "base/callback_list.h"
#include "chrome/browser/aegis/access/access_core_log_share.h"
#endif
#if BUILDFLAG(IS_MAC)
#include "base/time/default_tick_clock.h"
#include "base/timer/timer.h"
#endif

namespace aegis::access {

#if BUILDFLAG(IS_MAC)
class AccessOptionsService;
class TestProfilePublicationAttempt;
#endif
// Profile-owned product state for the toolbar site-proxy bubble.
// Holds intent separately from connection. P-mac-3 wires the toggle onto
// PrepareCandidate → Publish → ACK (and requires local proxy readiness).
class AccessSiteProxyBubbleController : public base::SupportsUserData::Data {
 public:
  static AccessSiteProxyBubbleController* Get(Profile* profile);
  static AccessSiteProxyBubbleController* GetOrCreate(Profile* profile);

  AccessSiteProxyBubbleController(const AccessSiteProxyBubbleController&) =
      delete;
  AccessSiteProxyBubbleController& operator=(
      const AccessSiteProxyBubbleController&) = delete;
  ~AccessSiteProxyBubbleController() override;

  AccessSiteIntentStore& intent_store() { return intent_store_; }
  const AccessSiteIntentStore& intent_store() const { return intent_store_; }

  bool CanViewCoreLogs() const;
  AccessCoreLogStore* core_logs() const { return core_logs_.get(); }
  void SetCoreLogLevel(std::string_view level);
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  void ReadConfiguredNodeId(base::OnceCallback<void(std::string)> done);
  enum class CoreLogShareResult { kCompleted, kFailed, kCleanupFailed };
  void ShareCoreLogs(content::WebContents* tab,
                     std::string preview,
                     uint64_t version,
                     base::OnceCallback<void(CoreLogShareResult)> done);
#endif

  uint64_t state_revision() const { return state_revision_; }

  AccessSiteProxyBubbleSnapshot SnapshotForCommittedUrl(
      const GURL& committed_url) const;

  // Updates product intent only (no publish). Prefer RequestHostProxySession
  // from the toolbar so publication/ACK runs.
  bool ApplyHostProxyToggle(const AccessSiteHost& host, bool enabled);

  // Product path: enable → local proxy ready → Prepare/Publish/ACK → ready.
  // Disable does not require a live proxy; publication failures remain visible.
  // Returns false only when the host/toggle cannot be accepted.
  bool RequestHostProxySession(const AccessSiteHost& host, bool enabled);
  bool RequestHostProxySession(const AccessSiteHost& host, bool enabled,
                               base::OnceClosure on_ready);
  void RequestFailedNavigationProxy(const GURL& original_url,
      base::OnceCallback<void(bool)> completion);
  base::RepeatingCallback<void(const GURL&, base::OnceCallback<void(bool)>)>
  GetFailedNavigationRetryHandler();
  void EnsureProfileRestored(const OwnershipKey& owner);
  static std::string LocalProxyGroup(const OwnershipKey& owner);
#if BUILDFLAG(IS_MAC)
  static bool IsTestProfileProxyGroup(std::string_view group);
  bool HasPreparedTestNode() const;
  std::optional<AccessLocalCoreReady> AcknowledgedTestProfileListener() const;
  // True when |host| can explicitly select a Test Profile now: either a
  // prepared listener awaits its first publication, or the active acknowledged
  // listener can be reused for this site without re-registration.
  bool CanSelectTestProfileForSite(const AccessSiteHost& host) const;
  bool RequestTestProfileForSite(const AccessSiteHost& host);
  void OnTestProfileListenerAuthFault(
      const OwnershipKey& owner, const aegis_access::GenerationTuple& observed,
      const std::string& registration_id);
#endif
  // Called only after Browser-owned policy evaluation selects this local
  // group. Restores its runtime binding without mutating persisted intent.
  void EnsureLocalCoreForRequest(const OwnershipKey& owner,
      const std::string& group, base::OnceCallback<void(bool)> completion);

  // P-mac-4 DEV panel. False on Beta/Release — UI must not register the
  // section. Never admits a general Access rule editor.
  bool IsDevAllowBlockPanelRegistered() const;
  AccessDevAllowBlockPanelSnapshot DevAllowBlockSnapshotForCommittedUrl(
      const GURL& committed_url) const;
  // Isolated diagnostic preview for an explicit host. Does not mutate the
  // committed product intent, connection state or published policy.
  AccessDevAllowBlockApplyResult ApplyDevAllowBlock(
      std::string_view raw_host,
      AccessDevAllowBlockAction action);

  // P-mac-5: node/runtime fault while allow stays allow. Never clears the
  // PROXY selection or publishes DIRECT. |wait| holds new requests; otherwise
  // they fail closed.
  bool NotifyHostProxyRuntimeFault(const AccessSiteHost& host, bool wait);

  // Re-arms the allow publication path after fault recovery.
  bool RecoverHostProxySession(const AccessSiteHost& host);

  // Rebuild PROXY publication after Network Service restart (no DIRECT).
  bool RebuildHostProxyAfterNetworkServiceRestart(const AccessSiteHost& host);

 private:
  friend class AccessSiteProxyProductTestPeer;
  explicit AccessSiteProxyBubbleController(Profile* profile);
  void PrepareLocalCore(const AccessSiteHost& host);
  void RestoreSavedIntents(OwnershipKey owner, bool restored);
  void AdoptWebsiteIntent(const AccessSiteHost& host, bool enabled);
#if BUILDFLAG(IS_MAC)
  // Original-tab retry admission for a manual Test Profile binding that does
  // not depend on the legacy DEV node file. Route checks stay at preparation.
  bool HasManualTestProfileRetryBinding() const;
  // |host| null: any committed site rule selects a Test Profile group.
  bool HasCommittedTestProfileSite(const AccessSiteHost* host) const;
#endif
  void OnProfileRestoredForCore(AccessSiteHost host, OwnershipKey owner,
                               bool restored);
  void OnLocalCorePrepared(AccessSiteHost host, OwnershipKey owner,
                          AccessLocalCoreReady result);
  void OnCommittedCoreVerified(AccessSiteHost host, uint64_t attempt,
                               AccessLocalCoreReady result);
  void CreateLocalCore();
#if BUILDFLAG(IS_MAC)
  void OnTestProfileSiteRestored(
      AccessSiteHost host,
      OwnershipKey owner,
      uint64_t revision,
      AccessLocalCoreReady ready,
      std::optional<bool> original_legacy_website_context,
      bool restored);
  // Reuses the active acknowledged registration for one more site. Only the
  // site policy is committed and ACKed; the listener is never revoked.
  void OnActiveTestProfileSiteRestored(
      AccessSiteHost host,
      OwnershipKey owner,
      std::string registration_id,
      std::string group,
      std::optional<bool> original_legacy_website_context,
      bool restored);
  void ReconcileCommittedSiteAfterTestPublication(
      const AccessSiteHost& host,
      const OwnershipKey& owner,
      std::optional<bool> original_legacy_website_context,
      std::string failure_reason = {});
  bool HasAcknowledgedCommittedLegacySite(const AccessSiteHost& host,
                                          bool website_context) const;
  bool RegisterTestProfileForPublication(
      AccessOptionsService& options, const OwnershipKey& owner,
      const aegis_access::GenerationTuple& common, uint64_t revision,
      const AccessLocalCoreReady& ready,
      scoped_refptr<TestProfilePublicationAttempt> attempt = nullptr);
  void RetireTestProfileConsumer(AccessLocalCoreReady ready,
                                  base::OnceCallback<void(bool)> completion);
  std::optional<aegis_access::GenerationTuple> CaptureTestProfilePolicy(
      const OwnershipKey& owner, const std::string& group) const;
  bool IsActiveTestProfileGroup(const OwnershipKey& owner,
      const std::string& group, const aegis_access::GenerationTuple& common) const;
  void RestoreTestProfileForRequest(const OwnershipKey& owner,
      const std::string& group, base::OnceCallback<void(bool)> completion);
  void OnSavedTestProfilePrepared(uint64_t attempt, AccessLocalCoreReady ready);
  void OnSavedTestProfilePublished(uint64_t attempt, uint64_t revision,
      AccessLocalCoreReady ready, bool acknowledged);
  void FinishSavedTestProfileRestore(bool ready);
  struct SavedTestProfileRestore {
    OwnershipKey owner;
    std::string group;
    aegis_access::GenerationTuple common;
    std::vector<base::OnceCallback<void(bool)>> waiters;
    base::TimeTicks publication_deadline;
  };
  std::optional<SavedTestProfileRestore> saved_test_profile_restore_;
  uint64_t saved_test_profile_attempt_ = 0;
  raw_ptr<const base::TickClock> saved_test_profile_publication_clock_ =
      base::DefaultTickClock::GetInstance();
  base::OneShotTimer saved_test_profile_publication_timer_;
  bool test_profile_revoker_installed_ = false;
  std::map<AccessSiteHost, std::string> test_profile_sites_;
#endif
  bool RegisterPreparedCore(const OwnershipKey& owner,
                            const AccessLocalCoreReady& result);
  bool IsCurrentPreparedCore(const OwnershipKey& owner,
                             const AccessLocalCoreReady& result) const;
  void OnRestoredCorePrepared(OwnershipKey owner, uint64_t attempt,
      AccessLocalCoreReady result);
  void OnRestoredCorePolicyReady(OwnershipKey owner, uint64_t attempt,
      aegis_access::GenerationTuple expected, bool accepted);
  void OnRestoredCoreVerified(OwnershipKey owner, uint64_t attempt,
      AccessLocalCoreReady result);
  void FinishCoreRequests(bool ready);
  void FinishNavigationPreparation(bool ready);
  void OnLocalCoreStopped(std::string registration_id);
  void FailCorePreparation(const AccessSiteHost& host, std::string error);

  AccessSiteProxySessionError EnsureLocalProxyReady(
      const AccessSiteHost& host,
      AccessSiteProxyLocalProxyReadyInfo* out) const;
  void BeginEnablePublication(const AccessSiteHost& host,
      AccessSiteProxyLocalProxyReadyInfo ready,
      base::OnceCallback<void(AccessMutationTransactionResult)> completion = {});
  void BeginDisablePublication(const AccessSiteHost& host, base::OnceClosure on_disabled);
  void OnPublicationComplete(AccessSiteHost host,
                             bool enabling,
                             AccessMutationTransactionResult result);
  std::unique_ptr<AccessRuleStore> OpenSessionStore(
      const OwnershipKey& owner) const;

  const raw_ptr<Profile> profile_;
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  CoreLogShareSession log_share_;
  base::CallbackListSubscription app_terminating_;
#endif
  scoped_refptr<AccessCoreLogStore> core_logs_;
  AccessSiteIntentStore intent_store_;
  AccessSiteIntentStore dev_intent_store_;
  std::set<AccessSiteHost> website_hosts_;
  enum class RuntimeFault { kWait, kUnavailable };
  struct HostState {
    static HostState FromRuntimeFault(RuntimeFault fault);

    AccessSiteConnectionState connection = AccessSiteConnectionState::kIdle;
    std::string failure_reason;
    // A legacy rollback or stopped core can recover with current verification
    // and an exact ACK.
    bool recheck_committed_legacy = false;
    // Keep the host fault through the switch's temporary Connecting state.
    std::optional<RuntimeFault> runtime_fault = std::nullopt;
    std::string operation_failure_reason = {};
  };
  struct BusyToggleNotice {
    AccessSiteHost host;
    uint64_t revision;
  };
  std::map<AccessSiteHost, HostState> host_states_;
  // A rejected toggle only contributes a notice for this presentation revision.
  std::optional<BusyToggleNotice> busy_toggle_notice_;
  uint64_t state_revision_ = 0;
  bool publication_in_flight_ = false;
  std::optional<AccessSiteHost> verifying_host_;
  uint64_t committed_verification_attempt_ = 0;
  std::unique_ptr<AccessLocalCoreManager> local_core_;
  bool local_core_ready_ = false;
  bool restoring_core_ = false;
  uint64_t core_attempt_ = 0;
  std::string core_registration_;
  std::optional<OwnershipKey> core_owner_;
  std::vector<base::OnceCallback<void(bool)>> core_request_waiters_;
  GURL retry_url_;
  base::OnceCallback<void(bool)> retry_completion_;
  base::OnceCallback<void(bool)> queued_retry_;
  base::OnceClosure on_enabled_;
  base::WeakPtrFactory<AccessSiteProxyBubbleController> weak_factory_{this};
};

}  // namespace aegis::access

#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_SITE_PROXY_BUBBLE_CONTROLLER_H_
