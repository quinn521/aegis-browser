// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_request_dispatch_state.h"

#include <memory>
#include <utility>

#include "base/supports_user_data.h"
#include "chrome/browser/aegis/aegis_profile_support.h"
#include "chrome/browser/profiles/profile.h"

namespace aegis::access {
namespace {

constexpr size_t kMaxDispatchBarriers = 1024;
constexpr size_t kMaxOwnedRequests = 4096;
const void* const kDispatchStateUserDataKey = &kDispatchStateUserDataKey;

class AccessRequestDispatchStateHolder : public base::SupportsUserData::Data {
 public:
  explicit AccessRequestDispatchStateHolder(
      scoped_refptr<AccessRequestDispatchState> state)
      : state_(std::move(state)) {}
  ~AccessRequestDispatchStateHolder() override = default;

  const scoped_refptr<AccessRequestDispatchState>& state() const {
    return state_;
  }

 private:
  scoped_refptr<AccessRequestDispatchState> state_;
};

}  // namespace

// static
scoped_refptr<AccessRequestDispatchState> AccessRequestDispatchState::Get(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  auto* holder = static_cast<AccessRequestDispatchStateHolder*>(
      profile->GetUserData(kDispatchStateUserDataKey));
  return holder ? holder->state() : nullptr;
}

// static
scoped_refptr<AccessRequestDispatchState> AccessRequestDispatchState::GetOrCreate(
    Profile* profile) {
  if (!profile || !aegis::IsAegisProfileSupported(profile)) {
    return nullptr;
  }
  if (scoped_refptr<AccessRequestDispatchState> existing = Get(profile)) {
    return existing;
  }

  auto state = base::MakeRefCounted<AccessRequestDispatchState>();
  profile->SetUserData(
      kDispatchStateUserDataKey,
      std::make_unique<AccessRequestDispatchStateHolder>(state));
  return state;
}

AccessRequestDispatchState::AccessRequestDispatchState()
    : barriers_(kMaxDispatchBarriers), ownership_(kMaxOwnedRequests) {}

AccessRequestDispatchState::~AccessRequestDispatchState() = default;

aegis_access::PublishedRequestRuntimeResult
AccessRequestDispatchState::EvaluateAndRegister(
    const aegis_access::PublishedRequestRuntimeInput& input) {
  base::AutoLock guard(lock_);
  return aegis_access::EvaluatePublishedRequestForDispatch(
      input, barriers_, ownership_);
}

aegis_access::RequestOwnershipLookupResult AccessRequestDispatchState::Lookup(
    const std::string& request_id,
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& generations) {
  base::AutoLock guard(lock_);
  return ownership_.Lookup(request_id, owner, generations);
}

aegis_access::RequestOwnershipStatus AccessRequestDispatchState::MarkDispatched(
    const std::string& request_id,
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& generations,
    std::unique_ptr<aegis_access::RequestTerminationHandle> handle) {
  base::AutoLock guard(lock_);
  return ownership_.MarkDispatched(request_id, owner, generations,
                                   std::move(handle));
}

aegis_access::RequestOwnershipStatus AccessRequestDispatchState::MarkStreaming(
    const std::string& request_id,
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& generations) {
  base::AutoLock guard(lock_);
  return ownership_.MarkStreaming(request_id, owner, generations);
}

aegis_access::RequestOwnershipTerminalResult
AccessRequestDispatchState::Complete(
    const std::string& request_id,
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& generations) {
  base::AutoLock guard(lock_);
  return ownership_.Complete(request_id, owner, generations);
}

aegis_access::RequestOwnershipTerminalResult
AccessRequestDispatchState::Cancel(
    const std::string& request_id,
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& generations) {
  base::AutoLock guard(lock_);
  return ownership_.Cancel(request_id, owner, generations);
}

aegis_access::RequestOwnershipBatchCancelResult
AccessRequestDispatchState::CancelMatchingPageTarget(
    const aegis_access::RequestCancellationSelector& selector) {
  base::AutoLock guard(lock_);
  return ownership_.CancelMatchingPageTarget(selector);
}

aegis_access::RequestDispatchBarrierStatus
AccessRequestDispatchState::InstallBlockBarrier(
    aegis_access::RequestDispatchBarrier barrier) {
  base::AutoLock guard(lock_);
  return barriers_.InstallBlockBarrier(std::move(barrier));
}

aegis_access::RequestDispatchBarrierStatus
AccessRequestDispatchState::ReleaseBlockBarrier(
    const aegis_access::RequestCancellationSelector& selector,
    const std::string& operation_id,
    uint64_t operation_sequence) {
  base::AutoLock guard(lock_);
  return barriers_.ReleaseBlockBarrier(selector, operation_id,
                                       operation_sequence);
}

}  // namespace aegis::access
