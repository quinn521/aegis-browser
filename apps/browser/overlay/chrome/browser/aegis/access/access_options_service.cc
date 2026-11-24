// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_options_service.h"
#include <limits>
#include "base/check.h"
#include "base/task/thread_pool.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/aegis/access/access_service_coordinator.h"
#include "chrome/browser/aegis/access/access_build_channel.h"
#include "chrome/browser/aegis/aegis_profile_support.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"
#if BUILDFLAG(IS_MAC)
#include "base/base64.h"
#include "base/apple/foundation_util.h"
#include "base/containers/span.h"
#include "base/strings/string_number_conversions.h"
#include "base/uuid.h"
#include "crypto/hash.h"
#include "chrome/browser/aegis/access/access_test_node_qr_mac.h"  // nogncheck
#include "components/aegis_access/access_test_node.h"  // nogncheck
#endif

namespace aegis::access {
namespace {

const char kOptionsServiceKey[] = "aegis-access-options-service";

#if BUILDFLAG(IS_MAC)
std::string VaultChannel(Profile* profile) {
  if (profile->IsOffTheRecord() ||
      !CanUseAccessOptionsForCurrentBuild(
          aegis_access::AccessOptionsOperation::kImport)) return {};
  return AccessBuildIdentityForCurrentBuild().product ==
                 aegis_access::AccessProductChannel::kDebug ? "debug" : "alpha";
}
std::string VaultScope(Profile* profile) {
  if (VaultChannel(profile).empty() || !profile->GetPath().IsAbsolute() ||
      profile->GetPath().ReferencesParent()) return {};
  const std::string scope = std::string(base::apple::BaseBundleID()) +
      "\naccess-test-node-v1\n" + profile->GetPath().AsUTF8Unsafe();
  return base::HexEncodeLower(crypto::hash::Sha256(base::as_byte_span(scope)));
}
#endif

}  // namespace
#if BUILDFLAG(IS_MAC)
// Raw text, decoded QR and parsed fields never leave this blocking actor.
// The UI sequence receives only a scoped opaque Keychain reference.
class AccessOptionsWorker {
 public:
  AccessOptionsWorker(aegis_access::AccessBuildIdentity identity,
      base::FilePath path, std::string profile_id, bool is_private,
      std::string channel, std::string scope,
      std::unique_ptr<AccessTestNodeVaultMac::Backend> backend)
      : allows_web_socket_tls_(
            !is_private &&
            identity.product == aegis_access::AccessProductChannel::kDebug &&
            aegis_access::CanUseAccessOptions(
                identity, aegis_access::AccessOptionsOperation::kImport)),
        store_(identity, std::move(path), std::move(profile_id), is_private),
        vault_(std::move(channel), std::move(scope), std::move(backend)) {}
  StoreResult<AccessOptionsSnapshot> ReadOptions() { return store_.OpenAndRead(); }
  AccessLocalCoreReady PrepareCoreForTesting(
      AccessInternalTestCoreBindingMac binding,
      base::RepeatingCallback<AccessLocalCoreReady(
          const AccessInternalTestCoreBindingMac&)> prepare_core) {
    if (!vault_.Read(binding.reference)) {
      return {.error = "The test Profile reference is unavailable."};
    }
    return prepare_core.Run(binding);
  }
  void SetSaveFailureForTesting(AccessOptionsStore::SaveFailurePointForTesting point) {
    store_.SetSaveFailureForTesting(point);
  }
  AccessTestProfileSaveResult Save(std::string editor, uint64_t expected_revision,
      std::string operation_id, AccessTestProfileDraft draft,
      std::shared_ptr<AccessOptionsCommitFence> commit_fence) {
    return SaveDraft(std::move(editor), expected_revision, std::move(operation_id),
                     std::move(draft), std::move(commit_fence), false);
  }
  AccessTestProfileSaveResult SavePreferences(uint64_t expected_revision,
      std::string operation_id, AccessTestProfileDraft draft,
      std::shared_ptr<AccessOptionsCommitFence> commit_fence) {
    return SaveDraft(std::string(), expected_revision, std::move(operation_id),
                     std::move(draft), std::move(commit_fence), true);
  }
  AccessTestProfileSaveResult SaveDraft(std::string editor, uint64_t expected_revision,
      std::string operation_id, AccessTestProfileDraft draft,
      std::shared_ptr<AccessOptionsCommitFence> commit_fence, bool committed_reference) {
    if (unknown_save_)
      return {StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
              std::nullopt, std::nullopt, "previous_operation_uncertain"};
    const auto before = store_.OpenAndRead();
    if (before.status != StoreStatus::kValid)
      return {before.status, AccessOptionsCommitOutcome::kNotCommitted,
              std::nullopt, std::nullopt, "options_unavailable"};
    if (before.value->revision != expected_revision)
      return {StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
              std::nullopt, before.value, "configuration_revision_conflict"};
    // This store currently has one typed test Profile. Retain a scope-wide
    // barrier even when an abandoned older instance references a prior row.
    if (vault_.HasRuntimeReferences())
      return {StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
              std::nullopt, before.value, "profile_runtime_in_use"};
    if (!commit_fence || commit_fence->canceled())
      return {StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
              std::nullopt, std::nullopt, "draft_canceled_before_commit"};
    const auto references = store_.ReadTestProfileSecretReferences();
    if (references.status != StoreStatus::kValid || !references.value)
      return {references.status, AccessOptionsCommitOutcome::kNotCommitted,
              std::nullopt, std::nullopt, "reference_recovery_unavailable"};
    if (vault_.ReconcileReferences(*references.value) != AccessTestNodeVaultStatus::kOk)
      return {StoreStatus::kIoError, AccessOptionsCommitOutcome::kNotCommitted,
              std::nullopt, std::nullopt, "secure_storage_unavailable"};
    const AccessTestNodeSecretReference reference{draft.secret_reference};
    if (committed_reference) {
      const auto& current = before.value->test_profile;
      if (!current || current->configuration.id != draft.id ||
          current->configuration.secret_reference != draft.secret_reference ||
          !vault_.Read(reference))
        return {StoreStatus::kInvalidArgument, AccessOptionsCommitOutcome::kNotCommitted,
                std::nullopt, std::nullopt, "committed_reference_unavailable"};
    } else if (!vault_.Read(reference) ||
               !vault_.HoldForCommit(editor, reference, operation_id)) {
      // Only this editor's staged, still-readable native reference can be held.
      return {StoreStatus::kInvalidArgument, AccessOptionsCommitOutcome::kNotCommitted,
              std::nullopt, std::nullopt, "draft_reference_unavailable"};
    }
    auto result = store_.SaveTestProfile(expected_revision, operation_id, draft, std::move(commit_fence));
    if (result.outcome == AccessOptionsCommitOutcome::kUnknown) {
      unknown_save_ = UnknownSave{expected_revision, operation_id, std::move(draft), *before.value};
    } else {
      vault_.ResolveCommit(operation_id, result.outcome == AccessOptionsCommitOutcome::kCommitted);
    }
    return result;
  }
  AccessTestProfileSaveResult Query(std::string operation_id) {
    const auto opened = store_.Open();
    if (opened != StoreStatus::kValid)
      return {opened, AccessOptionsCommitOutcome::kUnknown,
              std::nullopt, std::nullopt, "options_unavailable"};
    auto result = store_.GetTestProfileOperation(operation_id);
    if (!unknown_save_ || unknown_save_->operation != operation_id) return result;
    const auto& pending = *unknown_save_;
    bool resolved = false;
    if (result.outcome == AccessOptionsCommitOutcome::kCommitted && result.operation &&
        result.operation->expected_revision == pending.expected_revision &&
        result.snapshot && result.snapshot->test_profile &&
        result.snapshot->test_profile->operation_id == operation_id &&
        result.snapshot->test_profile->configuration == pending.draft) {
      resolved = true;
    } else if (result.status == StoreStatus::kMissing && result.snapshot &&
        result.snapshot->revision == pending.before.revision &&
        result.snapshot->configuration == pending.before.configuration) {
      result.outcome = AccessOptionsCommitOutcome::kNotCommitted;
      result.detail = "operation_not_committed";
      resolved = true;
    } else {
      result.outcome = AccessOptionsCommitOutcome::kUnknown;
      result.detail = "operation_result_uncertain";
    }
    if (resolved) {
      vault_.ResolveCommit(operation_id, result.outcome == AccessOptionsCommitOutcome::kCommitted);
      unknown_save_.reset();
    }
    return result;
  }
  TestNodePreviewResult Reject(std::string editor, TestNodePreviewStatus status) {
    return {vault_.CloseEditor(editor) == AccessTestNodeVaultStatus::kOk
                ? status : TestNodePreviewStatus::kSecureStorageUnavailable};
  }
  TestNodePreviewResult Text(std::string editor, std::string share) {
    if (vault_.CloseEditor(editor) != AccessTestNodeVaultStatus::kOk)
      return {TestNodePreviewStatus::kSecureStorageUnavailable};
    auto parsed = aegis_access::ParseAccessTestNodeShare(
        share, allows_web_socket_tls_
                   ? aegis_access::AccessTestNodeImportMode::kAllowWebSocketTls
                   : aegis_access::AccessTestNodeImportMode::kRealityOnly);
    switch (parsed.error) {
      case aegis_access::AccessTestNodeError::kNone:
        return Stage(editor, *parsed.node);
      case aegis_access::AccessTestNodeError::kInvalidShare:
        return {TestNodePreviewStatus::kInvalidShare};
      case aegis_access::AccessTestNodeError::kUnsupportedOutbound:
        return {TestNodePreviewStatus::kUnsupportedOutbound};
    }
  }
  TestNodePreviewResult Qr(std::string editor, std::string encoded) {
    if (vault_.CloseEditor(editor) != AccessTestNodeVaultStatus::kOk)
      return {TestNodePreviewStatus::kSecureStorageUnavailable};
    std::string image;
    if (!base::Base64Decode(encoded, &image))
      return {TestNodePreviewStatus::kInvalidImage};
    auto parsed = DecodeAccessTestNodeQrImage(image);
    switch (parsed.status) {
      case AccessTestNodeQrStatus::kDecoded:
        return Stage(editor, *parsed.node);
      case AccessTestNodeQrStatus::kInvalidShare:
        return {TestNodePreviewStatus::kInvalidShare};
      case AccessTestNodeQrStatus::kUnsupportedOutbound:
        return {TestNodePreviewStatus::kUnsupportedOutbound};
      case AccessTestNodeQrStatus::kInvalidImage:
        return {TestNodePreviewStatus::kInvalidImage};
      case AccessTestNodeQrStatus::kNoQrCode:
        return {TestNodePreviewStatus::kNoQrCode};
      case AccessTestNodeQrStatus::kMultipleQrCodes:
        return {TestNodePreviewStatus::kMultipleQrCodes};
    }
  }
  void CloseEditor(std::string editor) { vault_.CloseEditor(editor); }
  AccessTestNodeVaultStatus DiscardDraft(std::string editor) {
    return vault_.CloseEditor(editor);
  }
  void Discard(std::string editor, AccessTestNodeSecretReference reference) {
    vault_.Discard(editor, reference);
  }
 private:
  struct UnknownSave {
    uint64_t expected_revision;
    std::string operation;
    AccessTestProfileDraft draft;
    AccessOptionsSnapshot before;
  };
  TestNodePreviewResult Stage(const std::string& editor,
                              const aegis_access::AccessTestNode& node) {
    auto staged = vault_.Stage(editor, node);
    if (staged.status == AccessTestNodeVaultStatus::kOk)
      return {TestNodePreviewStatus::kPreview, std::move(staged.reference),
              node.web_socket_tls.has_value()};
    return {staged.status == AccessTestNodeVaultStatus::kLimitReached
                ? TestNodePreviewStatus::kBusy
                : TestNodePreviewStatus::kSecureStorageUnavailable};
  }
  const bool allows_web_socket_tls_;
  AccessOptionsStore store_;
  AccessTestNodeVaultMac vault_;
  std::optional<UnknownSave> unknown_save_;
};
#endif
AccessOptionsService* AccessOptionsService::GetOrCreate(Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile || !aegis::IsAegisProfileSupported(profile) ||
      !CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kRead))
    return nullptr;
  auto* service = static_cast<AccessOptionsService*>(profile->GetUserData(kOptionsServiceKey));
  if (!service) {
    auto owned = std::unique_ptr<AccessOptionsService>(new AccessOptionsService(profile));
    service = owned.get();
    profile->SetUserData(kOptionsServiceKey, std::move(owned));
  }
  return service;
}
AccessOptionsService::AccessOptionsService(Profile* profile
#if BUILDFLAG(IS_MAC)
    , std::unique_ptr<AccessTestNodeVaultMac::Backend> backend
#endif
    )
    : profile_(profile)
#if !BUILDFLAG(IS_MAC)
      , store_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE}),
          AccessBuildIdentityForCurrentBuild(), profile->GetPath(),
          "aegis-access-profile:" + profile->GetPath().AsUTF8Unsafe(),
          profile->IsOffTheRecord())
#endif
#if BUILDFLAG(IS_MAC)
      , worker_(base::ThreadPool::CreateSequencedTaskRunner(
            {base::MayBlock(), base::TaskPriority::USER_VISIBLE}),
            AccessBuildIdentityForCurrentBuild(), profile->GetPath(),
            "aegis-access-profile:" + profile->GetPath().AsUTF8Unsafe(),
            profile->IsOffTheRecord(), VaultChannel(profile), VaultScope(profile), std::move(backend)),
        provisional_test_profile_id_(base::Uuid::GenerateRandomV4().AsLowercaseString())
#endif
      {}
AccessOptionsService::~AccessOptionsService() {
#if BUILDFLAG(IS_MAC)
  if (test_profile_commit_fence_) test_profile_commit_fence_->Cancel();
#endif
}
bool AccessOptionsService::Admitted(aegis_access::AccessOptionsOperation operation) const {
  return CanUseAccessOptionsForCurrentBuild(operation);
}
void AccessOptionsService::Read(
    base::OnceCallback<void(StoreResult<AccessOptionsSnapshot>)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!Admitted(aegis_access::AccessOptionsOperation::kRead)) {
    std::move(callback).Run({StoreStatus::kConflict, std::nullopt, "channel_not_admitted"});
    return;
  }
  auto* product = AccessServiceCoordinator::GetOrCreate(profile_);
  if (!product) {
    std::move(callback).Run({StoreStatus::kRecoveryRequired, std::nullopt, "product_store_missing"});
    return;
  }
  product->CheckOptionsStoreBinding(base::BindOnce(&AccessOptionsService::OnProductStoreChecked,
      weak_factory_.GetWeakPtr(), std::move(callback)));
}
void AccessOptionsService::OnProductStoreChecked(
    base::OnceCallback<void(StoreResult<AccessOptionsSnapshot>)> callback, StoreStatus status) {
  if (status != StoreStatus::kValid) {
    std::move(callback).Run({status, std::nullopt, "product_store_not_ready"});
    return;
  }
#if BUILDFLAG(IS_MAC)
  worker_.AsyncCall(&AccessOptionsWorker::ReadOptions).Then(std::move(callback));
#else
  store_.AsyncCall(&AccessOptionsStore::OpenAndRead).Then(std::move(callback));
#endif
}
#if BUILDFLAG(IS_MAC)
TestProfilePublicationAttempt::TestProfilePublicationAttempt() = default;
TestProfilePublicationAttempt::~TestProfilePublicationAttempt() = default;

bool AccessOptionsService::CanPreviewTestNode() const {
  return !profile_->IsOffTheRecord() &&
         Admitted(aegis_access::AccessOptionsOperation::kImport);
}

std::unique_ptr<AccessOptionsService> AccessOptionsService::CreateForTesting(
    Profile* profile, std::unique_ptr<AccessTestNodeVaultMac::Backend> backend) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return std::unique_ptr<AccessOptionsService>(
      new AccessOptionsService(profile, std::move(backend)));
}

AccessOptionsService* AccessOptionsService::InstallForTesting(
    Profile* profile, std::unique_ptr<AccessTestNodeVaultMac::Backend> backend,
    base::RepeatingCallback<AccessLocalCoreReady(
        const AccessInternalTestCoreBindingMac&)> prepare_core) {
  CHECK(backend);
  CHECK(prepare_core);
  CHECK(!profile->GetUserData(kOptionsServiceKey));
  auto owned = CreateForTesting(profile, std::move(backend));
  auto* service = owned.get();
  service->prepare_core_for_testing_ = std::move(prepare_core);
  profile->SetUserData(kOptionsServiceKey, std::move(owned));
  return service;
}

base::WeakPtr<AccessOptionsService> AccessOptionsService::GetWeakPtr() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return weak_factory_.GetWeakPtr();
}
std::optional<std::string> AccessOptionsService::BeginTestNodeEditor() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanPreviewTestNode() || editors_.size() >= 16) return std::nullopt;
  auto id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  editors_.emplace(id, Editor{});
  return id;
}
void AccessOptionsService::CancelTestNodePreview(const std::string& editor) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto found = editors_.find(editor);
  if (found == editors_.end()) return;
  found->second.canceled = true;
  if (editor == test_profile_save_editor_ && test_profile_commit_fence_)
    test_profile_commit_fence_->Cancel();
  found->second.reference.reset();
  if (found->second.generation != std::numeric_limits<uint64_t>::max())
    ++found->second.generation;
  worker_.AsyncCall(&AccessOptionsWorker::CloseEditor)
      .WithArgs(editor);
}
void AccessOptionsService::EndTestNodeEditor(const std::string& editor) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (editor == test_profile_save_editor_ && test_profile_commit_fence_)
    test_profile_commit_fence_->Cancel();
  if (!editors_.erase(editor)) return;
  worker_.AsyncCall(&AccessOptionsWorker::CloseEditor)
      .WithArgs(editor);
}
void AccessOptionsService::DiscardTestNodeDraft(const std::string& editor,
    base::OnceCallback<void(TestNodePreviewStatus)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanPreviewTestNode() || !editors_.contains(editor)) {
    std::move(callback).Run(TestNodePreviewStatus::kNotAdmitted);
    return;
  }
  if (editors_.at(editor).saving) {
    std::move(callback).Run(TestNodePreviewStatus::kBusy);
    return;
  }
  CancelTestNodePreview(editor);
  worker_.AsyncCall(&AccessOptionsWorker::DiscardDraft)
      .WithArgs(editor)
      .Then(base::BindOnce([](base::OnceCallback<void(TestNodePreviewStatus)> cb,
                             AccessTestNodeVaultStatus status) {
        std::move(cb).Run(status == AccessTestNodeVaultStatus::kOk
                             ? TestNodePreviewStatus::kCanceled
                             : TestNodePreviewStatus::kSecureStorageUnavailable);
      }, std::move(callback)));
}
void AccessOptionsService::DiscardTestNodeReference(
    const std::string& editor, AccessTestNodeSecretReference reference) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  worker_.AsyncCall(&AccessOptionsWorker::Discard)
      .WithArgs(editor, std::move(reference));
}
std::optional<uint64_t> AccessOptionsService::StartTestNodePreview(
    const std::string& editor,
    base::OnceCallback<void(TestNodePreviewResult)>& callback) {
  const auto found = editors_.find(editor);
  if (!CanPreviewTestNode() || found == editors_.end()) {
    std::move(callback).Run({TestNodePreviewStatus::kNotAdmitted});
    return std::nullopt;
  }
  if (found->second.pending || found->second.saving || pending_previews_ >= 4 ||
      found->second.generation == std::numeric_limits<uint64_t>::max()) {
    std::move(callback).Run({TestNodePreviewStatus::kBusy});
    return std::nullopt;
  }
  found->second.pending = true;
  found->second.canceled = false;
  found->second.reference.reset();
  ++pending_previews_;
  return ++found->second.generation;
}
void AccessOptionsService::OnTestNodePreview(
    const std::string& editor, uint64_t generation,
    base::OnceCallback<void(TestNodePreviewResult)> callback,
    TestNodePreviewResult preview) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  --pending_previews_;
  auto found = editors_.find(editor);
  if (found != editors_.end()) found->second.pending = false;
  if (!CanPreviewTestNode() || found == editors_.end() ||
      found->second.canceled ||
      found->second.generation != generation) {
    if (preview.reference) DiscardTestNodeReference(editor, *preview.reference);
    std::move(callback).Run({TestNodePreviewStatus::kCanceled});
    return;
  }
  found->second.reference = preview.reference;
  std::move(callback).Run(std::move(preview));
}
void AccessOptionsService::PreviewTestNodeText(
    const std::string& editor, std::string_view share,
    base::OnceCallback<void(TestNodePreviewResult)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto generation = StartTestNodePreview(editor, callback);
  if (!generation) return;
  if (share.size() > aegis_access::kMaxAccessTestNodeShareBytes) {
    worker_.AsyncCall(&AccessOptionsWorker::Reject)
        .WithArgs(editor, TestNodePreviewStatus::kInvalidShare)
        .Then(base::BindOnce(&AccessOptionsService::OnTestNodePreview,
            weak_factory_.GetWeakPtr(), editor, *generation, std::move(callback)));
    return;
  }
  worker_.AsyncCall(&AccessOptionsWorker::Text)
      .WithArgs(editor, std::string(share))
      .Then(base::BindOnce(&AccessOptionsService::OnTestNodePreview,
          weak_factory_.GetWeakPtr(), editor, *generation, std::move(callback)));
}

void AccessOptionsService::PreviewTestNodeQr(
    const std::string& editor,
    std::string_view encoded_image,
    base::OnceCallback<void(TestNodePreviewResult)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto generation = StartTestNodePreview(editor, callback);
  if (!generation) return;
  constexpr size_t kMaxEncodedBytes =
      ((kMaxAccessTestNodeQrImageBytes + 2) / 3) * 4;
  if (encoded_image.empty() || encoded_image.size() > kMaxEncodedBytes) {
    worker_.AsyncCall(&AccessOptionsWorker::Reject)
        .WithArgs(editor, TestNodePreviewStatus::kInvalidImage)
        .Then(base::BindOnce(&AccessOptionsService::OnTestNodePreview,
            weak_factory_.GetWeakPtr(), editor, *generation, std::move(callback)));
    return;
  }
  worker_.AsyncCall(&AccessOptionsWorker::Qr)
      .WithArgs(editor, std::string(encoded_image))
      .Then(base::BindOnce(&AccessOptionsService::OnTestNodePreview,
          weak_factory_.GetWeakPtr(), editor, *generation, std::move(callback)));
}

void AccessOptionsService::SaveTestNodeProfile(const std::string& editor,
    uint64_t expected_revision, std::string operation_id, std::string name,
    aegis_access::AccessInboundPreferences inbound,
    base::OnceCallback<void(AccessTestProfileSaveResult)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto found = editors_.find(editor);
  if (!CanPreviewTestNode() || !Admitted(aegis_access::AccessOptionsOperation::kWrite) ||
      found == editors_.end()) {
    std::move(callback).Run({StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
                            std::nullopt, std::nullopt, "test_profile_not_admitted"});
    return;
  }
  if (test_profile_core_ ||
      (test_profile_runtime_state_ != AccessTestProfileRuntimeState::kIdle &&
       test_profile_runtime_state_ != AccessTestProfileRuntimeState::kUnavailable)) {
    std::move(callback).Run({StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
                            std::nullopt, std::nullopt, "profile_runtime_in_use"});
    return;
  }
  if (!test_profile_save_operation_.empty() || found->second.pending || found->second.saving) {
    std::move(callback).Run({StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
                            std::nullopt, std::nullopt, "operation_in_progress"});
    return;
  }
  if (found->second.canceled || !found->second.reference ||
      expected_revision >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      !base::Uuid::ParseLowercase(operation_id).is_valid() ||
      !IsValidAccessTestProfileDraft({provisional_test_profile_id_, name,
                                     found->second.reference->id, inbound})) {
    std::move(callback).Run({StoreStatus::kInvalidArgument, AccessOptionsCommitOutcome::kNotCommitted,
                            std::nullopt, std::nullopt, "invalid_test_profile_draft"});
    return;
  }
  found->second.saving = true;
  test_profile_save_operation_ = operation_id;
  test_profile_save_editor_ = editor;
  test_profile_save_uncertain_ = false;
  test_profile_commit_fence_ = std::make_shared<AccessOptionsCommitFence>();
  Read(base::BindOnce(&AccessOptionsService::OnTestProfileSaveRead,
      weak_factory_.GetWeakPtr(), editor, found->second.generation, expected_revision,
      std::move(operation_id), std::move(name), inbound, std::move(callback)));
}

void AccessOptionsService::SaveTestNodePreferences(const std::string& editor,
    uint64_t expected_revision, std::string operation_id,
    aegis_access::AccessInboundPreferences inbound,
    base::OnceCallback<void(AccessTestProfileSaveResult)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto found = editors_.find(editor);
  if (!CanPreviewTestNode() || !Admitted(aegis_access::AccessOptionsOperation::kWrite) ||
      found == editors_.end()) {
    std::move(callback).Run({StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
                            std::nullopt, std::nullopt, "test_profile_not_admitted"});
    return;
  }
  if (test_profile_core_ ||
      (test_profile_runtime_state_ != AccessTestProfileRuntimeState::kIdle &&
       test_profile_runtime_state_ != AccessTestProfileRuntimeState::kUnavailable)) {
    std::move(callback).Run({StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
                            std::nullopt, std::nullopt, "profile_runtime_in_use"});
    return;
  }
  if (!test_profile_save_operation_.empty() || found->second.pending || found->second.saving) {
    std::move(callback).Run({StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
                            std::nullopt, std::nullopt, "operation_in_progress"});
    return;
  }
  if (found->second.reference ||
      expected_revision >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      !base::Uuid::ParseLowercase(operation_id).is_valid() ||
      aegis_access::ValidateAccessInboundPreferences(inbound) !=
          aegis_access::AccessInboundPreferenceError::kNone) {
    std::move(callback).Run({StoreStatus::kInvalidArgument, AccessOptionsCommitOutcome::kNotCommitted,
                            std::nullopt, std::nullopt, "invalid_test_profile_draft"});
    return;
  }
  found->second.saving = true;
  test_profile_save_operation_ = operation_id;
  test_profile_save_editor_ = editor;
  test_profile_save_uncertain_ = false;
  test_profile_commit_fence_ = std::make_shared<AccessOptionsCommitFence>();
  Read(base::BindOnce(&AccessOptionsService::OnTestProfilePreferencesRead,
      weak_factory_.GetWeakPtr(), editor, found->second.generation, expected_revision,
      std::move(operation_id), inbound, std::move(callback)));
}

AccessTestProfileRuntimeState AccessOptionsService::test_profile_runtime_state() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return test_profile_runtime_state_;
}
AccessLocalCoreReady AccessOptionsService::test_profile_runtime() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto result = test_profile_ready_;
  result.ready = result.ready && test_profile_runtime_state_ == AccessTestProfileRuntimeState::kActive;
  return result;
}

std::optional<std::pair<uint64_t, AccessLocalCoreReady>>
AccessOptionsService::PreparedTestNode() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (test_profile_runtime_state_ != AccessTestProfileRuntimeState::kPrepared ||
      !test_profile_ready_.ready || !CanPreviewTestNode() ||
      !Admitted(aegis_access::AccessOptionsOperation::kActivate)) {
    return std::nullopt;
  }
  return std::pair(test_profile_runtime_revision_, test_profile_ready_);
}

void AccessOptionsService::PrepareTestNodeProfile(uint64_t expected_revision,
    base::OnceCallback<void(AccessLocalCoreReady)> callback) {
  StartTestProfilePreparation(expected_revision, std::move(callback));
}

void AccessOptionsService::PrepareSavedTestNodeProfile(
    std::string profile_id, uint64_t version,
    base::OnceCallback<void(AccessLocalCoreReady)> callback) {
  StartTestProfilePreparation(SavedTestProfileBinding{std::move(profile_id), version},
                              std::move(callback));
}

bool AccessOptionsService::CanRestoreSavedTestNodeProfile() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return !test_profile_cold_preparation_consumed_ &&
      test_profile_runtime_state_ == AccessTestProfileRuntimeState::kIdle &&
      test_profile_save_operation_.empty() && profile_->IsRegularProfile() &&
      CanPreviewTestNode() && Admitted(aegis_access::AccessOptionsOperation::kActivate);
}

void AccessOptionsService::StartTestProfilePreparation(
    TestProfileExpectation expectation,
    base::OnceCallback<void(AccessLocalCoreReady)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto* saved = std::get_if<SavedTestProfileBinding>(&expectation);
  const uint64_t version_or_revision = saved ? saved->version : std::get<uint64_t>(expectation);
  if (!CanPreviewTestNode() || !Admitted(aegis_access::AccessOptionsOperation::kActivate) ||
      version_or_revision == 0 ||
      version_or_revision > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      (saved && (!profile_->IsRegularProfile() ||
                 !base::Uuid::ParseLowercase(saved->id).is_valid()))) {
    std::move(callback).Run({.error = "This Profile cannot activate a manual test node."});
    return;
  }
  if (saved && !CanRestoreSavedTestNodeProfile()) {
    std::move(callback).Run({.error = "The saved test Profile requires explicit preparation."});
    return;
  }
  if (!test_profile_save_operation_.empty() || test_profile_core_ ||
      (test_profile_runtime_state_ != AccessTestProfileRuntimeState::kIdle &&
       test_profile_runtime_state_ != AccessTestProfileRuntimeState::kUnavailable) ||
      test_profile_runtime_epoch_ == std::numeric_limits<uint64_t>::max() ||
      test_profile_authorization_generation_ == std::numeric_limits<uint64_t>::max()) {
    std::move(callback).Run({.error = "Resolve the current operation or revoke its consumers first."});
    return;
  }
  if (!saved) {
    test_profile_cold_preparation_consumed_ = true;
  }
  // Own the fence before the asynchronous store-binding/read starts. A second
  // window cannot save or prepare between the snapshot and reference reserve.
  test_profile_runtime_state_ = AccessTestProfileRuntimeState::kPreparing;
  const auto epoch = ++test_profile_runtime_epoch_;
  test_profile_prepare_completion_ = std::move(callback);
  test_profile_preparation_deadline_ = base::TimeTicks::Now() + base::Seconds(25);
  test_profile_runtime_timer_.Start(FROM_HERE, base::Seconds(25), base::BindOnce(
      &AccessOptionsService::OnTestProfilePreparationTimedOut, weak_factory_.GetWeakPtr(), epoch));
  Read(base::BindOnce(&AccessOptionsService::OnTestProfileRuntimeRead,
      weak_factory_.GetWeakPtr(), epoch, std::move(expectation)));
}
void AccessOptionsService::OnTestProfileRuntimeRead(uint64_t epoch, TestProfileExpectation expectation,
    StoreResult<AccessOptionsSnapshot> snapshot) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (epoch != test_profile_runtime_epoch_ ||
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kPreparing) return;
  auto fail = [&](std::string message,
                  AccessTestProfileRuntimeState final_state =
                      AccessTestProfileRuntimeState::kUnavailable) {
    test_profile_runtime_timer_.Stop();
    test_profile_runtime_state_ = final_state;
    if (test_profile_prepare_completion_)
      std::move(test_profile_prepare_completion_).Run({.error = std::move(message)});
  };
  if (base::TimeTicks::Now() >= test_profile_preparation_deadline_) {
    fail("The original preparation deadline has expired.");
    return;
  }
  if (!CanPreviewTestNode() || !Admitted(aegis_access::AccessOptionsOperation::kActivate) ||
      !test_profile_save_operation_.empty() || snapshot.status != StoreStatus::kValid ||
      !snapshot.value || !snapshot.value->test_profile) {
    fail("The committed test Profile or its revision is unavailable.");
    return;
  }
  const auto& stored = *snapshot.value->test_profile;
  const auto* saved = std::get_if<SavedTestProfileBinding>(&expectation);
  if (saved ? (saved->id != stored.configuration.id || saved->version != stored.version)
            : std::get<uint64_t>(expectation) != snapshot.value->revision) {
    fail("The committed test Profile does not match the requested binding.",
         saved ? AccessTestProfileRuntimeState::kIdle
               : AccessTestProfileRuntimeState::kUnavailable);
    return;
  }
  using Protocol = aegis_access::AccessInboundProtocolPreference;
  using Transport = aegis_access::RegisteredProxyTransport;
  if (stored.configuration.inbound.protocol != Protocol::kHttp &&
      stored.configuration.inbound.protocol != Protocol::kSocks5) {
    fail("Automatic protocol selection is not admitted in this candidate. Choose an explicit internal protocol.");
    return;
  }
  if (saved) {
    test_profile_cold_preparation_consumed_ = true;
  }
  const auto transport = stored.configuration.inbound.protocol == Protocol::kHttp
      ? Transport::kHttp : Transport::kSocks5;
  AccessInternalTestCoreBindingMac binding{stored.configuration.id, stored.version,
      ++test_profile_authorization_generation_, VaultChannel(profile_), VaultScope(profile_),
      {stored.configuration.secret_reference}, stored.configuration.inbound, transport};
  auto input = prepare_core_for_testing_
      ? std::nullopt : ResolveAccessInternalTestCoreInput(std::move(binding));
  if (!input && !prepare_core_for_testing_) {
    fail("The trusted native core or controlled HTTPS probe is unavailable for this build.");
    return;
  }
  test_profile_runtime_revision_ = snapshot.value->revision;
  test_profile_ready_ = {};
  test_profile_ready_.profile_id = stored.configuration.id;
  test_profile_ready_.profile_version = stored.version;
  test_profile_ready_.authorization_generation = test_profile_authorization_generation_;
  test_profile_ready_.transport = transport;
  auto completion = base::BindOnce(&AccessOptionsService::OnTestProfileCorePrepared,
                                   weak_factory_.GetWeakPtr(), epoch);
  if (prepare_core_for_testing_) {
    worker_.AsyncCall(&AccessOptionsWorker::PrepareCoreForTesting)
        .WithArgs(std::move(binding), prepare_core_for_testing_)
        .Then(std::move(completion));
    return;
  }
  input->internal_preparation_deadline = test_profile_preparation_deadline_;
  test_profile_core_ = std::make_unique<AccessLocalCoreManager>(std::move(*input));
  test_profile_core_->SetStoppedCallback(base::BindRepeating(
      &AccessOptionsService::OnTestProfileCoreStopped, weak_factory_.GetWeakPtr()));
  test_profile_core_->Prepare(std::move(completion));
}
void AccessOptionsService::OnTestProfileCorePrepared(uint64_t epoch, AccessLocalCoreReady ready) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (epoch != test_profile_runtime_epoch_ ||
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kPreparing) return;
  test_profile_runtime_timer_.Stop();
  auto callback = std::move(test_profile_prepare_completion_);
  if (base::TimeTicks::Now() >= test_profile_preparation_deadline_) {
    ready.ready = false;
    ready.error = "The original preparation deadline has expired.";
  }
  if (ready.ready && (!ready.no_local_auth || !ready.parent_listener_lease_held ||
      ready.local_credentials || ready.retirement_pending ||
      ready.port == 0 || ready.registration_id.empty() ||
      ready.listener_socket_generation == 0 || ready.profile_id != test_profile_ready_.profile_id ||
      ready.profile_version != test_profile_ready_.profile_version ||
      ready.authorization_generation != test_profile_ready_.authorization_generation ||
      ready.transport != test_profile_ready_.transport)) {
    ready.ready = false;
    ready.error = "The prepared listener does not match this committed Profile.";
  }
  test_profile_ready_ = ready;
  test_profile_runtime_state_ = ready.ready ? AccessTestProfileRuntimeState::kPrepared
                                          : AccessTestProfileRuntimeState::kUnavailable;
  if (!ready.ready) {
    // No consumer has been authorized yet; cleanup still uses the confirmed
    // native stop path and retains unknown retirement behind the same fence.
    RevokeTestNodeProfile(base::BindOnce([](bool) {}));
  }
  if (callback) std::move(callback).Run(std::move(ready));
}
void AccessOptionsService::OnTestProfilePreparationTimedOut(uint64_t epoch) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (epoch != test_profile_runtime_epoch_ ||
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kPreparing) return;
  auto callback = std::move(test_profile_prepare_completion_);
  RevokeTestNodeProfile(base::BindOnce([](bool) {}));
  if (callback) std::move(callback).Run({.error = "The original preparation deadline has expired."});
}
bool AccessOptionsService::BeginTestNodePublication(
    uint64_t revision, const AccessLocalCoreReady& ready,
    scoped_refptr<TestProfilePublicationAttempt> attempt) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanPreviewTestNode() || !Admitted(aegis_access::AccessOptionsOperation::kActivate) ||
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kPrepared ||
      !MatchesPreparedTestNode(revision, ready) ||
      !test_profile_consumer_revoker_ || test_profile_consumer_revoker_.IsCancelled()) return false;
  // Set before publication: an unknown or late ACK may
  // already have installed a consumer and therefore cannot release the port.
  test_profile_consumers_possible_ = true;
  test_profile_publication_attempt_ = std::move(attempt);
  test_profile_runtime_state_ = AccessTestProfileRuntimeState::kPublishing;
  return true;
}
bool AccessOptionsService::CompleteTestNodePublication(uint64_t revision,
    const AccessLocalCoreReady& ready, bool acknowledged) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanPreviewTestNode() || !Admitted(aegis_access::AccessOptionsOperation::kActivate) ||
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kPublishing ||
      !MatchesPreparedTestNode(revision, ready)) return false;
  test_profile_publication_attempt_.reset();
  test_profile_runtime_state_ = acknowledged ? AccessTestProfileRuntimeState::kActive
                                            : AccessTestProfileRuntimeState::kUncertain;
  return true;
}

bool AccessOptionsService::MatchesPreparedTestNode(
    uint64_t revision, const AccessLocalCoreReady& ready) const {
  return (test_profile_core_ || prepare_core_for_testing_) &&
      ready.ready && ready.no_local_auth && ready.parent_listener_lease_held &&
      !ready.local_credentials && !ready.retirement_pending &&
      revision == test_profile_runtime_revision_ &&
      ready.registration_id == test_profile_ready_.registration_id &&
      ready.port == test_profile_ready_.port &&
      ready.profile_id == test_profile_ready_.profile_id &&
      ready.profile_version == test_profile_ready_.profile_version &&
      ready.transport == test_profile_ready_.transport &&
      ready.authorization_generation == test_profile_ready_.authorization_generation &&
      ready.listener_socket_generation == test_profile_ready_.listener_socket_generation;
}

void AccessOptionsService::RevokePreparedTestNodeIfCurrent(
    uint64_t revision, const AccessLocalCoreReady& ready) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if ((test_profile_runtime_state_ == AccessTestProfileRuntimeState::kPrepared ||
       test_profile_runtime_state_ == AccessTestProfileRuntimeState::kPublishing) &&
      MatchesPreparedTestNode(revision, ready)) {
    RevokeTestNodeProfile(base::DoNothing());
  }
}
bool AccessOptionsService::RevokeTestNodeProfileAfterNetworkLoss(
    const std::string& registration_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (registration_id.empty() || registration_id != test_profile_ready_.registration_id ||
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kActive ||
      !profile_->IsRegularProfile()) {
    return false;
  }
  RevokeTestNodeProfile(base::DoNothing());
  // Set after the shared revocation entry, which clears it for every other
  // caller. Retirement normally completes asynchronously and reads it then.
  if (test_profile_runtime_state_ == AccessTestProfileRuntimeState::kRevoking) {
    test_profile_network_loss_restore_ = true;
  } else if (test_profile_runtime_state_ == AccessTestProfileRuntimeState::kIdle) {
    test_profile_cold_preparation_consumed_ = false;
  }
  return true;
}
bool AccessOptionsService::SetTestNodeConsumerRevoker(base::RepeatingCallback<void(
    AccessLocalCoreReady, base::OnceCallback<void(bool)>)> revoker) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!revoker || test_profile_consumer_revoker_) return false;
  test_profile_consumer_revoker_ = std::move(revoker);
  return true;
}
void AccessOptionsService::OnTestProfileCoreStopped(std::string registration) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (registration.empty() || registration != test_profile_ready_.registration_id) return;
  if (test_profile_runtime_state_ == AccessTestProfileRuntimeState::kRevoking) return;
  test_profile_runtime_state_ = AccessTestProfileRuntimeState::kUnavailable;
  RevokeTestNodeProfile(base::BindOnce([](bool) {}));
}
void AccessOptionsService::RevokeTestNodeProfile(base::OnceCallback<void(bool)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  test_profile_publication_attempt_.reset();
  test_profile_cold_preparation_consumed_ = true;
  test_profile_network_loss_restore_ = false;
  test_profile_revoke_waiters_.push_back(std::move(callback));
  if (test_profile_runtime_state_ == AccessTestProfileRuntimeState::kRevoking) return;
  if (test_profile_core_) test_profile_core_->StopForwarding();
  if (test_profile_runtime_epoch_ == std::numeric_limits<uint64_t>::max()) {
    test_profile_runtime_timer_.Stop();
    test_profile_runtime_state_ = AccessTestProfileRuntimeState::kUncertain;
    if (test_profile_prepare_completion_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
          std::move(test_profile_prepare_completion_),
          AccessLocalCoreReady{.error = "The native preparation generation is exhausted."}));
    }
    auto callbacks = std::move(test_profile_revoke_waiters_);
    test_profile_revoke_waiters_.clear();
    for (auto& pending : callbacks) std::move(pending).Run(false);
    return;
  }
  test_profile_runtime_state_ = AccessTestProfileRuntimeState::kRevoking;
  const auto epoch = ++test_profile_runtime_epoch_;
  test_profile_runtime_timer_.Stop();
  if (test_profile_prepare_completion_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(
        std::move(test_profile_prepare_completion_),
        AccessLocalCoreReady{.error = "Test node preparation was canceled."}));
  }
  test_profile_runtime_timer_.Start(FROM_HERE, base::Seconds(25), base::BindOnce(
      &AccessOptionsService::OnTestProfileConsumersRevoked, weak_factory_.GetWeakPtr(), epoch, false));
  if (!test_profile_consumers_possible_) {
    OnTestProfileConsumersRevoked(epoch, true);
  } else if (!test_profile_consumer_revoker_ || test_profile_consumer_revoker_.IsCancelled()) {
    OnTestProfileConsumersRevoked(epoch, false);
  } else {
    test_profile_consumer_revoker_.Run(test_profile_ready_, base::BindOnce(
        &AccessOptionsService::OnTestProfileConsumersRevoked, weak_factory_.GetWeakPtr(), epoch));
  }
}

bool AccessOptionsService::CanRevokeTestNodeProfileAtRevision(uint64_t revision) const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return revision && revision == test_profile_runtime_revision_ &&
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kIdle &&
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kPreparing;
}

bool AccessOptionsService::RevokeTestNodeProfileAtRevision(
    uint64_t revision, base::OnceCallback<void(bool)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanRevokeTestNodeProfileAtRevision(revision)) {
    return false;
  }
  if (test_profile_runtime_state_ == AccessTestProfileRuntimeState::kPublishing &&
      test_profile_publication_attempt_) {
    test_profile_publication_attempt_->explicitly_revoked_ = true;
  }
  RevokeTestNodeProfile(std::move(callback));
  return true;
}
void AccessOptionsService::OnTestProfileConsumersRevoked(uint64_t epoch, bool confirmed) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (epoch != test_profile_runtime_epoch_ ||
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kRevoking) return;
  if (!confirmed || !test_profile_core_) {
    OnTestProfileCoreRetired(epoch, confirmed);
    return;
  }
  test_profile_consumers_possible_ = false;
  test_profile_core_->StopConfirmed(base::BindOnce(&AccessOptionsService::OnTestProfileCoreRetired,
      weak_factory_.GetWeakPtr(), epoch));
}
void AccessOptionsService::OnTestProfileCoreRetired(uint64_t epoch, bool confirmed) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (epoch != test_profile_runtime_epoch_ ||
      test_profile_runtime_state_ != AccessTestProfileRuntimeState::kRevoking) return;
  test_profile_runtime_timer_.Stop();
  test_profile_runtime_state_ = confirmed ? AccessTestProfileRuntimeState::kIdle
                                         : AccessTestProfileRuntimeState::kUncertain;
  if (confirmed) {
    test_profile_core_.reset();
    test_profile_ready_ = {};
    test_profile_runtime_revision_ = 0;
    test_profile_consumers_possible_ = false;
    // Only a confirmed retirement after Network Service loss reopens the
    // single cold restore from the persisted Profile binding.
    if (test_profile_network_loss_restore_) test_profile_cold_preparation_consumed_ = false;
  }
  test_profile_network_loss_restore_ = false;
  auto callbacks = std::move(test_profile_revoke_waiters_);
  test_profile_revoke_waiters_.clear();
  for (auto& pending : callbacks) std::move(pending).Run(confirmed);
}

void AccessOptionsService::SetSaveFailureForTesting(AccessOptionsStore::SaveFailurePointForTesting point) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  worker_.AsyncCall(&AccessOptionsWorker::SetSaveFailureForTesting).WithArgs(point);
}

void AccessOptionsService::OnTestProfileSaveRead(std::string editor, uint64_t generation,
    uint64_t expected_revision, std::string operation_id, std::string name,
    aegis_access::AccessInboundPreferences inbound,
    base::OnceCallback<void(AccessTestProfileSaveResult)> callback,
    StoreResult<AccessOptionsSnapshot> snapshot) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto found = editors_.find(editor);
  if (!CanPreviewTestNode() || found == editors_.end() || found->second.canceled ||
      found->second.generation != generation || !found->second.reference) {
    OnTestProfileSaveComplete(editor, operation_id, std::move(callback),
        {StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
         std::nullopt, std::nullopt, "draft_canceled_before_commit"});
    return;
  }
  if (snapshot.status != StoreStatus::kValid || !snapshot.value ||
      snapshot.value->revision != expected_revision) {
    OnTestProfileSaveComplete(editor, operation_id, std::move(callback),
        {snapshot.status == StoreStatus::kValid ? StoreStatus::kConflict : snapshot.status,
         AccessOptionsCommitOutcome::kNotCommitted, std::nullopt, std::move(snapshot.value),
         "configuration_revision_conflict"});
    return;
  }
  const auto id = snapshot.value->test_profile
      ? snapshot.value->test_profile->configuration.id : provisional_test_profile_id_;
  AccessTestProfileDraft draft{id, std::move(name), found->second.reference->id, inbound};
  worker_.AsyncCall(&AccessOptionsWorker::Save)
      .WithArgs(editor, expected_revision, operation_id, std::move(draft), test_profile_commit_fence_)
      .Then(base::BindOnce(&AccessOptionsService::OnTestProfileSaveComplete,
          weak_factory_.GetWeakPtr(), editor, operation_id, std::move(callback)));
}

void AccessOptionsService::OnTestProfilePreferencesRead(std::string editor,
    uint64_t generation, uint64_t expected_revision, std::string operation_id,
    aegis_access::AccessInboundPreferences inbound,
    base::OnceCallback<void(AccessTestProfileSaveResult)> callback,
    StoreResult<AccessOptionsSnapshot> snapshot) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto found = editors_.find(editor);
  if (!CanPreviewTestNode() || found == editors_.end() ||
      found->second.generation != generation || found->second.reference) {
    OnTestProfileSaveComplete(editor, operation_id, std::move(callback),
        {StoreStatus::kConflict, AccessOptionsCommitOutcome::kNotCommitted,
         std::nullopt, std::nullopt, "draft_canceled_before_commit"});
    return;
  }
  if (snapshot.status != StoreStatus::kValid || !snapshot.value ||
      snapshot.value->revision != expected_revision) {
    OnTestProfileSaveComplete(editor, operation_id, std::move(callback),
        {snapshot.status == StoreStatus::kValid ? StoreStatus::kConflict : snapshot.status,
         AccessOptionsCommitOutcome::kNotCommitted, std::nullopt, std::move(snapshot.value),
         "configuration_revision_conflict"});
    return;
  }
  if (!snapshot.value->test_profile) {
    OnTestProfileSaveComplete(editor, operation_id, std::move(callback),
        {StoreStatus::kInvalidArgument, AccessOptionsCommitOutcome::kNotCommitted,
         std::nullopt, std::move(snapshot.value), "no_saved_test_profile"});
    return;
  }
  auto draft = snapshot.value->test_profile->configuration;
  draft.inbound = inbound;
  worker_.AsyncCall(&AccessOptionsWorker::SavePreferences)
      .WithArgs(expected_revision, operation_id, std::move(draft), test_profile_commit_fence_)
      .Then(base::BindOnce(&AccessOptionsService::OnTestProfileSaveComplete,
          weak_factory_.GetWeakPtr(), editor, operation_id, std::move(callback)));
}

void AccessOptionsService::OnTestProfileSaveComplete(std::string editor,
    std::string operation_id, base::OnceCallback<void(AccessTestProfileSaveResult)> callback,
    AccessTestProfileSaveResult result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (operation_id == test_profile_save_operation_) {
    test_profile_save_uncertain_ = result.outcome == AccessOptionsCommitOutcome::kUnknown;
    const auto found = editors_.find(editor);
    if (found != editors_.end()) {
      found->second.saving = test_profile_save_uncertain_;
      if (result.outcome == AccessOptionsCommitOutcome::kCommitted)
        found->second.reference.reset();
    }
    if (!test_profile_save_uncertain_) {
      test_profile_save_operation_.clear();
      test_profile_save_editor_.clear();
      test_profile_commit_fence_.reset();
    }
  }
  std::move(callback).Run(std::move(result));
}

void AccessOptionsService::QueryTestNodeProfileOperation(std::string operation_id,
    base::OnceCallback<void(AccessTestProfileSaveResult)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!CanPreviewTestNode() || !base::Uuid::ParseLowercase(operation_id).is_valid()) {
    std::move(callback).Run({StoreStatus::kInvalidArgument, AccessOptionsCommitOutcome::kUnknown,
                            std::nullopt, std::nullopt, "operation_not_admitted"});
    return;
  }
  if (operation_id == test_profile_save_operation_ && !test_profile_save_uncertain_) {
    std::move(callback).Run({StoreStatus::kConflict, AccessOptionsCommitOutcome::kUnknown,
                            std::nullopt, std::nullopt, "operation_in_progress"});
    return;
  }
  Read(base::BindOnce(&AccessOptionsService::OnTestProfileQueryRead,
      weak_factory_.GetWeakPtr(), std::move(operation_id), std::move(callback)));
}

bool AccessOptionsService::HasUncertainTestNodeApply() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return test_profile_save_uncertain_ && !test_profile_save_operation_.empty();
}

void AccessOptionsService::ReconcileUncertainTestNodeApply(base::OnceClosure completion) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!HasUncertainTestNodeApply()) {
    std::move(completion).Run();
    return;
  }
  QueryTestNodeProfileOperation(test_profile_save_operation_,
      base::BindOnce([](base::OnceClosure done, AccessTestProfileSaveResult) {
        std::move(done).Run();
      }, std::move(completion)));
}

void AccessOptionsService::OnTestProfileQueryRead(std::string operation_id,
    base::OnceCallback<void(AccessTestProfileSaveResult)> callback,
    StoreResult<AccessOptionsSnapshot> snapshot) {
  if (snapshot.status != StoreStatus::kValid || !CanPreviewTestNode()) {
    std::move(callback).Run({snapshot.status, AccessOptionsCommitOutcome::kUnknown,
                            std::nullopt, std::nullopt, "product_store_not_ready"});
    return;
  }
  const auto editor = operation_id == test_profile_save_operation_ ? test_profile_save_editor_ : "";
  worker_.AsyncCall(&AccessOptionsWorker::Query).WithArgs(operation_id)
      .Then(base::BindOnce(&AccessOptionsService::OnTestProfileSaveComplete,
          weak_factory_.GetWeakPtr(), editor, operation_id, std::move(callback)));
}
#endif

}  // namespace aegis::access
