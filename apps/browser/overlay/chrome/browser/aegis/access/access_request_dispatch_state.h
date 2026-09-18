// Copyright 2026 GCSA

#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_REQUEST_DISPATCH_STATE_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_REQUEST_DISPATCH_STATE_H_

#include <cstdint>
#include <string>

#include "base/supports_user_data.h"
#include "components/aegis_access/request_ownership_registry.h"

class Profile;

namespace aegis::access {

struct AccessBlockOperationResult {
  aegis_access::RequestDispatchBarrierStatus barrier_status =
      aegis_access::RequestDispatchBarrierStatus::kInvalidBarrier;
  bool cancellation_attempted = false;
  aegis_access::RequestOwnershipBatchCancelResult cancellation;
};

// UI-thread Profile-owned request dispatch state shared by all Aegis
// URLLoaderFactory wrappers for the Profile. Keeping barrier and ownership
// registries here gives policy mutation/cancellation code one stable owner
// without exposing renderer or Network Service objects to the pure contracts.
class AccessRequestDispatchState : public base::SupportsUserData::Data {
 public:
  static AccessRequestDispatchState* Get(Profile* profile);
  static AccessRequestDispatchState* GetOrCreate(Profile* profile);

  AccessRequestDispatchState(const AccessRequestDispatchState&) = delete;
  AccessRequestDispatchState& operator=(const AccessRequestDispatchState&) =
      delete;
  ~AccessRequestDispatchState() override;

  aegis_access::RequestDispatchBarrierRegistry& barriers() {
    return barriers_;
  }
  aegis_access::RequestOwnershipRegistry& ownership() {
    return ownership_;
  }

  // Installs the BLOCK barrier before touching any in-flight request, then
  // synchronously cancels only requests matching the trusted page-target
  // selector. If cancellation cannot complete, the barrier intentionally
  // remains installed so no new matching request can escape.
  AccessBlockOperationResult BeginBlockOperation(
      aegis_access::RequestDispatchBarrier barrier);

  // Only the exact owning operation may release its barrier. Persistence and
  // ACK orchestration lives above this state owner and calls release only after
  // its own success boundary.
  aegis_access::RequestDispatchBarrierStatus ReleaseBlockOperation(
      const aegis_access::RequestCancellationSelector& selector,
      const std::string& operation_id,
      uint64_t operation_sequence);

 private:
  AccessRequestDispatchState();

  aegis_access::RequestDispatchBarrierRegistry barriers_;
  aegis_access::RequestOwnershipRegistry ownership_;
};

}  // namespace aegis::access

#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_REQUEST_DISPATCH_STATE_H_
