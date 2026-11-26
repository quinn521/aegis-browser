// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_private_context_client.h"

#include <memory>
#include <utility>
#include <type_traits>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "services/network/public/mojom/aegis_access_route_types.mojom.h"

namespace aegis::access {
namespace {
using namespace network;

constexpr auto kPrivateContextSetupTimeout = base::Seconds(30);

// The Mojo responder may outlive the deadline. Settle the Browser callback
// once and release its resources even while the responder is still pending.
template <typename Reply>
base::OnceCallback<void(Reply)> BoundFactorySetupReply(
    base::OnceCallback<void(Reply)> completion,
    base::OnceClosure* reject = nullptr) {
  struct PendingReply {
    void Finish(Reply reply) {
      if (!completion) return;
      timer.Stop();
      if (!deadline.is_null() && base::TimeTicks::Now() >= deadline) {
        std::move(completion).Run(std::remove_cvref_t<Reply>{});
        return;
      }
      std::move(completion).Run(std::forward<Reply>(reply));
    }
    base::TimeTicks deadline;
    base::OneShotTimer timer;
    base::OnceCallback<void(Reply)> completion;
  };
  auto pending = std::make_shared<PendingReply>();
  pending->completion = std::move(completion);
  auto fail = [](std::weak_ptr<PendingReply> weak) {
    if (auto pending = weak.lock()) pending->Finish(std::remove_cvref_t<Reply>{});
  };
  if (reject) *reject = base::BindOnce(fail, std::weak_ptr(pending));
  if (BUILDFLAG(IS_MAC)) {
    pending->deadline = base::TimeTicks::Now() + kPrivateContextSetupTimeout;
    pending->timer.Start(FROM_HERE, kPrivateContextSetupTimeout,
                         base::BindOnce(fail, std::weak_ptr(pending)));
  }
  return base::BindOnce(
      [](std::shared_ptr<PendingReply> pending, Reply reply) {
        pending->Finish(std::forward<Reply>(reply));
      }, std::move(pending));
}

mojom::AegisAccessChannel Channel(aegis_access::ChannelNamespace channel) {
  switch (channel) {
    case aegis_access::ChannelNamespace::kDev: return mojom::AegisAccessChannel::kDev;
    case aegis_access::ChannelNamespace::kAlpha: return mojom::AegisAccessChannel::kAlpha;
    case aegis_access::ChannelNamespace::kBeta: return mojom::AegisAccessChannel::kBeta;
    case aegis_access::ChannelNamespace::kRelease: return mojom::AegisAccessChannel::kRelease;
    case aegis_access::ChannelNamespace::kInvalid: NOTREACHED();
  }
  NOTREACHED();
}
std::optional<mojom::AegisAccessRuleScope> Scope(aegis_access::PolicyScope value) {
  switch (value) {
    case aegis_access::PolicyScope::kSite: return mojom::AegisAccessRuleScope::kSite;
    case aegis_access::PolicyScope::kProfile: return mojom::AegisAccessRuleScope::kProfile;
    case aegis_access::PolicyScope::kWebsite: return mojom::AegisAccessRuleScope::kWebsite;
    case aegis_access::PolicyScope::kNone:
    case aegis_access::PolicyScope::kInvalid: return std::nullopt;
  }
  return std::nullopt;
}
std::optional<mojom::AegisAccessScheme> Scheme(aegis_access::RequestScheme value) {
  switch (value) {
    case aegis_access::RequestScheme::kHttp: return mojom::AegisAccessScheme::kHttp;
    case aegis_access::RequestScheme::kHttps: return mojom::AegisAccessScheme::kHttps;
    case aegis_access::RequestScheme::kWs: return mojom::AegisAccessScheme::kWs;
    case aegis_access::RequestScheme::kWss: return mojom::AegisAccessScheme::kWss;
    case aegis_access::RequestScheme::kInvalid: return std::nullopt;
  }
  return std::nullopt;
}
std::optional<mojom::AegisAccessPortScope> Ports(aegis_access::PortScope value) {
  switch (value) {
    case aegis_access::PortScope::kAllBrowserPermitted: return mojom::AegisAccessPortScope::kAllBrowserPermitted;
    case aegis_access::PortScope::kExplicitSubset: return mojom::AegisAccessPortScope::kExplicitSubset;
    case aegis_access::PortScope::kInvalid: return std::nullopt;
  }
  return std::nullopt;
}
std::optional<mojom::AegisAccessMode> Mode(aegis_access::AccessMode value) {
  switch (value) {
    case aegis_access::AccessMode::kNone: return mojom::AegisAccessMode::kNone;
    case aegis_access::AccessMode::kDirect: return mojom::AegisAccessMode::kDirect;
    case aegis_access::AccessMode::kProxy: return mojom::AegisAccessMode::kProxy;
    case aegis_access::AccessMode::kReject: return mojom::AegisAccessMode::kReject;
    case aegis_access::AccessMode::kInvalid: return std::nullopt;
  }
  return std::nullopt;
}
std::optional<mojom::AegisAccessProtection> Protection(aegis_access::ProtectionOverride value) {
  switch (value) {
    case aegis_access::ProtectionOverride::kNone: return mojom::AegisAccessProtection::kNone;
    case aegis_access::ProtectionOverride::kTracker: return mojom::AegisAccessProtection::kTracker;
    case aegis_access::ProtectionOverride::kEasyList: return mojom::AegisAccessProtection::kEasyList;
    case aegis_access::ProtectionOverride::kCname: return mojom::AegisAccessProtection::kCname;
    case aegis_access::ProtectionOverride::kInvalid: return std::nullopt;
  }
  return std::nullopt;
}
bool Matches(const mojom::AegisAccessSnapshot& expected,
             const mojom::AegisAccessSnapshotAckPtr& ack) {
  if (!ack || !ack->accepted || !ack->owner || !ack->common_generations ||
      !ack->owner->Equals(*expected.owner) ||
      ack->incarnation != expected.incarnation || ack->sequence != expected.sequence ||
      !ack->common_generations->Equals(*expected.common_generations) ||
      ack->registrations.size() != expected.endpoints.size() ||
      ack->unavailable_groups.size() != expected.unavailable_groups.size()) return false;
  for (size_t i = 0; i < expected.endpoints.size(); ++i) {
    const auto& endpoint = expected.endpoints[i];
    const auto& registration = ack->registrations[i];
    if (!registration || registration->proxy_group_id != endpoint->proxy_group_id ||
        registration->registration_id != endpoint->registration_id ||
        registration->selection_generation != endpoint->selection_generation ||
        registration->transport != endpoint->transport ||
        registration->numeric_loopback_host != endpoint->numeric_loopback_host ||
        registration->port != endpoint->port ||
        registration->listener_binding.is_null() != endpoint->listener_binding.is_null() ||
        (registration->listener_binding &&
         !registration->listener_binding->Equals(*endpoint->listener_binding))) return false;
  }
  for (size_t i = 0; i < expected.unavailable_groups.size(); ++i) {
    if (!ack->unavailable_groups[i] ||
        !ack->unavailable_groups[i]->Equals(*expected.unavailable_groups[i]))
      return false;
  }
  return true;
}
}  // namespace
AccessPrivateContextClient::AccessPrivateContextClient(
    aegis_access::OwnershipKey owner) {
  CHECK(aegis_access::IsCompleteOwner(owner));
  owner_ = network::mojom::AegisAccessOwner::New(Channel(owner.channel),
      std::move(owner.profile_token), std::move(owner.storage_partition_token));
}
AccessPrivateContextClient::~AccessPrivateContextClient() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}
void AccessPrivateContextClient::Initialize(network::mojom::NetworkContext* context,
    base::OnceCallback<void(bool)> completion) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!context_.is_bound());
  bind_deadline_ = base::TimeTicks::Now() + kPrivateContextSetupTimeout;
  const auto operation = ++operation_;
  auto receiver = context_.BindNewPipeAndPassReceiver();
  context_.set_disconnect_with_reason_handler(base::BindOnce(
      &AccessPrivateContextClient::OnDisconnect, weak_factory_.GetWeakPtr()));
  auto reply = BoundFactorySetupReply(base::BindOnce(
          [](base::WeakPtr<AccessPrivateContextClient> self, uint64_t operation,
             base::OnceCallback<void(bool)> completion,
             mojom::AegisAccessContextIdentityPtr identity) {
            const bool current = self && operation == self->operation_;
            const bool accepted = current && !self->retired_ && identity &&
                !identity->incarnation.empty() && identity->process_id;
            if (current) {
              self->reject_initialize_.Reset();
              if (accepted) {
                self->incarnation_ = identity->incarnation;
                self->process_id_ = identity->process_id;
                self->connected_ = true;
              } else {
                self->context_.reset();
              }
            }
            std::move(completion).Run(accepted);
          }, weak_factory_.GetWeakPtr(), operation, std::move(completion)),
          &reject_initialize_);
  context->BindAegisAccessContext(
      std::move(receiver), lifetime_.BindNewPipeAndPassRemote(),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(reply), mojom::AegisAccessContextIdentityPtr()));
}

void AccessPrivateContextClient::CheckConnected(
    base::OnceCallback<void(bool)> completion) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!is_connected()) {
    std::move(completion).Run(false);
    return;
  }
  auto reply = BoundFactorySetupReply(std::move(completion));
  context_.QueryVersion(base::BindOnce(
      [](base::OnceCallback<void(bool)> reply, uint32_t) {
        std::move(reply).Run(true);
      }, mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(reply), false)));
}

void AccessPrivateContextClient::PublishSnapshot(
    network::mojom::AegisAccessSnapshotPtr snapshot,
    base::OnceCallback<void(bool)> completion) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!connected_ || publication_pending_ || !snapshot || !snapshot->owner ||
      !snapshot->owner->Equals(*owner_) || !snapshot->common_generations ||
      (!snapshot->incarnation.empty() && snapshot->incarnation != incarnation_) ||
      snapshot->sequence <= last_requested_sequence_) {
    std::move(completion).Run(false);
    return;
  }
  snapshot->incarnation = incarnation_;
  auto expected = snapshot.Clone();
  last_requested_sequence_ = snapshot->sequence;
  publication_pending_ = true;
  ready_ = false;
  const auto operation = ++operation_;
  context_->InstallSnapshot(std::move(snapshot),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(base::BindOnce(
          [](base::WeakPtr<AccessPrivateContextClient> self, uint64_t operation,
             network::mojom::AegisAccessSnapshotPtr expected,
             base::OnceCallback<void(bool)> completion,
             network::mojom::AegisAccessSnapshotAckPtr ack) {
            const bool current = self && operation == self->operation_;
            const bool accepted = current && Matches(*expected, ack);
            if (current) {
              self->publication_pending_ = false;
              self->ready_ = accepted;
              if (accepted) self->published_sequence_ = expected->sequence;
            }
            std::move(completion).Run(accepted);
          }, weak_factory_.GetWeakPtr(), operation, std::move(expected),
          std::move(completion)), network::mojom::AegisAccessSnapshotAckPtr()));
}
void AccessPrivateContextClient::PublishPolicy(
    const aegis_access::PublishedAccessPolicySnapshot& policy,
    const aegis_access::GenerationTuple& common, uint64_t sequence,
    const std::vector<AccessPrivateEndpoint>& endpoints,
    base::OnceCallback<void(bool)> completion) {
  PublishPolicy(policy, common, sequence, endpoints, {}, std::move(completion));
}
void AccessPrivateContextClient::PublishPolicy(
    const aegis_access::PublishedAccessPolicySnapshot& policy,
    const aegis_access::GenerationTuple& common, uint64_t sequence,
    const std::vector<AccessPrivateEndpoint>& endpoints,
    const std::map<std::string, uint64_t>& unavailable_groups,
    base::OnceCallback<void(bool)> completion) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto reject = [&] { std::move(completion).Run(false); };
  if (aegis_access::ValidateAccessPolicySnapshot(policy) !=
          aegis_access::PolicyMatchReason::kNone ||
      policy.policy_generation != common.policy_generation ||
      common.policy_generation == 0 || common.identity_generation == 0 ||
      common.network_epoch == 0 || common.base_proxy_config_generation == 0 ||
      common.selection_generation != 0) {
    reject();
    return;
  }
  auto snapshot = network::mojom::AegisAccessSnapshot::New();
  snapshot->owner = network::mojom::AegisAccessOwner::New(Channel(policy.owner.channel),
      policy.owner.profile_token, policy.owner.storage_partition_token);
  snapshot->sequence = sequence;
  snapshot->common_generations = network::mojom::AegisAccessGenerations::New(
      common.policy_generation, common.identity_generation, 0,
      common.network_epoch, common.base_proxy_config_generation);
  for (const auto& rule : policy.rules) {
    const auto scope = Scope(rule.scope);
    const auto ports = Ports(rule.ports.scope);
    const auto mode = Mode(rule.mode);
    const auto protection = Protection(rule.protection_override);
    if (!scope || !ports || !mode || !protection) {
      reject();
      return;
    }
    auto encoded = network::mojom::AegisAccessRule::New();
    encoded->rule_id = rule.rule_id;
    encoded->scope = *scope;
    encoded->top_level_site = rule.top_level_site;
    encoded->destination_host = rule.destination_host;
    encoded->include_subdomains = rule.include_subdomains;
    for (const auto scheme : rule.schemes) {
      const auto value = Scheme(scheme);
      if (!value) {
        reject();
        return;
      }
      encoded->schemes.push_back(*value);
    }
    encoded->port_scope = *ports;
    encoded->explicit_ports = rule.ports.explicit_ports;
    encoded->mode = *mode;
    encoded->proxy_group_id = rule.proxy_group_id;
    encoded->protection_override = *protection;
    encoded->row_revision = rule.row_revision;
    encoded->last_operation_sequence = rule.last_operation_sequence;
    snapshot->rules.push_back(std::move(encoded));
  }
  for (const auto& endpoint : endpoints) {
    const auto& registration = endpoint.registration;
    const auto& g = registration.generations;
    using Transport = aegis_access::RegisteredProxyTransport;
    const bool no_auth = endpoint.listener_binding &&
        aegis_access::IsValidAccessPreparedListenerBinding(*endpoint.listener_binding) &&
        !endpoint.local_credentials;
    const bool legacy_http = !endpoint.listener_binding && registration.transport == Transport::kHttp &&
        endpoint.local_credentials && !endpoint.local_credentials->username().empty() &&
        !endpoint.local_credentials->password().empty();
    if (registration.owner != policy.owner ||
        (!no_auth && !legacy_http) ||
        (registration.transport != Transport::kHttp && registration.transport != Transport::kSocks5) ||
        (registration.host != "127.0.0.1" && registration.host != "::1") || !registration.port ||
        (registration.transport == Transport::kSocks5 && registration.port == 1080) ||
        g.policy_generation != common.policy_generation ||
        g.identity_generation != common.identity_generation ||
        g.network_epoch != common.network_epoch ||
        g.base_proxy_config_generation != common.base_proxy_config_generation ||
        g.selection_generation == 0) {
      reject();
      return;
    }
    auto encoded = network::mojom::AegisAccessProxyEndpoint::New();
    encoded->proxy_group_id = registration.proxy_group_id;
    encoded->registration_id = registration.registration_id;
    encoded->selection_generation = g.selection_generation;
    encoded->numeric_loopback_host = registration.host;
    encoded->port = registration.port;
    encoded->transport = registration.transport == Transport::kSocks5
        ? mojom::AegisAccessProxyTransport::kSocks5 : mojom::AegisAccessProxyTransport::kHttp;
    if (endpoint.listener_binding) {
      const auto& binding = *endpoint.listener_binding;
      encoded->listener_binding = mojom::AegisAccessPreparedListenerBinding::New(
          binding.wire_version, binding.connection_profile_id, binding.profile_version,
          binding.upstream_authorization_generation, binding.listener_generation);
    }
    encoded->credentials = endpoint.local_credentials;
    snapshot->endpoints.push_back(std::move(encoded));
  }
  for (const auto& [group, selection] : unavailable_groups) {
    snapshot->unavailable_groups.push_back(
        network::mojom::AegisAccessUnavailableGroup::New(group, selection));
  }
  PublishSnapshot(std::move(snapshot), std::move(completion));
}

void AccessPrivateContextClient::BeginRestore(
    base::OnceCallback<void(bool)> completion) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ready_ = false;
  publication_pending_ = false;
  ++operation_;
  if (!connected_) {
    std::move(completion).Run(false);
    return;
  }
  context_->BeginRestore(owner_.Clone(), incarnation_,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(completion), false));
}
void AccessPrivateContextClient::CreateFactory(
    network::mojom::URLLoaderFactoryParamsPtr params,
    network::mojom::AegisAccessFactoryBindingPtr binding,
    mojo::PendingReceiver<network::mojom::AegisAccessURLLoaderFactory> receiver,
    base::OnceCallback<void(bool)> completion) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!ready_ || !binding || !binding->owner ||
      !binding->owner->Equals(*owner_) ||
      (!binding->incarnation.empty() && binding->incarnation != incarnation_)) {
    std::move(completion).Run(false);
    return;
  }
  binding->incarnation = incarnation_;
  auto reply = BoundFactorySetupReply(base::BindOnce(
          [](base::WeakPtr<AccessPrivateContextClient> self, uint64_t operation,
             base::OnceCallback<void(bool)> completion, bool created) {
            std::move(completion).Run(self && operation == self->operation_ &&
                                      self->ready_ && created);
          }, weak_factory_.GetWeakPtr(), operation_, std::move(completion)));
  context_->CreateFactory(std::move(receiver), std::move(params), std::move(binding),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(reply), false));
}
void AccessPrivateContextClient::SetDisconnectHandler(
    base::OnceCallback<void(bool)> handler) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(connected_ && !on_disconnect_);
  on_disconnect_ = std::move(handler);
}

void AccessPrivateContextClient::SetRetirementHandler(base::OnceClosure handler) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!retired_ && !on_retired_);
  on_retired_ = std::move(handler);
}

void AccessPrivateContextClient::OnContextRetired() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  retired_ = true;
  connected_ = ready_ = publication_pending_ = false;
  ++operation_;
  auto notify = std::move(on_retired_);
  if (reject_initialize_) std::move(reject_initialize_).Run();
  if (notify) std::move(notify).Run();
}

void AccessPrivateContextClient::OnDisconnect(uint32_t reason, const std::string&) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  connected_ = ready_ = publication_pending_ = false;
  incarnation_.clear();
  ++operation_;
  auto notify = std::move(on_disconnect_);
  if (reject_initialize_) std::move(reject_initialize_).Run();
  if (notify) {
    std::move(notify).Run(reason == network::mojom::kAegisAccessContextRetired);
  }
}
}  // namespace aegis::access
