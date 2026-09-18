// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_request_dispatch_state.h"

#include <memory>
#include <utility>

#include "chrome/browser/aegis/aegis_profile_support.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"

namespace aegis::access {
namespace {

const void* const kDispatchStateUserDataKey = &kDispatchStateUserDataKey;
constexpr size_t kMaxDispatchBarriers = 256;
constexpr size_t kMaxOwnedRequests = 4096;

}  // namespace

// static
AccessRequestDispatchState* AccessRequestDispatchState::Get(Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile) {
    return nullptr;
  }
  return static_cast<AccessRequestDispatchState*>(
      profile->GetUserData(kDispatchStateUserDataKey));
}

// static
AccessRequestDispatchState* AccessRequestDispatchState::GetOrCreate(
    Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!profile || !aegis::IsAegisProfileSupported(profile)) {
    return nullptr;
  }
  if (auto* existing = Get(profile)) {
    return existing;
  }
  auto state =
      std::unique_ptr<AccessRequestDispatchState>(new AccessRequestDispatchState());
  auto* result = state.get();
  profile->SetUserData(kDispatchStateUserDataKey, std::move(state));
  return result;
}

AccessRequestDispatchState::AccessRequestDispatchState()
    : barriers_(kMaxDispatchBarriers), ownership_(kMaxOwnedRequests) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

AccessRequestDispatchState::~AccessRequestDispatchState() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

AccessBlockOperationResult AccessRequestDispatchState::BeginBlockOperation(
    aegis_access::RequestDispatchBarrier barrier) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const aegis_access::RequestCancellationSelector selector = barrier.selector;

  AccessBlockOperationResult result;
  result.barrier_status = barriers_.InstallBlockBarrier(std::move(barrier));
  if (result.barrier_status !=
      aegis_access::RequestDispatchBarrierStatus::kOk) {
    return result;
  }

  result.cancellation_attempted = true;
  result.cancellation = ownership_.CancelMatchingPageTarget(selector);
  return result;
}

aegis_access::RequestDispatchBarrierStatus
AccessRequestDispatchState::ReleaseBlockOperation(
    const aegis_access::RequestCancellationSelector& selector,
    const std::string& operation_id,
    uint64_t operation_sequence) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return barriers_.ReleaseBlockBarrier(selector, operation_id,
                                       operation_sequence);
}

}  // namespace aegis::access
