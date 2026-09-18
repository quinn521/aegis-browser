// Copyright 2026 GCSA

#include "components/aegis_access/published_request_runtime.h"

#include "components/aegis_access/access_route_planner.h"

namespace aegis_access {
namespace {

RouteInput BuildRouteInput(const PublishedRequestRuntimeInput& input) {
  RouteInput route;
  route.policy_state = input.policy_state;
  route.effective_mode = input.effective_mode;
  route.policy_scope = input.policy_scope;
  route.effective_proxy_group_id = input.effective_proxy_group_id;
  route.require_proxy_intent = input.require_proxy_intent;
  route.site_ownership_reliable = input.request.site_ownership_reliable;
  route.request_owner = input.request.owner;
  route.snapshot_state = input.snapshot_state;
  route.snapshot_owner = input.snapshot_owner;
  route.request_generations = input.request.generations;
  route.snapshot_generations = input.snapshot_generations;
  route.protection_restriction = input.protection_restriction;
  route.managed_restriction = input.managed_restriction;
  route.runtime_state = input.runtime_state;
  route.registered_proxy_entry = input.registered_proxy_entry;
  return route;
}

PublishedRequestRuntimeResult NoDispatch(PublishedRequestRuntimeStatus status,
                                         RoutePlan route_plan) {
  PublishedRequestRuntimeResult result;
  result.status = status;
  result.route_plan = std::move(route_plan);
  return result;
}

RoutePlan StalePolicyGenerationPlan(
    const PublishedRequestRuntimeInput& input) {
  RoutePlan plan;
  plan.action = RouteAction::kFail;
  plan.effective_mode = input.effective_mode;
  plan.reason = RouteReason::kStaleGeneration;
  plan.generations = input.request.generations;
  return plan;
}

}  // namespace

PublishedRequestRuntimeResult EvaluatePublishedRequestForDispatch(
    const PublishedRequestRuntimeInput& input,
    const RequestDispatchBarrierRegistry* barriers,
    RequestOwnershipRegistry* ownership_registry) {
  if (input.snapshot_state == SnapshotState::kPublished &&
      (input.matched_policy_generation == 0 ||
       input.matched_policy_generation !=
           input.snapshot_generations.policy_generation)) {
    return NoDispatch(PublishedRequestRuntimeStatus::kStalePolicyGeneration,
                      StalePolicyGenerationPlan(input));
  }

  RoutePlan route_plan = PlanAccessRoute(BuildRouteInput(input));
  switch (route_plan.action) {
    case RouteAction::kWait:
      return NoDispatch(PublishedRequestRuntimeStatus::kWait,
                        std::move(route_plan));
    case RouteAction::kDeny:
      return NoDispatch(PublishedRequestRuntimeStatus::kDeny,
                        std::move(route_plan));
    case RouteAction::kFail:
      return NoDispatch(PublishedRequestRuntimeStatus::kFail,
                        std::move(route_plan));
    case RouteAction::kPreserveNative:
    case RouteAction::kUseRegisteredProxy:
      break;
  }

  RequestDispatchGateResult gate = EvaluateAndRegisterRequestForDispatch(
      input.request, barriers, ownership_registry);
  PublishedRequestRuntimeResult result;
  result.route_plan = std::move(route_plan);
  result.dispatch_gate = gate;

  if (gate.status == RequestDispatchGateStatus::kAllowRegistered &&
      gate.decision == RequestDispatchDecision::kAllow) {
    result.status = PublishedRequestRuntimeStatus::kDispatchRegistered;
  } else if (gate.status == RequestDispatchGateStatus::kBlockedByBarrier) {
    result.status = PublishedRequestRuntimeStatus::kBlockedByBarrier;
  } else {
    result.status = PublishedRequestRuntimeStatus::kRegistrationFailed;
  }
  return result;
}

}  // namespace aegis_access
