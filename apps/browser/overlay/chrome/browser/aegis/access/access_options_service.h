// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OPTIONS_SERVICE_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OPTIONS_SERVICE_H_
#include <optional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/supports_user_data.h"
#include "build/build_config.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/timer/timer.h"
#include "chrome/browser/aegis/access/access_options_store.h"
#if BUILDFLAG(IS_MAC)
#include "chrome/browser/aegis/access/access_test_node_vault_mac.h"
#include "chrome/browser/aegis/access/access_local_core_manager.h"
#endif
class Profile;
namespace aegis::access {
#if BUILDFLAG(IS_MAC)
enum class TestNodePreviewStatus {
  kPreview,
  kNotAdmitted,
  kInvalidShare,
  kUnsupportedOutbound,
  kInvalidImage,
  kNoQrCode,
  kMultipleQrCodes,
  kCanceled,
  kBusy,
  kSecureStorageUnavailable,
};
// The editor retains an opaque reference only; node fields stay in the vault.
struct TestNodePreviewResult {
  TestNodePreviewStatus status;
  std::optional<AccessTestNodeSecretReference> reference = std::nullopt;
  bool uses_web_socket_tls = false;
};
class AccessOptionsWorker;
enum class AccessTestProfileRuntimeState {
  kIdle, kPreparing, kPrepared, kPublishing, kActive, kRevoking, kUnavailable, kUncertain,
};

// UI-thread state shared by one manual switch callback and its publication.
class TestProfilePublicationAttempt
    : public base::RefCounted<TestProfilePublicationAttempt> {
 public:
  TestProfilePublicationAttempt();
  bool explicitly_revoked() const { return explicitly_revoked_; }

 private:
  friend class base::RefCounted<TestProfilePublicationAttempt>;
  friend class AccessOptionsService;
  ~TestProfilePublicationAttempt();

  bool explicitly_revoked_ = false;
};
#endif

// Each actual Profile owns a separate actor, including each private Profile.
class AccessOptionsService : public base::SupportsUserData::Data {
 public:
  static AccessOptionsService* GetOrCreate(Profile* profile);
  ~AccessOptionsService() override;
  bool Admitted(aegis_access::AccessOptionsOperation operation) const;
  void Read(base::OnceCallback<void(StoreResult<AccessOptionsSnapshot>)> callback);
#if BUILDFLAG(IS_MAC)
  static std::unique_ptr<AccessOptionsService> CreateForTesting(
      Profile* profile,
      std::unique_ptr<AccessTestNodeVaultMac::Backend> backend);
  // Installs a Profile-owned service with a synthetic Vault and native core.
  // The callback runs on the Vault worker after reading the committed reference.
  static AccessOptionsService* InstallForTesting(
      Profile* profile, std::unique_ptr<AccessTestNodeVaultMac::Backend> backend,
      base::RepeatingCallback<AccessLocalCoreReady(
          const AccessInternalTestCoreBindingMac&)> prepare_core);
  base::WeakPtr<AccessOptionsService> GetWeakPtr();
  std::optional<std::string> BeginTestNodeEditor();
  void CancelTestNodePreview(const std::string& editor);
  void DiscardTestNodeDraft(const std::string& editor,
      base::OnceCallback<void(TestNodePreviewStatus)> callback);
  void EndTestNodeEditor(const std::string& editor);
  void DiscardTestNodeReference(const std::string& editor,
                                AccessTestNodeSecretReference reference);
  void PreviewTestNodeText(
      const std::string& editor,
      std::string_view share,
      base::OnceCallback<void(TestNodePreviewResult)> callback);
  void PreviewTestNodeQr(
      const std::string& editor,
      std::string_view encoded_image,
      base::OnceCallback<void(TestNodePreviewResult)> callback);
  // Intent persistence only; activation has its own runtime/ACK fence. The
  // handler supplies a native editor, never a renderer-chosen secret reference.
  void SaveTestNodeProfile(const std::string& editor, uint64_t expected_revision,
      std::string operation_id, std::string name,
      aegis_access::AccessInboundPreferences inbound,
      base::OnceCallback<void(AccessTestProfileSaveResult)> callback);
  // Saves listener preferences for the committed Profile. The native owner
  // reuses the durable Keychain reference; no new import is staged.
  void SaveTestNodePreferences(const std::string& editor, uint64_t expected_revision,
      std::string operation_id, aegis_access::AccessInboundPreferences inbound,
      base::OnceCallback<void(AccessTestProfileSaveResult)> callback);
  void QueryTestNodeProfileOperation(std::string operation_id,
      base::OnceCallback<void(AccessTestProfileSaveResult)> callback);
  bool HasUncertainTestNodeApply() const;
  void ReconcileUncertainTestNodeApply(base::OnceClosure completion);
  void SetSaveFailureForTesting(AccessOptionsStore::SaveFailurePointForTesting point);
  // Native consumer API. No renderer supplies a node reference, probe, manifest
  // path, endpoint or authorization generation. Prepare fences Save before IO.
  void PrepareTestNodeProfile(uint64_t expected_revision,
      base::OnceCallback<void(AccessLocalCoreReady)> callback);
  bool CanRestoreSavedTestNodeProfile() const;
  void PrepareSavedTestNodeProfile(
      std::string profile_id, uint64_t version,
      base::OnceCallback<void(AccessLocalCoreReady)> callback);
  bool BeginTestNodePublication(
      uint64_t revision, const AccessLocalCoreReady& ready,
      scoped_refptr<TestProfilePublicationAttempt> attempt = nullptr);
  // Reports whether this completion belonged to the in-flight registration.
  bool CompleteTestNodePublication(uint64_t revision,
      const AccessLocalCoreReady& ready, bool acknowledged);
  bool SetTestNodeConsumerRevoker(base::RepeatingCallback<void(
      AccessLocalCoreReady, base::OnceCallback<void(bool)>)> revoker);
  void RevokeTestNodeProfile(base::OnceCallback<void(bool)> callback);
  bool CanRevokeTestNodeProfileAtRevision(uint64_t revision) const;
  bool RevokeTestNodeProfileAtRevision(uint64_t revision,
      base::OnceCallback<void(bool)> callback);
  void RevokePreparedTestNodeIfCurrent(uint64_t revision,
                                      const AccessLocalCoreReady& ready);
  // The private Context that acknowledged |registration_id| was lost. Revokes
  // that listener and, once retirement is confirmed, admits exactly one fresh
  // registration from the saved Profile binding.
  bool RevokeTestNodeProfileAfterNetworkLoss(const std::string& registration_id);
  AccessTestProfileRuntimeState test_profile_runtime_state() const;
  AccessLocalCoreReady test_profile_runtime() const;
  std::optional<std::pair<uint64_t, AccessLocalCoreReady>> PreparedTestNode() const;
#endif

 private:
#if BUILDFLAG(IS_MAC)
  friend class AccessSiteProxyProductTestPeer;

  bool CanPreviewTestNode() const;
  std::optional<uint64_t> StartTestNodePreview(
      const std::string& editor,
      base::OnceCallback<void(TestNodePreviewResult)>& callback);
  void OnTestNodePreview(const std::string& editor, uint64_t generation,
      base::OnceCallback<void(TestNodePreviewResult)> callback,
      TestNodePreviewResult preview);
  // Reports a stale draft as a revision conflict before reporting that the
  // current test Profile runtime blocks the save. |switches_profile| marks a
  // request to activate another Profile, which Alpha rejects while one runs.
  void RejectRuntimeInUseAfterRevisionCheck(
      uint64_t expected_revision, bool switches_profile,
      base::OnceCallback<void(AccessTestProfileSaveResult)> callback);
  void OnTestProfileSaveRead(std::string editor, uint64_t generation,
      uint64_t expected_revision, std::string operation_id, std::string name,
      aegis_access::AccessInboundPreferences inbound,
      base::OnceCallback<void(AccessTestProfileSaveResult)> callback,
      StoreResult<AccessOptionsSnapshot> snapshot);
  void OnTestProfileSaveComplete(std::string editor, std::string operation_id,
      base::OnceCallback<void(AccessTestProfileSaveResult)> callback,
      AccessTestProfileSaveResult result);
  void OnTestProfilePreferencesRead(std::string editor, uint64_t generation,
      uint64_t expected_revision, std::string operation_id,
      aegis_access::AccessInboundPreferences inbound,
      base::OnceCallback<void(AccessTestProfileSaveResult)> callback,
      StoreResult<AccessOptionsSnapshot> snapshot);
  void OnTestProfileQueryRead(std::string operation_id,
      base::OnceCallback<void(AccessTestProfileSaveResult)> callback,
      StoreResult<AccessOptionsSnapshot> snapshot);
  struct SavedTestProfileBinding {
    std::string id;
    uint64_t version;
  };
  using TestProfileExpectation = std::variant<uint64_t, SavedTestProfileBinding>;
  void StartTestProfilePreparation(
      TestProfileExpectation expectation,
      base::OnceCallback<void(AccessLocalCoreReady)> callback);
  void OnTestProfileRuntimeRead(uint64_t epoch, TestProfileExpectation expectation,
      StoreResult<AccessOptionsSnapshot> snapshot);
  bool MatchesPreparedTestNode(uint64_t revision,
                               const AccessLocalCoreReady& ready) const;
  void OnTestProfileCorePrepared(uint64_t epoch, AccessLocalCoreReady ready);
  void OnTestProfilePreparationTimedOut(uint64_t epoch);
  void OnTestProfileCoreStopped(std::string registration);
  void OnTestProfileConsumersRevoked(uint64_t epoch, bool confirmed);
  void OnTestProfileCoreRetired(uint64_t epoch, bool confirmed);
  explicit AccessOptionsService(Profile* profile,
      std::unique_ptr<AccessTestNodeVaultMac::Backend> backend = nullptr);
#else
  explicit AccessOptionsService(Profile* profile);
#endif
  void OnProductStoreChecked(base::OnceCallback<void(StoreResult<AccessOptionsSnapshot>)> callback, StoreStatus status);
  raw_ptr<Profile> profile_;
#if !BUILDFLAG(IS_MAC)
  base::SequenceBound<AccessOptionsStore> store_;
#endif
#if BUILDFLAG(IS_MAC)
  struct Editor {
    uint64_t generation = 0;
    bool pending = false;
    bool canceled = false;
    bool saving = false;
    std::optional<AccessTestNodeSecretReference> reference;
  };
  base::SequenceBound<AccessOptionsWorker> worker_;
  std::map<std::string, Editor> editors_;
  size_t pending_previews_ = 0;
  std::string provisional_test_profile_id_;
  std::string test_profile_save_operation_;
  std::string test_profile_save_editor_;
  bool test_profile_save_uncertain_ = false;
  std::shared_ptr<AccessOptionsCommitFence> test_profile_commit_fence_;
  std::unique_ptr<AccessLocalCoreManager> test_profile_core_;
  base::RepeatingCallback<AccessLocalCoreReady(
      const AccessInternalTestCoreBindingMac&)> prepare_core_for_testing_;
  AccessLocalCoreReady test_profile_ready_;
  AccessTestProfileRuntimeState test_profile_runtime_state_ = AccessTestProfileRuntimeState::kIdle;
  scoped_refptr<TestProfilePublicationAttempt> test_profile_publication_attempt_;
  // A stale saved binding advances the epoch without consuming cold recovery.
  bool test_profile_cold_preparation_consumed_ = false;
  // Set only while a Network Service loss revocation is in flight. Explicit
  // revocation clears it, so the user's choice is never undone.
  bool test_profile_network_loss_restore_ = false;
  uint64_t test_profile_runtime_epoch_ = 0;
  uint64_t test_profile_runtime_revision_ = 0;
  uint64_t test_profile_authorization_generation_ = 0;
  bool test_profile_consumers_possible_ = false;
  base::RepeatingCallback<void(AccessLocalCoreReady, base::OnceCallback<void(bool)>)>
      test_profile_consumer_revoker_;
  std::vector<base::OnceCallback<void(bool)>> test_profile_revoke_waiters_;
  base::OnceCallback<void(AccessLocalCoreReady)> test_profile_prepare_completion_;
  base::TimeTicks test_profile_preparation_deadline_;
  base::OneShotTimer test_profile_runtime_timer_;
#endif
  base::WeakPtrFactory<AccessOptionsService> weak_factory_{this};
};
}  // namespace aegis::access
#endif
