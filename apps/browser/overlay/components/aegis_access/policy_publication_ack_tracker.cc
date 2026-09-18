// Copyright 2026 GCSA

#include "components/aegis_access/policy_publication_ack_tracker.h"

#include <algorithm>
#include <utility>

namespace aegis_access {
namespace {

bool IsValidAckToken(const std::string& token) {
  return !token.empty() && token.size() <= 128;
}

}  // namespace

PolicyPublicationAckTracker::PolicyPublicationAckTracker(
    size_t max_operations,
    size_t max_ack_tokens_per_operation)
    : max_operations_(max_operations),
      max_ack_tokens_per_operation_(max_ack_tokens_per_operation) {}

PolicyPublicationAckTracker::~PolicyPublicationAckTracker() = default;

bool PolicyPublicationAckTracker::IsReady(const Entry& entry) {
  return entry.received_acks.size() == entry.required_acks.size() &&
         (!entry.require_termination || entry.termination_complete) &&
         entry.durable_committed;
}

PolicyPublicationAckSnapshot PolicyPublicationAckTracker::SnapshotFor(
    const Entry& entry) {
  return {
      entry.identity,
      entry.required_acks.size(),
      entry.received_acks.size(),
      entry.termination_complete,
      entry.durable_committed,
      IsReady(entry),
  };
}

PolicyPublicationAckResult PolicyPublicationAckTracker::ResultFor(
    PolicyPublicationAckStatus status,
    const Entry* entry) {
  if (!entry) {
    return {status, std::nullopt};
  }
  return {status, SnapshotFor(*entry)};
}

PolicyPublicationAckTracker::Entry*
PolicyPublicationAckTracker::FindBySelector(
    const RequestCancellationSelector& selector) {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const Entry& entry) {
                           return SameRequestCancellationSelector(
                               entry.identity.selector, selector);
                         });
  return it == entries_.end() ? nullptr : &*it;
}

const PolicyPublicationAckTracker::Entry*
PolicyPublicationAckTracker::FindBySelector(
    const RequestCancellationSelector& selector) const {
  auto it = std::find_if(entries_.begin(), entries_.end(),
                         [&](const Entry& entry) {
                           return SameRequestCancellationSelector(
                               entry.identity.selector, selector);
                         });
  return it == entries_.end() ? nullptr : &*it;
}

PolicyPublicationAckStatus PolicyPublicationAckTracker::ValidateIdentity(
    const PolicyPublicationIdentity& identity,
    const Entry& entry) const {
  if (identity.operation_sequence < entry.identity.operation_sequence) {
    return PolicyPublicationAckStatus::kStaleOperation;
  }
  if (identity.operation_sequence != entry.identity.operation_sequence ||
      identity.operation_id != entry.identity.operation_id) {
    return PolicyPublicationAckStatus::kVersionMismatch;
  }
  if (identity.policy_generation < entry.identity.policy_generation) {
    return PolicyPublicationAckStatus::kStaleGeneration;
  }
  if (identity.policy_generation != entry.identity.policy_generation) {
    return PolicyPublicationAckStatus::kVersionMismatch;
  }
  return PolicyPublicationAckStatus::kPending;
}

PolicyPublicationAckResult PolicyPublicationAckTracker::Begin(
    PolicyPublicationAckRequirements requirements) {
  if (requirements.identity.operation_id.empty() ||
      requirements.identity.operation_sequence == 0 ||
      requirements.identity.policy_generation == 0 ||
      !IsValidRequestCancellationSelector(requirements.identity.selector) ||
      requirements.required_ack_tokens.empty() ||
      requirements.required_ack_tokens.size() >
          max_ack_tokens_per_operation_) {
    return {PolicyPublicationAckStatus::kInvalidOperation, std::nullopt};
  }

  std::set<std::string> required_acks;
  for (const std::string& token : requirements.required_ack_tokens) {
    if (!IsValidAckToken(token) || !required_acks.insert(token).second) {
      return {PolicyPublicationAckStatus::kInvalidOperation, std::nullopt};
    }
  }

  Entry* current = FindBySelector(requirements.identity.selector);
  if (current) {
    if (requirements.identity.operation_sequence <
        current->identity.operation_sequence) {
      return ResultFor(PolicyPublicationAckStatus::kStaleOperation, current);
    }
    if (requirements.identity.operation_sequence ==
        current->identity.operation_sequence) {
      if (requirements.identity != current->identity ||
          required_acks != current->required_acks ||
          requirements.require_termination != current->require_termination) {
        return ResultFor(PolicyPublicationAckStatus::kVersionMismatch, current);
      }
      return ResultFor(IsReady(*current)
                           ? PolicyPublicationAckStatus::kReady
                           : PolicyPublicationAckStatus::kPending,
                       current);
    }
    if (requirements.identity.policy_generation <=
        current->identity.policy_generation) {
      return ResultFor(PolicyPublicationAckStatus::kStaleGeneration, current);
    }

    current->identity = std::move(requirements.identity);
    current->required_acks = std::move(required_acks);
    current->received_acks.clear();
    current->require_termination = requirements.require_termination;
    current->termination_complete = false;
    current->durable_committed = false;
    return ResultFor(PolicyPublicationAckStatus::kPending, current);
  }

  if (entries_.size() >= max_operations_) {
    return {PolicyPublicationAckStatus::kCapacityExceeded, std::nullopt};
  }

  entries_.push_back({
      std::move(requirements.identity),
      std::move(required_acks),
      {},
      requirements.require_termination,
      false,
      false,
  });
  return ResultFor(PolicyPublicationAckStatus::kPending, &entries_.back());
}

PolicyPublicationAckResult PolicyPublicationAckTracker::Acknowledge(
    const PolicyPublicationIdentity& identity,
    const std::string& ack_token) {
  Entry* entry = FindBySelector(identity.selector);
  if (!entry) {
    return {PolicyPublicationAckStatus::kNotFound, std::nullopt};
  }
  const PolicyPublicationAckStatus identity_status =
      ValidateIdentity(identity, *entry);
  if (identity_status != PolicyPublicationAckStatus::kPending) {
    return ResultFor(identity_status, entry);
  }
  if (!entry->required_acks.contains(ack_token)) {
    return ResultFor(PolicyPublicationAckStatus::kUnexpectedAck, entry);
  }
  if (!entry->received_acks.insert(ack_token).second) {
    return ResultFor(PolicyPublicationAckStatus::kDuplicateAck, entry);
  }
  return ResultFor(IsReady(*entry) ? PolicyPublicationAckStatus::kReady
                                   : PolicyPublicationAckStatus::kPending,
                   entry);
}

PolicyPublicationAckResult
PolicyPublicationAckTracker::MarkTerminationsComplete(
    const PolicyPublicationIdentity& identity) {
  Entry* entry = FindBySelector(identity.selector);
  if (!entry) {
    return {PolicyPublicationAckStatus::kNotFound, std::nullopt};
  }
  const PolicyPublicationAckStatus identity_status =
      ValidateIdentity(identity, *entry);
  if (identity_status != PolicyPublicationAckStatus::kPending) {
    return ResultFor(identity_status, entry);
  }
  entry->termination_complete = true;
  return ResultFor(IsReady(*entry) ? PolicyPublicationAckStatus::kReady
                                   : PolicyPublicationAckStatus::kPending,
                   entry);
}

PolicyPublicationAckResult PolicyPublicationAckTracker::MarkDurablyCommitted(
    const PolicyPublicationIdentity& identity) {
  Entry* entry = FindBySelector(identity.selector);
  if (!entry) {
    return {PolicyPublicationAckStatus::kNotFound, std::nullopt};
  }
  const PolicyPublicationAckStatus identity_status =
      ValidateIdentity(identity, *entry);
  if (identity_status != PolicyPublicationAckStatus::kPending) {
    return ResultFor(identity_status, entry);
  }
  entry->durable_committed = true;
  return ResultFor(IsReady(*entry) ? PolicyPublicationAckStatus::kReady
                                   : PolicyPublicationAckStatus::kPending,
                   entry);
}

PolicyPublicationAckResult PolicyPublicationAckTracker::Lookup(
    const PolicyPublicationIdentity& identity) const {
  const Entry* entry = FindBySelector(identity.selector);
  if (!entry) {
    return {PolicyPublicationAckStatus::kNotFound, std::nullopt};
  }
  const PolicyPublicationAckStatus identity_status =
      ValidateIdentity(identity, *entry);
  if (identity_status != PolicyPublicationAckStatus::kPending) {
    return ResultFor(identity_status, entry);
  }
  return ResultFor(IsReady(*entry) ? PolicyPublicationAckStatus::kReady
                                   : PolicyPublicationAckStatus::kPending,
                   entry);
}

PolicyPublicationAckResult PolicyPublicationAckTracker::Finalize(
    const PolicyPublicationIdentity& identity) {
  Entry* entry = FindBySelector(identity.selector);
  if (!entry) {
    return {PolicyPublicationAckStatus::kNotFound, std::nullopt};
  }
  const PolicyPublicationAckStatus identity_status =
      ValidateIdentity(identity, *entry);
  if (identity_status != PolicyPublicationAckStatus::kPending) {
    return ResultFor(identity_status, entry);
  }
  if (!IsReady(*entry)) {
    return ResultFor(PolicyPublicationAckStatus::kNotReady, entry);
  }

  const PolicyPublicationAckSnapshot snapshot = SnapshotFor(*entry);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& candidate) {
                                  return SameRequestCancellationSelector(
                                      candidate.identity.selector,
                                      identity.selector);
                                }),
                 entries_.end());
  return {PolicyPublicationAckStatus::kFinalized, snapshot};
}

}  // namespace aegis_access
