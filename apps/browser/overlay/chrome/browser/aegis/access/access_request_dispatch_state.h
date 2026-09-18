// Copyright 2026 GCSA

#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_REQUEST_DISPATCH_STATE_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_REQUEST_DISPATCH_STATE_H_

#include <memory>
#include <string>

#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "components/aegis_access/published_request_runtime.h"
#include "components/aegis_access/request_ownership_registry.h"

class Profile;

namespace aegis::access {

// Thread-safe browser owner for dispatch barriers and in-flight request
// ownership. Profile user data owns one reference; URLLoader throttles retain a
// reference so request cleanup stays safe if Profile teardown races completion.
class AccessRequestDispatchState
    : public base::RefCountedThreadSafe<AccessRequestDispatchState> {
 public:
  static scoped_refptr<AccessRequestDispatchState> Get(Profile* profile);
  static scoped_refptr<AccessRequestDispatchState> GetOrCreate(Profile* profile);

  aegis_access::PublishedRequestRuntimeResult EvaluateAndRegister(
      const aegis_access::PublishedRequestRuntimeInput& input);

  aegis_access::RequestOwnershipLookupResult Lookup(
      const std::string& request_id,
      const aegis_access::OwnershipKey& owner,
      const aegis_access::GenerationTuple& generations);

  aegis_access::RequestOwnershipStatus MarkDispatched(
      const std::string& request_id,
      const aegis_access::OwnershipKey& owner,
      const aegis_access::GenerationTuple& generations,
      std::unique_ptr<aegis_access::RequestTerminationHandle> handle);

  aegis_access::RequestOwnershipStatus MarkStreaming(
      const std::string& request_id,
      const aegis_access::OwnershipKey& owner,
      const aegis_access::GenerationTuple& generations);

  aegis_access::RequestOwnershipTerminalResult Complete(
      const std::string& request_id,
      const aegis_access::OwnershipKey& owner,
      const aegis_access::GenerationTuple& generations);

  aegis_access::RequestOwnershipTerminalResult Cancel(
      const std::string& request_id,
      const aegis_access::OwnershipKey& owner,
      const aegis_access::GenerationTuple& generations);

  aegis_access::RequestOwnershipBatchCancelResult CancelMatchingPageTarget(
      const aegis_access::RequestCancellationSelector& selector);

  aegis_access::RequestDispatchBarrierStatus InstallBlockBarrier(
      aegis_access::RequestDispatchBarrier barrier);
  aegis_access::RequestDispatchBarrierStatus ReleaseBlockBarrier(
      const aegis_access::RequestCancellationSelector& selector,
      const std::string& operation_id,
      uint64_t operation_sequence);

 private:
  friend class base::RefCountedThreadSafe<AccessRequestDispatchState>;

  AccessRequestDispatchState();
  ~AccessRequestDispatchState();

  base::Lock lock_;
  aegis_access::RequestDispatchBarrierRegistry barriers_ GUARDED_BY(lock_);
  aegis_access::RequestOwnershipRegistry ownership_ GUARDED_BY(lock_);
};

}  // namespace aegis::access

#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_REQUEST_DISPATCH_STATE_H_
