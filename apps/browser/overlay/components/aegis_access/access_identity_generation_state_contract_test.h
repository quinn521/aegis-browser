// Copyright 2026 GCSA

#ifndef COMPONENTS_AEGIS_ACCESS_ACCESS_IDENTITY_GENERATION_STATE_CONTRACT_TEST_H_
#define COMPONENTS_AEGIS_ACCESS_ACCESS_IDENTITY_GENERATION_STATE_CONTRACT_TEST_H_

#include <cstdint>
#include <limits>
#include <string>

#include "components/aegis_access/access_identity_generation_state.h"

namespace aegis_access {

class IdentityGenerationStateTestPeer {
 public:
  static void SetGeneration(IdentityGenerationState* state,
                            uint64_t generation) {
    state->generation_ = generation;
  }
};

namespace test {

class IdentityGenerationStateTestObserver {
 public:
  virtual ~IdentityGenerationStateTestObserver() = default;
  virtual void Expect(bool condition, const std::string& label) = 0;
};

inline AccessIdentityBinding InstallationGuestIdentity() {
  return {AccessIdentityKind::kInstallationGuest, "visitor-principal",
          "visitor-entitlement", "prod", "access"};
}

inline AccessIdentityBinding AuthenticatedIdentity() {
  return {AccessIdentityKind::kAuthenticated, "account-principal",
          "account-entitlement", "prod", "access"};
}

inline AccessIdentityBinding SignedOutIdentity() {
  return {AccessIdentityKind::kSignedOut, "", "", "prod", "access"};
}

inline void RunIdentityGenerationStateUnitTests(
    IdentityGenerationStateTestObserver& observer) {
  IdentityGenerationState state;
  observer.Expect(state.generation() == 0 && !state.binding().has_value() &&
                      !state.exhausted(),
                  "identity generation unit starts unpublished");

  const IdentityGenerationCommitResult guest =
      state.Commit(InstallationGuestIdentity());
  observer.Expect(
      guest.status == IdentityGenerationCommitStatus::kCommitted &&
          guest.generation == 1 && state.generation() == 1 &&
          state.binding().has_value() &&
          state.binding()->kind == AccessIdentityKind::kInstallationGuest,
      "identity generation unit commits installation guest");

  const IdentityGenerationCommitResult duplicate =
      state.Commit(InstallationGuestIdentity());
  observer.Expect(
      duplicate.status == IdentityGenerationCommitStatus::kUnchanged &&
          duplicate.generation == 1 && state.generation() == 1,
      "identity generation unit duplicate commit is idempotent");

  const IdentityGenerationCommitResult account =
      state.Commit(AuthenticatedIdentity());
  observer.Expect(
      account.status == IdentityGenerationCommitStatus::kCommitted &&
          account.generation == 2 && state.generation() == 2 &&
          state.binding()->kind == AccessIdentityKind::kAuthenticated,
      "identity generation unit verified account advances generation");

  const IdentityGenerationCommitResult signed_out =
      state.Commit(SignedOutIdentity());
  observer.Expect(
      signed_out.status == IdentityGenerationCommitStatus::kCommitted &&
          signed_out.generation == 3 && state.generation() == 3 &&
          state.binding()->kind == AccessIdentityKind::kSignedOut,
      "identity generation unit sign out advances generation");

  const IdentityGenerationCommitResult continue_guest =
      state.Commit(InstallationGuestIdentity());
  observer.Expect(
      continue_guest.status == IdentityGenerationCommitStatus::kCommitted &&
          continue_guest.generation == 4 && state.generation() == 4,
      "identity generation unit explicit continue guest advances generation");
}

inline void RunIdentityGenerationStateRegressionTests(
    IdentityGenerationStateTestObserver& observer) {
  {
    IdentityGenerationState state;
    AccessIdentityBinding invalid_guest = InstallationGuestIdentity();
    invalid_guest.principal_id.clear();
    const IdentityGenerationCommitResult result =
        state.Commit(invalid_guest);
    observer.Expect(
        result.status == IdentityGenerationCommitStatus::kInvalidIdentity &&
            result.generation == 0 && state.generation() == 0 &&
            !state.binding().has_value(),
        "identity generation regression invalid guest never publishes");
  }

  {
    IdentityGenerationState state;
    AccessIdentityBinding invalid_signed_out = SignedOutIdentity();
    invalid_signed_out.principal_id = "stale-principal";
    const IdentityGenerationCommitResult result =
        state.Commit(invalid_signed_out);
    observer.Expect(
        result.status == IdentityGenerationCommitStatus::kInvalidIdentity &&
            state.generation() == 0,
        "identity generation regression signed out cannot retain principal");
  }

  {
    IdentityGenerationState first;
    IdentityGenerationState second;
    first.Commit(InstallationGuestIdentity());
    observer.Expect(first.generation() == 1 && second.generation() == 0 &&
                        !second.binding().has_value(),
                    "identity generation regression owners are isolated");
  }

  {
    IdentityGenerationState state;
    state.Commit(InstallationGuestIdentity());
    IdentityGenerationStateTestPeer::SetGeneration(
        &state, std::numeric_limits<uint64_t>::max());
    const IdentityGenerationCommitResult exhausted =
        state.Commit(AuthenticatedIdentity());
    observer.Expect(
        exhausted.status == IdentityGenerationCommitStatus::kExhausted &&
            exhausted.generation == 0 && state.generation() == 0 &&
            state.exhausted() && !state.binding().has_value(),
        "identity generation regression overflow fails closed");

    const IdentityGenerationCommitResult retry =
        state.Commit(InstallationGuestIdentity());
    observer.Expect(
        retry.status == IdentityGenerationCommitStatus::kExhausted &&
            retry.generation == 0 && state.generation() == 0 &&
            !state.binding().has_value(),
        "identity generation regression exhausted state stays unpublished");
  }
}

}  // namespace test
}  // namespace aegis_access

#endif  // COMPONENTS_AEGIS_ACCESS_ACCESS_IDENTITY_GENERATION_STATE_CONTRACT_TEST_H_
