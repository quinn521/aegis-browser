// Copyright 2026 GCSA

#include "chrome/browser/aegis/agent/typesafe_choice_contract.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace aegis::agent {

bool ValidateTypeSafeChoice(
    const TypeSafeChoiceValue& value,
    std::span<const std::string_view> allowed_options,
    double minimum_confidence,
    std::string* error) {
  if (!error) {
    return false;
  }
  error->clear();
  if (allowed_options.size() < 2u || !std::isfinite(minimum_confidence) ||
      minimum_confidence < 0.0 || minimum_confidence > 1.0 ||
      !std::isfinite(value.confidence) || value.confidence < 0.0 ||
      value.confidence > 1.0) {
    *error = "invalid TypeSafe choice contract";
    return false;
  }

  std::map<std::string_view, double> distribution;
  double total = 0.0;
  for (const auto& [name, probability] : value.probabilities) {
    const bool allowed =
        std::ranges::find(allowed_options, name) != allowed_options.end();
    if (!allowed || distribution.contains(name) ||
        !std::isfinite(probability) || probability < 0.0 ||
        probability > 1.0) {
      *error = "invalid TypeSafe probability distribution";
      return false;
    }
    distribution.emplace(name, probability);
    total += probability;
  }
  if (distribution.size() != allowed_options.size() ||
      std::abs(total - 1.0) > 0.000001) {
    *error = "incomplete TypeSafe probability distribution";
    return false;
  }

  auto selected = distribution.find(value.choice);
  if (selected == distribution.end()) {
    *error = "unknown TypeSafe choice";
    return false;
  }
  for (const auto& [name, probability] : distribution) {
    if (name != selected->first && probability >= selected->second) {
      *error = "TypeSafe choice contradicts its probability distribution";
      return false;
    }
  }
  if (value.confidence < minimum_confidence) {
    *error = "TypeSafe choice confidence is below the routing threshold";
    return false;
  }
  return true;
}

}  // namespace aegis::agent
