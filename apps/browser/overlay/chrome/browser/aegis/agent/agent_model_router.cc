// Copyright 2026 GCSA

#include "chrome/browser/aegis/agent/agent_model_router.h"

#include <algorithm>
#include <tuple>

namespace aegis::agent {
namespace {

bool IsLocal(const AgentModelDestination& destination) {
  return destination.kind != AgentModelDestination::Kind::kCloud;
}

bool MeetsRequirements(const AgentModelCatalogEntry& entry,
                       const AgentModelRequirements& requirements) {
  return (!requirements.requires_tool_calls || entry.supports_tool_calls) &&
         (requirements.context != AgentContextNeed::kLong ||
          entry.supports_long_context) &&
         (requirements.reasoning != AgentReasoningNeed::kStrong ||
          entry.supports_strong_reasoning);
}

auto KnownCostKey(const AgentModelCatalogEntry& entry) {
  return std::tuple(!entry.cost_microusd_per_million_tokens.has_value(),
                    entry.cost_microusd_per_million_tokens.value_or(0));
}

bool ComesBefore(const AgentModelCatalogEntry* left,
                 const AgentModelCatalogEntry* right,
                 AgentModelSelectionMode mode) {
  if (mode == AgentModelSelectionMode::kCost) {
    return std::tuple(KnownCostKey(*left), -left->quality_score,
                      left->latency_score, -left->priority, left->id) <
           std::tuple(KnownCostKey(*right), -right->quality_score,
                      right->latency_score, -right->priority, right->id);
  }
  if (mode == AgentModelSelectionMode::kQuality) {
    return std::tuple(-left->quality_score, left->latency_score,
                      KnownCostKey(*left), -left->priority, left->id) <
           std::tuple(-right->quality_score, right->latency_score,
                      KnownCostKey(*right), -right->priority, right->id);
  }
  return std::tuple(-left->priority, -left->quality_score,
                    left->latency_score, KnownCostKey(*left), left->id) <
         std::tuple(-right->priority, -right->quality_score,
                    right->latency_score, KnownCostKey(*right), right->id);
}

std::optional<AgentModelRoutePlan> SelectFixedRoute(
    const AgentModelSelectionInput& input,
    std::string* error) {
  if (!input.fixed_destination || !input.fixed_destination->IsValid()) {
    *error = "fixed model is not configured";
    return std::nullopt;
  }
  AgentModelRoutePlan plan{.primary = *input.fixed_destination,
                           .mode = AgentModelSelectionMode::kFixed,
                           .catalog_revision = input.catalog_revision,
                           .reason = "fixed"};
  for (const AgentModelCatalogEntry& entry : input.catalog) {
    if (entry.IsValid() && entry.enabled && entry.authorized &&
        entry.destination == plan.primary) {
      plan.primary_cost_microusd_per_million_tokens =
          entry.cost_microusd_per_million_tokens;
      break;
    }
  }
  return plan;
}

std::vector<const AgentModelCatalogEntry*> EligibleCandidates(
    const AgentModelSelectionInput& input) {
  std::vector<const AgentModelCatalogEntry*> candidates;
  candidates.reserve(input.catalog.size());
  for (const AgentModelCatalogEntry& entry : input.catalog) {
    const bool local_only_rejected =
        input.mode == AgentModelSelectionMode::kLocalOnly &&
        !IsLocal(entry.destination);
    if (entry.IsValid() && entry.enabled && entry.authorized &&
        !local_only_rejected && MeetsRequirements(entry, input.requirements)) {
      candidates.push_back(&entry);
    }
  }
  std::ranges::sort(candidates, [&input](const auto* left, const auto* right) {
    return ComesBefore(left, right, input.mode);
  });
  return candidates;
}

AgentModelRoutePlan BuildAutomaticRoute(
    const AgentModelSelectionInput& input,
    const std::vector<const AgentModelCatalogEntry*>& candidates) {
  AgentModelRoutePlan plan{
      .primary = candidates.front()->destination,
      .primary_cost_microusd_per_million_tokens =
          candidates.front()->cost_microusd_per_million_tokens,
      .mode = input.mode,
      .catalog_revision = input.catalog_revision,
      .reason = input.mode == AgentModelSelectionMode::kLocalOnly
                    ? "automatic_local_only"
                    : "automatic_policy"};
  const auto fallback = std::ranges::find_if(
      candidates.begin() + 1, candidates.end(), [&plan](const auto* candidate) {
        return candidate->destination != plan.primary;
      });
  if (fallback != candidates.end()) {
    plan.fallback = (*fallback)->destination;
    plan.fallback_cost_microusd_per_million_tokens =
        (*fallback)->cost_microusd_per_million_tokens;
  }
  return plan;
}

}  // namespace

bool AgentModelCatalogEntry::IsValid() const {
  return !id.empty() && id.size() <= 128u && destination.IsValid() &&
         quality_score >= 0 && quality_score <= 100 && latency_score >= 0 &&
         latency_score <= 100 && priority >= -1000 && priority <= 1000 &&
         (!cost_microusd_per_million_tokens ||
          *cost_microusd_per_million_tokens >= 0);
}

std::optional<AgentModelRoutePlan> SelectAgentModelRoute(
    const AgentModelSelectionInput& input,
    std::string* error) {
  if (!error) {
    return std::nullopt;
  }
  error->clear();
  if (input.catalog_revision < 0) {
    *error = "invalid model catalog revision";
    return std::nullopt;
  }
  if (input.mode == AgentModelSelectionMode::kFixed) {
    return SelectFixedRoute(input, error);
  }

  const std::vector<const AgentModelCatalogEntry*> candidates =
      EligibleCandidates(input);
  if (candidates.empty()) {
    *error = "no authorized model satisfies the task requirements";
    return std::nullopt;
  }
  return BuildAutomaticRoute(input, candidates);
}

}  // namespace aegis::agent
