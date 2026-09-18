// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_request_dispatch_state.h"

#include <memory>
#include <string>
#include <utility>

#include "base/memory/raw_ptr.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {

class FlagTerminationHandle final
    : public aegis_access::RequestTerminationHandle {
 public:
  explicit FlagTerminationHandle(bool* terminated) : terminated_(terminated) {}
  ~FlagTerminationHandle() override = default;

  void Terminate() override { *terminated_ = true; }

 private:
  raw_ptr<bool> terminated_;
};

aegis_access::RequestOwnershipRecord OwnedRequest(
    std::string request_id,
    std::string exact_host = "target.example") {
  aegis_access::RequestOwnershipRecord record;
  record.request_id = std::move(request_id);
  record.owner = {aegis_access::ChannelNamespace::kDev, "profile-token",
                  "default-partition"};
  record.generations = {1, 1, 1, 1, 1};
  record.document_token = "document-token";
  record.site_ownership_reliable = true;
  record.top_level_site = "https://top.example";
  record.exact_host = std::move(exact_host);
  record.scheme = aegis_access::RequestScheme::kHttps;
  record.port = 443;
  return record;
}

aegis_access::RequestCancellationSelector SelectorFor(
    const aegis_access::RequestOwnershipRecord& record) {
  return {record.owner,
          record.document_token,
          record.pending_navigation_token,
          record.top_level_site,
          record.exact_host,
          record.scheme,
          record.port};
}

class AccessRequestDispatchStateTest : public testing::Test {
 protected:
  content::BrowserTaskEnvironment task_environment_;
};

TEST_F(AccessRequestDispatchStateTest, RejectsNullProfile) {
  EXPECT_EQ(AccessRequestDispatchState::Get(nullptr), nullptr);
  EXPECT_EQ(AccessRequestDispatchState::GetOrCreate(nullptr), nullptr);
}

TEST_F(AccessRequestDispatchStateTest, ProfileOwnsOneStableState) {
  auto profile = TestingProfile::Builder().Build();
  auto* first = AccessRequestDispatchState::GetOrCreate(profile.get());
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, AccessRequestDispatchState::Get(profile.get()));
  EXPECT_EQ(first, AccessRequestDispatchState::GetOrCreate(profile.get()));
}

TEST_F(AccessRequestDispatchStateTest, ProfilesAreIsolated) {
  auto first_profile = TestingProfile::Builder().Build();
  auto second_profile = TestingProfile::Builder().Build();

  auto* first = AccessRequestDispatchState::GetOrCreate(first_profile.get());
  auto* second = AccessRequestDispatchState::GetOrCreate(second_profile.get());
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
}

TEST_F(AccessRequestDispatchStateTest,
       BlockOperationInstallsBarrierBeforeTerminatingMatchingRequest) {
  auto profile = TestingProfile::Builder().Build();
  auto* state = AccessRequestDispatchState::GetOrCreate(profile.get());
  ASSERT_NE(state, nullptr);

  const auto matching = OwnedRequest("matching-request");
  const auto other = OwnedRequest("other-request", "other.example");
  ASSERT_EQ(state->ownership().Register(matching),
            aegis_access::RequestOwnershipStatus::kOk);
  ASSERT_EQ(state->ownership().Register(other),
            aegis_access::RequestOwnershipStatus::kOk);

  bool terminated = false;
  ASSERT_EQ(state->ownership().MarkDispatched(
                matching.request_id, matching.owner, matching.generations,
                std::make_unique<FlagTerminationHandle>(&terminated)),
            aegis_access::RequestOwnershipStatus::kOk);

  const auto selector = SelectorFor(matching);
  const AccessBlockOperationResult result = state->BeginBlockOperation(
      {"block-operation", 10, selector});
  EXPECT_EQ(result.barrier_status,
            aegis_access::RequestDispatchBarrierStatus::kOk);
  EXPECT_TRUE(result.cancellation_attempted);
  EXPECT_EQ(result.cancellation.status,
            aegis_access::RequestOwnershipStatus::kOk);
  ASSERT_EQ(result.cancellation.cancellations.size(), 1u);
  EXPECT_TRUE(result.cancellation.cancellations.front().termination_invoked);
  EXPECT_TRUE(terminated);
  EXPECT_EQ(state->ownership().size(), 1u);
  EXPECT_EQ(state->barriers().EvaluateRequest(matching).decision,
            aegis_access::RequestDispatchDecision::kBlock);

  EXPECT_EQ(state->ReleaseBlockOperation(selector, "wrong-operation", 10),
            aegis_access::RequestDispatchBarrierStatus::kStaleOperation);
  EXPECT_EQ(state->barriers().size(), 1u);
  EXPECT_EQ(state->ReleaseBlockOperation(selector, "block-operation", 10),
            aegis_access::RequestDispatchBarrierStatus::kOk);
  EXPECT_EQ(state->barriers().size(), 0u);
}

TEST_F(AccessRequestDispatchStateTest,
       StaleBlockOperationCannotCancelUnderNewerBarrier) {
  auto profile = TestingProfile::Builder().Build();
  auto* state = AccessRequestDispatchState::GetOrCreate(profile.get());
  ASSERT_NE(state, nullptr);

  const auto matching = OwnedRequest("stale-request");
  const auto selector = SelectorFor(matching);
  ASSERT_EQ(state->BeginBlockOperation({"newer-operation", 20, selector})
                .barrier_status,
            aegis_access::RequestDispatchBarrierStatus::kOk);
  ASSERT_EQ(state->ownership().Register(matching),
            aegis_access::RequestOwnershipStatus::kOk);

  const AccessBlockOperationResult stale =
      state->BeginBlockOperation({"stale-operation", 19, selector});
  EXPECT_EQ(stale.barrier_status,
            aegis_access::RequestDispatchBarrierStatus::kStaleOperation);
  EXPECT_FALSE(stale.cancellation_attempted);
  EXPECT_EQ(state->ownership().size(), 1u);
  EXPECT_EQ(state->barriers().EvaluateRequest(matching).decision,
            aegis_access::RequestDispatchDecision::kBlock);
}

}  // namespace
}  // namespace aegis::access
