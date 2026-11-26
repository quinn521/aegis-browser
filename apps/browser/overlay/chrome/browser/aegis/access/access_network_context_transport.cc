// Copyright 2026 GCSA

#include "chrome/browser/aegis/access/access_network_context_transport.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <utility>

#include "base/barrier_closure.h"
#include "base/task/bind_post_task.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/process/process_handle.h"
#include "base/functional/callback_helpers.h"
#include "base/uuid.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/aegis/aegis_profile_support.h"
#include "chrome/browser/profiles/profile.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/proxy_server.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_info.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "url/gurl.h"

namespace aegis::access {
namespace {

const void* const kTransportUserDataKey = &kTransportUserDataKey;
constexpr size_t kMaxExactHosts = 256;
constexpr size_t kMaxPartitionKeyBytes = 1024;
constexpr base::TimeDelta kPrivateInstallTimeout = base::Seconds(30);

std::optional<content::ServiceProcessId> NetworkServiceProcessFor(
    uint64_t pid) {
  if (pid == static_cast<uint64_t>(base::GetCurrentProcId())) {
    return content::ServiceProcessId();
  }
  std::optional<content::ServiceProcessId> found;
  for (const auto& info : content::ServiceProcessHost::GetRunningProcessInfo()) {
    if (!info.IsService<network::mojom::NetworkService>() ||
        pid != static_cast<uint64_t>(info.GetProcess().Pid())) {
      continue;
    }
    if (found) return std::nullopt;
    found = info.service_process_id();
  }
  return found;
}

bool IsCanonicalExactHost(const std::string& host) {
  if (host.empty() || host.back() == '.') {
    return false;
  }
  const GURL url("https://" + host + "/");
  return url.is_valid() && url.has_host() && url.host() == host;
}

bool AreCanonicalExactHosts(const std::vector<std::string>& exact_hosts) {
  return !exact_hosts.empty() && exact_hosts.size() <= kMaxExactHosts &&
         std::ranges::is_sorted(exact_hosts) &&
         std::adjacent_find(exact_hosts.begin(), exact_hosts.end()) ==
             exact_hosts.end() &&
         std::ranges::all_of(exact_hosts, IsCanonicalExactHost);
}

aegis_access::RoutePlan RoutePlanForEndpoint(
    const aegis_access::RegisteredProxyEndpoint& endpoint) {
  aegis_access::RoutePlan plan;
  plan.action = aegis_access::RouteAction::kUseRegisteredProxy;
  plan.effective_mode = aegis_access::AccessMode::kProxy;
  plan.reason = aegis_access::RouteReason::kNone;
  plan.generations = endpoint.generations;
  plan.registered_proxy_entry = aegis_access::RegisteredProxyEntry{
      endpoint.registration_id, endpoint.proxy_group_id, endpoint.owner,
      endpoint.generations};
  return plan;
}

bool IsCandidateIdentityValid(
    const aegis_access::PolicyPublicationIdentity& identity,
    uint64_t network_epoch) {
  return !(
      !aegis_access::IsValidRequestCancellationSelector(identity.selector) ||
      identity.operation_id.empty() || identity.operation_sequence == 0 ||
      identity.policy_generation == 0 ||
      identity.policy_generation != identity.operation_sequence ||
      (identity.selection_generation == 0) != identity.proxy_group_id.empty() ||
      identity.network_epoch == 0 || identity.network_epoch != network_epoch);
}

}  // namespace

// static
AccessNetworkContextTransport* AccessNetworkContextTransport::Get(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return static_cast<AccessNetworkContextTransport*>(
      profile->GetUserData(kTransportUserDataKey));
}

// static
AccessNetworkContextTransport* AccessNetworkContextTransport::GetOrCreate(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  if (auto* existing = Get(profile)) {
    return existing;
  }
  auto transport = std::unique_ptr<AccessNetworkContextTransport>(
      new AccessNetworkContextTransport());
  auto* result = transport.get();
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  profile->SetAccountProxyDispatchGate(base::BindRepeating(
      [](base::WeakPtr<AccessNetworkContextTransport> self, const GURL& url) {
        return !self || self->AccountDispatchBlocked(url);
      }, result->weak_factory_.GetWeakPtr()));
#endif
  profile->SetUserData(kTransportUserDataKey, std::move(transport));
  return result;
}

AccessNetworkContextTransport::AccessNetworkContextTransport()
    : runtime_profile_token_(
          base::Uuid::GenerateRandomV4().AsLowercaseString()) {
  net::NetworkChangeNotifier::AddNetworkChangeObserver(this);
  content::ServiceProcessHost::AddObserver(this);
}

AccessNetworkContextTransport::~AccessNetworkContextTransport() {
  content::ServiceProcessHost::RemoveObserver(this);
  net::NetworkChangeNotifier::RemoveNetworkChangeObserver(this);
}

void AccessNetworkContextTransport::OnServiceProcessTerminatedNormally(
    const content::ServiceProcessInfo& info) {
  if (info.IsService<network::mojom::NetworkService>()) {
    OnNetworkServiceExited(info.service_process_id());
  }
}

void AccessNetworkContextTransport::OnServiceProcessCrashed(
    const content::ServiceProcessInfo& info) {
  if (info.IsService<network::mojom::NetworkService>()) {
    OnNetworkServiceExited(info.service_process_id());
  }
}

void AccessNetworkContextTransport::OnNetworkServiceExited(
    content::ServiceProcessId service_process) {
  for (auto& [partition, state] : partitions_) {
    const auto retired = std::erase_if(
        state.disconnected_private_contexts, [&](const auto& item) {
          return item.second.service_process == service_process;
        });
    if (!state.private_publication) continue;
    const auto owner = state.private_publication->policy.owner;
    std::vector<std::string> exited_contexts;
    for (const auto& [incarnation, context] : state.private_contexts) {
      if (context.service_process == service_process) {
        exited_contexts.push_back(incarnation);
      }
    }
    for (const auto& incarnation : exited_contexts) {
      OnPrivateContextRetired(owner, incarnation);
    }
    if (retired && HasReadyPrivateContext(owner)) {
      SettlePrivatePublication(state, true);
    } else if (retired && state.private_contexts.empty()) {
      SettleTestListenerRetirements(state, true);
    }
  }
}

void AccessNetworkContextTransport::OnNetworkChanged(
    net::NetworkChangeNotifier::ConnectionType) {
  if (network_epoch_ == 0) {
    return;
  }
  if (network_epoch_ == std::numeric_limits<uint64_t>::max()) {
    // Zero is the incomplete GenerationTuple sentinel. Once the counter is
    // exhausted, keep the source invalid so callers fail closed rather than
    // wrapping to a previously valid epoch.
    network_epoch_ = 0;
    return;
  }
  ++network_epoch_;

  // Keep the already-installed localhost proxy route intact here. Broadcasting
  // an empty CustomProxyConfig would restore Chromium's native proxy result and
  // could silently downgrade a request that still requires proxying to DIRECT.
  // The incremented epoch invalidates stale re-publication; the request-runtime
  // generation gate is responsible for blocking stale tuples while the local
  // proxy runtime rebinds to the new network.
}

// static
bool AccessNetworkContextTransport::ConfigureNetworkContext(
    Profile* profile,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* network_context_params) {
  if (!aegis::IsAegisProfileSupported(profile) || !network_context_params ||
      network_context_params->initial_custom_proxy_config ||
      network_context_params->custom_proxy_config_client_receiver.is_valid() ||
      network_context_params->custom_proxy_connection_observer_remote.is_valid()) {
    return false;
  }
  AccessNetworkContextTransport* transport = GetOrCreate(profile);
  const std::optional<std::string> key = PartitionKey(relative_partition_path);
  if (!transport || !key) {
    return false;
  }

  PartitionState& state = transport->partitions_[*key];
  network_context_params->initial_custom_proxy_config =
      transport->BuildConfig(state);
  mojo::PendingRemote<network::mojom::CustomProxyConfigClient> client;
  network_context_params->custom_proxy_config_client_receiver =
      client.InitWithNewPipeAndPassReceiver();
  if (state.clients_generation == std::numeric_limits<uint64_t>::max()) {
    return false;
  }
  ++state.clients_generation;
  state.clients.Add(std::move(client));
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  state.clients.set_disconnect_handler(base::BindRepeating(
      [](base::WeakPtr<AccessNetworkContextTransport> self,
         std::string partition, mojo::RemoteSetElementId) {
        if (!self) return;
        auto found = self->partitions_.find(partition);
        if (found == self->partitions_.end()) return;
        auto& current = found->second;
        if (!current.account_fenced.empty() || !current.account_sites.empty())
          self->PublishAccountConfig(current);
      }, transport->weak_factory_.GetWeakPtr(), *key));
  if (!state.account_fenced.empty() || !state.account_sites.empty())
    transport->PublishAccountConfig(state);
#endif
  return true;
}

std::optional<aegis_access::OwnershipKey>
AccessNetworkContextTransport::OwnerForPartition(
    aegis_access::ChannelNamespace channel,
    const base::FilePath& relative_partition_path) const {
  if (!aegis_access::IsKnownChannel(channel)) {
    return std::nullopt;
  }
  const std::optional<std::string> partition_token =
      PartitionToken(relative_partition_path);
  if (!partition_token) {
    return std::nullopt;
  }
  return aegis_access::OwnershipKey{channel, runtime_profile_token_,
                                    *partition_token};
}

bool AccessNetworkContextTransport::OwnsConfiguredPartition(
    const aegis_access::OwnershipKey& owner) const {
  return aegis_access::IsCompleteOwner(owner) &&
         owner.profile_token == runtime_profile_token_ &&
         partitions_.contains(owner.storage_partition_token);
}

bool AccessNetworkContextTransport::PublishProxySelection(
    const base::FilePath& relative_partition_path,
    std::vector<std::string> exact_hosts,
    const aegis_access::RegisteredProxyEndpoint& endpoint) {
  const std::optional<std::string> key = PartitionKey(relative_partition_path);
  if (!key ||
      !IsEndpointCurrentForPartition(relative_partition_path, endpoint)) {
    return false;
  }

  std::sort(exact_hosts.begin(), exact_hosts.end());
  if (!AreCanonicalExactHosts(exact_hosts)) {
    return false;
  }

  PartitionState candidate;
  candidate.endpoint = endpoint;
  candidate.exact_hosts = std::move(exact_hosts);
  network::mojom::CustomProxyConfigPtr candidate_config =
      BuildConfig(candidate);
  if (!candidate_config ||
      candidate_config->rules.type ==
          net::ProxyConfig::ProxyRules::Type::EMPTY) {
    return false;
  }

  PartitionState& state = partitions_[*key];
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  if (state.account_config) return false;
#endif
  state.endpoint = std::move(candidate.endpoint);
  state.exact_hosts = std::move(candidate.exact_hosts);
  Broadcast(state);
  return true;
}

#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
const std::vector<std::string>& AccessNetworkContextTransport::AccountSites() const {
  static const base::NoDestructor<std::vector<std::string>> empty;
  const auto it = partitions_.find("<default>");
  return it == partitions_.end() ? *empty : it->second.account_sites;
}

bool AccessNetworkContextTransport::AccountDispatchBlocked(const GURL& url) const {
  auto it = partitions_.find("<default>");
  if (it == partitions_.end()) return false;
  std::string host(url.host());
  while (!host.empty() && host.back() == '.') host.pop_back();
  return it->second.account_fenced.contains(host);
}

void AccessNetworkContextTransport::PublishAccountConfig(PartitionState& state) {
  const auto revision = ++state.account_publication;
  if (state.clients.empty()) return;
  auto config = BuildConfig(state);
  if (!config) return;
  UpdateClientConfigs(state, config, base::BindOnce(
      [](base::WeakPtr<AccessNetworkContextTransport> self, uint64_t revision, bool accepted) {
        if (!self || !accepted) return;
        auto& current = self->partitions_.at("<default>");
        if (current.account_publication == revision) current.account_fenced.clear();
      }, weak_factory_.GetWeakPtr(), revision));
}

bool AccessNetworkContextTransport::SetAccountNodeSites(
    const std::vector<std::string>& sites, const std::string& host, uint16_t port,
    const std::string& authorization) {
  auto& state = partitions_["<default>"];
  if (sites.empty()) {
    state.account_fenced.insert(state.account_sites.begin(), state.account_sites.end());
    state.account_sites.clear();
    state.account_config.reset();
    PublishAccountConfig(state);
    return true;
  }
  // Private request routes carry their own per-request endpoint. Only the
  // legacy partition-wide custom proxy competes for this configuration.
  if (!AreCanonicalExactHosts(sites) || (state.endpoint && !state.private_routing))
    return false;
  auto config = network::mojom::CustomProxyConfig::New();
  auto blocked = net::ProxyServer::FromSchemeHostAndPort(
      net::ProxyServer::SCHEME_HTTPS, "blocked.invalid", 443);
  auto proxy = host.empty() ? blocked : net::ProxyServer::FromSchemeHostAndPort(
      net::ProxyServer::SCHEME_HTTPS, host, port);
  if (!proxy.is_valid() || (!host.empty() && authorization.empty())) return false;
  config->rules.type = net::ProxyConfig::ProxyRules::Type::PROXY_LIST_PER_SCHEME;
  for (const auto& site : sites)
    config->rules.bypass_rules.AddAegisExactSite(site);
  // Keep this single endpoint exclusive to the config. The CONNECT-only node
  // rejects ordinary HTTP and no tunnel header accompanies those requests.
  config->rules.proxies_for_http.SetSingleProxyServer(proxy);
  config->rules.proxies_for_https.SetSingleProxyServer(proxy);
  config->rules.reverse_bypass = true;
  config->should_override_existing_config = true;
  config->allow_non_idempotent_methods = true;
  // An empty value marks a blocked account route during acquisition/logout.
  // The delegate never sends that placeholder on the wire.
  config->connect_tunnel_headers.SetHeader(net::HttpRequestHeaders::kProxyAuthorization,
                                           authorization);
  // The Browser dispatch path and all NetworkContexts have separate pipes.
  // Fence changed exact hosts before publishing, until every recipient ACKs.
  if (!state.account_config ||
      state.account_config->connect_tunnel_headers.GetHeader(net::HttpRequestHeaders::kProxyAuthorization) !=
          config->connect_tunnel_headers.GetHeader(net::HttpRequestHeaders::kProxyAuthorization) ||
      state.account_config->rules.proxies_for_https.AllChains() != config->rules.proxies_for_https.AllChains()) {
    state.account_fenced.insert(state.account_sites.begin(), state.account_sites.end());
    state.account_fenced.insert(sites.begin(), sites.end());
  } else {
    std::set_symmetric_difference(state.account_sites.begin(), state.account_sites.end(),
        sites.begin(), sites.end(), std::inserter(state.account_fenced, state.account_fenced.end()));
  }
  state.account_sites = sites;
  state.account_config = std::move(config);
  PublishAccountConfig(state);
  return true;
}
#endif

bool AccessNetworkContextTransport::ClearProxySelection(
    const base::FilePath& relative_partition_path) {
  const std::optional<std::string> key = PartitionKey(relative_partition_path);
  if (!key) {
    return false;
  }
  auto it = partitions_.find(*key);
  if (it == partitions_.end()) {
    return true;
  }
  it->second.endpoint.reset();
  it->second.exact_hosts.clear();
  Broadcast(it->second);
  return true;
}

bool AccessNetworkContextTransport::RegisterPreparedProxy(
    const AccessPrivateEndpoint& entry) {
  const auto& endpoint = entry.registration;
  const auto& g = endpoint.generations;
  const bool no_auth = entry.listener_binding &&
      aegis_access::IsValidAccessPreparedListenerBinding(*entry.listener_binding) &&
      !entry.local_credentials;
  const bool legacy_http = !entry.listener_binding &&
      endpoint.transport == aegis_access::RegisteredProxyTransport::kHttp &&
      entry.local_credentials && !entry.local_credentials->username().empty() &&
      !entry.local_credentials->password().empty();
  if (!OwnsConfiguredPartition(endpoint.owner) ||
      (endpoint.transport != aegis_access::RegisteredProxyTransport::kHttp &&
       endpoint.transport != aegis_access::RegisteredProxyTransport::kSocks5) ||
      (endpoint.transport == aegis_access::RegisteredProxyTransport::kSocks5 && endpoint.port == 1080) ||
      (endpoint.host != "127.0.0.1" && endpoint.host != "::1") || !endpoint.port ||
      endpoint.proxy_group_id.empty() || endpoint.registration_id.empty() ||
      !g.policy_generation || !g.identity_generation || !g.selection_generation ||
      g.network_epoch != network_epoch_ || !g.base_proxy_config_generation ||
      (!no_auth && !legacy_http)) return false;
  auto& state = partitions_.at(endpoint.owner.storage_partition_token);
  if (no_auth) {
    if (!state.private_routing) return false;
    const auto live = state.private_endpoints.find(endpoint.proxy_group_id);
    if (live != state.private_endpoints.end() &&
        live->second.registration.registration_id != endpoint.registration_id) {
      return false;
    }
    const auto retiring = state.retiring_test_listeners.find(endpoint.proxy_group_id);
    if (retiring != state.retiring_test_listeners.end() &&
        (!retiring->second.acknowledged ||
         retiring->second.registration_id == endpoint.registration_id)) {
      return false;
    }
  }
  for (const auto& [group, old] : state.private_endpoints) {
    if (old.registration.registration_id == endpoint.registration_id &&
        (group != endpoint.proxy_group_id || old.registration.host != endpoint.host ||
         old.registration.port != endpoint.port ||
         old.registration.transport != endpoint.transport ||
         old.listener_binding != entry.listener_binding ||
         old.local_credentials.has_value() != entry.local_credentials.has_value() ||
         (old.local_credentials && !old.local_credentials->Equals(*entry.local_credentials)))) return false;
  }
  state.private_endpoints.insert_or_assign(endpoint.proxy_group_id, entry);
  state.private_unavailable_generations.erase(endpoint.proxy_group_id);
  if (!no_auth) state.endpoint = endpoint;
  return true;
}
std::vector<AccessPrivateEndpoint> AccessNetworkContextTransport::PrivateEndpoints(
    const aegis_access::OwnershipKey& owner) const {
  std::vector<AccessPrivateEndpoint> endpoints;
  if (!OwnsConfiguredPartition(owner)) return endpoints;
  for (const auto& [group, entry] : partitions_.at(owner.storage_partition_token).private_endpoints)
    endpoints.push_back(entry);
  return endpoints;
}
bool AccessNetworkContextTransport::ForgetPreparedProxy(
    const aegis_access::OwnershipKey& owner, const std::string& group,
    const std::string& registration_id) {
  if (!OwnsConfiguredPartition(owner)) return false;
  auto& state = partitions_.at(owner.storage_partition_token);
  const auto found = state.private_endpoints.find(group);
  if (found == state.private_endpoints.end() ||
      found->second.registration.owner != owner ||
      found->second.registration.registration_id != registration_id) return false;
  state.private_unavailable_generations.insert_or_assign(
      group, found->second.registration.generations.selection_generation);
  state.private_endpoints.erase(found);
  if (state.endpoint && state.endpoint->registration_id == registration_id)
    state.endpoint.reset();
  return true;
}

void AccessNetworkContextTransport::SetTestListenerLostCallback(
    TestListenerLostCallback callback) {
  test_listener_lost_ = std::move(callback);
}

void AccessNetworkContextTransport::InvalidateTestListenersAfterContextLoss(
    const aegis_access::OwnershipKey& owner, PartitionState& state) {
  std::vector<std::pair<std::string, std::string>> lost;
  for (const auto& [group, entry] : state.private_endpoints) {
    if (entry.listener_binding && entry.registration.owner == owner)
      lost.emplace_back(group, entry.registration.registration_id);
  }
  if (lost.empty()) return;
  for (const auto& [group, registration_id] : lost) {
    const auto selection =
        state.private_endpoints.at(group).registration.generations.selection_generation;
    ForgetPreparedProxy(owner, group, registration_id);
    // The listener keeps its address until its owner confirms retirement.
    // Until then no registration for this group can be installed again.
    state.retiring_test_listeners.insert_or_assign(
        group, PartitionState::RetiringListener{registration_id});
    if (state.private_publication) {
      auto& publication = *state.private_publication;
      std::erase_if(publication.endpoints, [&](const auto& published) {
        return published.registration.registration_id == registration_id;
      });
      publication.unavailable.insert_or_assign(group, selection);
    }
  }
  if (state.private_publication) ++state.private_revision;
  for (const auto& [incarnation, entry] : state.private_contexts)
    PublishPrivateContext(owner, incarnation);
  if (!test_listener_lost_) return;
  for (const auto& [group, registration_id] : lost) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(test_listener_lost_, owner, group, registration_id));
  }
}

std::optional<aegis_access::RegisteredProxyEndpoint>
AccessNetworkContextTransport::PreparedProxyForGroup(
    const aegis_access::OwnershipKey& owner, const std::string& group) const {
  if (!OwnsConfiguredPartition(owner)) return std::nullopt;
  const auto& state = partitions_.at(owner.storage_partition_token);
  const auto found = state.private_endpoints.find(group);
  if (found != state.private_endpoints.end() &&
      found->second.registration.owner == owner) {
    return found->second.registration;
  }
  if (!state.private_routing && state.endpoint &&
      state.endpoint->owner == owner && state.endpoint->proxy_group_id == group) {
    return state.endpoint;
  }
  return std::nullopt;
}

void AccessNetworkContextTransport::RetirePreparedTestListener(
    const aegis_access::PublishedAccessPolicySnapshot& policy,
    const aegis_access::GenerationTuple& common,
    const std::string& group, const std::string& registration_id,
    base::OnceCallback<void(bool)> completion) {
  if (!OwnsConfiguredPartition(policy.owner)) {
    std::move(completion).Run(false);
    return;
  }
  auto& state = partitions_.at(policy.owner.storage_partition_token);
  const auto endpoint = state.private_endpoints.find(group);
  auto retiring = state.retiring_test_listeners.find(group);
  if (endpoint != state.private_endpoints.end()) {
    if (!endpoint->second.listener_binding ||
        !ForgetPreparedProxy(policy.owner, group, registration_id)) {
      std::move(completion).Run(false);
      return;
    }
    retiring = state.retiring_test_listeners.insert_or_assign(
        group, PartitionState::RetiringListener{registration_id}).first;
  }
  if (retiring == state.retiring_test_listeners.end() ||
      retiring->second.registration_id != registration_id) {
    std::move(completion).Run(false);
    return;
  }
  if (retiring->second.acknowledged) {
    std::move(completion).Run(true);
    return;
  }
  retiring->second.completion = std::move(completion);
  if (PublishPrivatePolicy(policy, common, base::DoNothing()).status ==
      AccessNetworkConfigAckStatus::kInvalidPublication) {
    std::move(retiring->second.completion).Run(false);
  }
}
bool AccessNetworkContextTransport::HasReadyPrivateContext(
    const aegis_access::OwnershipKey& owner) const {
  if (!OwnsConfiguredPartition(owner)) return false;
  const auto& state = partitions_.at(owner.storage_partition_token);
  const auto& contexts = state.private_contexts;
  return !contexts.empty() && std::ranges::all_of(contexts,
      [&](const auto& entry) {
        return !entry.second.install_attempt && entry.second.client->is_ready() &&
            entry.second.applied_revision == state.private_revision;
      });
}

bool AccessNetworkContextTransport::HasCurrentPrivatePolicy(
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& common) const {
  if (!HasReadyPrivateContext(owner)) return false;
  const auto& publication =
      partitions_.at(owner.storage_partition_token).private_publication;
  return publication && publication->common == common;
}

#if BUILDFLAG(IS_MAC)
bool AccessNetworkContextTransport::HasAcknowledgedTestListener(
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& common, const std::string& group,
    const std::string& registration_id) const {
  return HasAcknowledgedPrivateProxy(owner, common, group, registration_id,
                                     /*test_listener=*/true);
}

bool AccessNetworkContextTransport::HasAcknowledgedLegacyProxy(
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& common, const std::string& group,
    const std::string& registration_id) const {
  return HasAcknowledgedPrivateProxy(owner, common, group, registration_id,
                                     /*test_listener=*/false);
}

bool AccessNetworkContextTransport::HasAcknowledgedPrivateProxy(
    const aegis_access::OwnershipKey& owner,
    const aegis_access::GenerationTuple& common, const std::string& group,
    const std::string& registration_id, bool test_listener) const {
  if (!HasCurrentPrivatePolicy(owner, common) ||
      common.network_epoch != network_epoch_) {
    return false;
  }
  const auto& state = partitions_.at(owner.storage_partition_token);
  const auto live = state.private_endpoints.find(group);
  if (live == state.private_endpoints.end() ||
      live->second.listener_binding.has_value() != test_listener ||
      live->second.registration.owner != owner ||
      live->second.registration.registration_id != registration_id) {
    return false;
  }
  return std::ranges::any_of(state.private_publication->endpoints,
      [&](const auto& published) {
        return published.registration == live->second.registration &&
               published.listener_binding == live->second.listener_binding;
      });
}
#endif

AccessNetworkContextTransport::PrivateContextEntry::PrivateContextEntry(
    PrivateContext client) : client(std::move(client)) {}
AccessNetworkContextTransport::PrivateContextEntry::~PrivateContextEntry() = default;

void AccessNetworkContextTransport::BindPrivateContext(
    const aegis_access::OwnershipKey& owner, network::mojom::NetworkContext* context,
    base::OnceCallback<void(PrivateContext)> completion) {
  if (!context || !OwnsConfiguredPartition(owner)) {
    std::move(completion).Run(nullptr);
    return;
  }
  auto& state = partitions_.at(owner.storage_partition_token);
  if (!state.private_routing) {
    state.private_routing = true;
    // Clear any earlier legacy selection before the private capability can
    // become usable. Subsequent Context creation also starts with empty config.
    Broadcast(state);
  }
  auto client = std::make_shared<AccessPrivateContextClient>(owner);
  client->Initialize(context, base::BindOnce(
      [](base::WeakPtr<AccessNetworkContextTransport> self,
         aegis_access::OwnershipKey owner, PrivateContext client,
         base::OnceCallback<void(PrivateContext)> completion, bool initialized) {
        if (!self || !initialized) {
          std::move(completion).Run(nullptr);
          return;
        }
        self->AdmitPrivateContext(std::move(owner), std::move(client),
                                  std::move(completion));
      }, weak_factory_.GetWeakPtr(), owner, client, std::move(completion)));
}

void AccessNetworkContextTransport::AdmitPrivateContext(
    aegis_access::OwnershipKey owner, PrivateContext client,
    base::OnceCallback<void(PrivateContext)> completion) {
  if (!OwnsConfiguredPartition(owner) || !client->is_connected()) {
    std::move(completion).Run(nullptr);
    return;
  }
  auto& state = partitions_.at(owner.storage_partition_token);
  const auto previous = state.private_contexts.find(client->incarnation());
  if (previous == state.private_contexts.end()) {
    FinishPrivateContextBind(std::move(owner), std::move(client), {},
                             std::move(completion), false);
    return;
  }
  // The old control disconnect and the new bind use distinct pipes. Ask the
  // existing control for a response issued after the new handshake before
  // reusing it. Dropping the old pipe may settle the query while its entry is
  // being erased, so complete the bind in a later task.
  const auto& previous_client = previous->second.client;
  const auto previous_control = previous_client->control_id();
  previous_client->CheckConnected(base::BindPostTaskToCurrentDefault(base::BindOnce(
      [](base::WeakPtr<AccessNetworkContextTransport> self,
         aegis_access::OwnershipKey owner, PrivateContext client,
         base::UnguessableToken previous_control,
         base::OnceCallback<void(PrivateContext)> completion, bool alive) {
        if (!self) {
          std::move(completion).Run(nullptr);
          return;
        }
        self->FinishPrivateContextBind(std::move(owner), std::move(client),
            previous_control, std::move(completion), alive);
      }, weak_factory_.GetWeakPtr(), std::move(owner), std::move(client),
      previous_control, std::move(completion))));
}

void AccessNetworkContextTransport::FinishPrivateContextBind(
    aegis_access::OwnershipKey owner, PrivateContext client,
    base::UnguessableToken previous_control,
    base::OnceCallback<void(PrivateContext)> completion, bool previous_alive) {
  if (!OwnsConfiguredPartition(owner) || !client->is_connected()) {
    std::move(completion).Run(nullptr);
    return;
  }
  const auto service_process = NetworkServiceProcessFor(client->process_id());
  if (!service_process) {
    std::move(completion).Run(nullptr);
    return;
  }
  auto& state = partitions_.at(owner.storage_partition_token);
  const auto incarnation = client->incarnation();
  const auto previous = state.private_contexts.find(incarnation);
  if (previous != state.private_contexts.end()) {
    if (previous->second.service_process != *service_process) {
      std::move(completion).Run(nullptr);
      return;
    }
    if (previous->second.client->control_id() != previous_control) {
      AdmitPrivateContext(std::move(owner), std::move(client), std::move(completion));
      return;
    }
    if (previous_alive && previous->second.client->is_connected()) {
      std::move(completion).Run(previous->second.client);
      return;
    }
    ForgetPrivateControl(owner, incarnation);
  }
  auto [entry, inserted] = state.private_contexts.try_emplace(incarnation, client);
  CHECK(inserted);
  entry->second.service_process = *service_process;
  if (const auto lost = state.disconnected_private_contexts.find(incarnation);
      lost != state.disconnected_private_contexts.end()) {
    entry->second.sequence = lost->second.sequence;
    entry->second.possible_listeners = lost->second.possible_listeners;
  }
  client->SetDisconnectHandler(base::BindOnce(
      &AccessNetworkContextTransport::OnPrivateContextDisconnected,
      weak_factory_.GetWeakPtr(), owner, incarnation, client->control_id()));
  client->SetRetirementHandler(base::BindOnce(
      &AccessNetworkContextTransport::OnPrivateContextRetired,
      weak_factory_.GetWeakPtr(), owner, incarnation));
  if (std::ranges::any_of(state.retiring_test_listeners,
          [](const auto& item) { return !!item.second.completion; })) {
    PublishPrivateContext(owner, incarnation);
  }
  std::move(completion).Run(std::move(client));
}

void AccessNetworkContextTransport::OnPrivateContextDisconnected(
    aegis_access::OwnershipKey owner, std::string incarnation,
    base::UnguessableToken control, bool retired) {
  if (retired) {
    OnPrivateContextRetired(std::move(owner), std::move(incarnation));
    return;
  }
  if (!OwnsConfiguredPartition(owner)) return;
  auto& state = partitions_.at(owner.storage_partition_token);
  const auto context = state.private_contexts.find(incarnation);
  if (context == state.private_contexts.end() ||
      context->second.client->control_id() != control) {
    return;
  }
  ForgetPrivateControl(owner, incarnation);
  if (HasReadyPrivateContext(owner)) SettlePrivatePublication(state, true);
}

void AccessNetworkContextTransport::ForgetPrivateControl(
    const aegis_access::OwnershipKey& owner, const std::string& incarnation) {
  auto& state = partitions_.at(owner.storage_partition_token);
  const auto context = state.private_contexts.find(incarnation);
  CHECK(context != state.private_contexts.end());
  if (context->second.sequence) {
    state.disconnected_private_contexts.insert_or_assign(
        incarnation, PartitionState::DisconnectedContext{
                         context->second.sequence, context->second.service_process,
                         std::move(context->second.possible_listeners),
                         context->second.client});
  }
  state.private_contexts.erase(context);
  SettlePrivatePublication(state, false);
  InvalidateTestListenersAfterContextLoss(owner, state);
}

void AccessNetworkContextTransport::OnPrivateContextRetired(
    aegis_access::OwnershipKey owner, std::string incarnation) {
  if (!OwnsConfiguredPartition(owner)) return;
  auto& state = partitions_.at(owner.storage_partition_token);
  const bool connected = state.private_contexts.erase(incarnation) != 0;
  const bool disconnected = state.disconnected_private_contexts.erase(incarnation) != 0;
  if (!connected && !disconnected) return;
  if (connected) {
    SettlePrivatePublication(state, false);
    InvalidateTestListenersAfterContextLoss(owner, state);
  }
  if (HasReadyPrivateContext(owner)) {
    SettlePrivatePublication(state, true);
  } else if (state.private_contexts.empty()) {
    SettleTestListenerRetirements(state, true);
  }
}

bool AccessNetworkContextTransport::SamePrivatePublication(
    const PrivatePublication& a, const PrivatePublication& b) {
  if (a.policy != b.policy || a.common != b.common || a.unavailable != b.unavailable ||
      a.endpoints.size() != b.endpoints.size()) return false;
  for (size_t i = 0; i < a.endpoints.size(); ++i) {
    const auto& left = a.endpoints[i];
    const auto& right = b.endpoints[i];
    if (left.registration != right.registration || left.listener_binding != right.listener_binding ||
        left.local_credentials.has_value() != right.local_credentials.has_value() ||
        (left.local_credentials && !left.local_credentials->Equals(*right.local_credentials)))
      return false;
  }
  return true;
}
AccessNetworkConfigAckResult AccessNetworkContextTransport::PublishPrivatePolicy(
    const aegis_access::PublishedAccessPolicySnapshot& policy,
    const aegis_access::GenerationTuple& common,
    base::OnceCallback<void(bool)> completion) {
  if (!OwnsConfiguredPartition(policy.owner) ||
      aegis_access::ValidateAccessPolicySnapshot(policy) != aegis_access::PolicyMatchReason::kNone ||
      common.policy_generation != policy.policy_generation || !common.policy_generation ||
      !common.identity_generation || !common.network_epoch ||
      common.network_epoch != network_epoch_ || !common.base_proxy_config_generation ||
      common.selection_generation != 0) {
    std::move(completion).Run(false);
    return {AccessNetworkConfigAckStatus::kInvalidPublication, 0};
  }
  auto& state = partitions_.at(policy.owner.storage_partition_token);
  PrivatePublication candidate{policy, common, {}, {}};
  std::set<std::string> groups;
  for (const auto& rule : policy.rules)
    if (rule.mode == aegis_access::AccessMode::kProxy) groups.insert(rule.proxy_group_id);
  for (const auto& group : groups) {
    auto found = state.private_endpoints.find(group);
    if (found != state.private_endpoints.end()) {
      auto& entry = found->second;
      auto& g = entry.registration.generations;
      if (g.identity_generation == common.identity_generation &&
          g.network_epoch == common.network_epoch &&
          g.base_proxy_config_generation == common.base_proxy_config_generation) {
        g.policy_generation = common.policy_generation;
        candidate.endpoints.push_back(entry);
        continue;
      }
      candidate.unavailable.emplace(group, g.selection_generation);
    } else {
      // This process has not selected an endpoint for the restored group.
      const auto high_water = state.private_unavailable_generations.find(group);
      candidate.unavailable.emplace(group,
          high_water == state.private_unavailable_generations.end() ? 0 : high_water->second);
    }
  }
  if (!state.private_publication || !SamePrivatePublication(*state.private_publication, candidate)) {
    SettlePrivatePublication(state, false);
    state.private_publication = std::move(candidate);
    ++state.private_revision;
  }
  if (state.private_contexts.empty()) {
    state.private_publication_waiters.push_back(std::move(completion));
    SettlePrivatePublication(state, false);
    return {AccessNetworkConfigAckStatus::kNoClients, 0};
  }
  if (BUILDFLAG(IS_MAC) && std::ranges::any_of(state.private_contexts,
      [now = base::TimeTicks::Now()](const auto& item) {
        return item.second.InstallExpired(now);
      })) {
    // Keep the desired snapshot for catch-up, but fail callers while a live
    // Context still holds an expired install. Other Contexts can still catch up.
    SettlePrivatePublication(state, false);
    SettleTestListenerRetirements(state, false);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(completion), false));
  } else {
    state.private_publication_waiters.push_back(std::move(completion));
  }
  const size_t required = state.private_contexts.size();
  for (const auto& [incarnation, entry] : state.private_contexts)
    PublishPrivateContext(policy.owner, incarnation);
  if (HasReadyPrivateContext(policy.owner)) SettlePrivatePublication(state, true);
  return {AccessNetworkConfigAckStatus::kStarted, required};
}
void AccessNetworkContextTransport::PublishPrivateContext(
    const aegis_access::OwnershipKey& owner, const std::string& incarnation) {
  auto& state = partitions_.at(owner.storage_partition_token);
  auto& entry = state.private_contexts.at(incarnation);
  if (entry.install_attempt || !state.private_publication ||
      (entry.applied_revision == state.private_revision && entry.client->is_ready())) return;
  const auto& publication = *state.private_publication;
  for (const auto& endpoint : publication.endpoints) {
    if (endpoint.listener_binding)
      entry.possible_listeners.insert(endpoint.registration.registration_id);
  }
  entry.install_attempt = ++private_install_attempt_;
  if (BUILDFLAG(IS_MAC)) {
    entry.install_deadline = base::TimeTicks::Now() + kPrivateInstallTimeout;
    entry.install_timer.Start(FROM_HERE, kPrivateInstallTimeout, base::BindOnce(
        &AccessNetworkContextTransport::OnPrivateContextInstallTimeout,
        weak_factory_.GetWeakPtr(), owner, incarnation, entry.install_attempt));
  }
  entry.client->PublishPolicy(publication.policy, publication.common, ++entry.sequence,
      publication.endpoints, publication.unavailable,
      base::BindOnce(&AccessNetworkContextTransport::OnPrivateContextPublished,
          weak_factory_.GetWeakPtr(), owner, incarnation, entry.install_attempt,
          state.private_revision));
}
void AccessNetworkContextTransport::OnPrivateContextInstallTimeout(
    aegis_access::OwnershipKey owner, std::string incarnation, uint64_t attempt) {
  auto found = partitions_.find(owner.storage_partition_token);
  if (found == partitions_.end()) return;
  auto& state = found->second;
  const auto entry = state.private_contexts.find(incarnation);
  if (entry == state.private_contexts.end() || entry->second.install_attempt != attempt)
    return;
  SettlePrivatePublication(state, false);
  SettleTestListenerRetirements(state, false);
}
void AccessNetworkContextTransport::OnPrivateContextPublished(
    aegis_access::OwnershipKey owner, std::string incarnation,
    uint64_t attempt, uint64_t revision, bool accepted) {
  auto found = partitions_.find(owner.storage_partition_token);
  if (found == partitions_.end()) return;
  auto& state = found->second;
  auto entry = state.private_contexts.find(incarnation);
  if (entry == state.private_contexts.end() || entry->second.install_attempt != attempt)
    return;
  auto& install = entry->second;
  const bool expired = BUILDFLAG(IS_MAC) &&
      install.InstallExpired(base::TimeTicks::Now());
  install.install_timer.Stop();
  install.install_attempt = 0;
  install.install_deadline = {};
  if (expired) {
    install.applied_revision = 0;
    SettlePrivatePublication(state, false);
    SettleTestListenerRetirements(state, false);
    if (install.client->incarnation() != incarnation ||
        state.private_publication->common.network_epoch != network_epoch_) return;
  }
  if (expired || revision != state.private_revision) {
    // The old reply only clears the in-flight install. Even an unchanged
    // desired revision needs a fresh sequence and ACK after a timeout.
    PublishPrivateContext(owner, incarnation);
    return;
  }
  if (!accepted) {
    SettlePrivatePublication(state, false);
    if (install.client->is_connected()) SettleTestListenerRetirements(state, false);
    return;
  }
  state.disconnected_private_contexts.erase(incarnation);
  install.possible_listeners.clear();
  for (const auto& endpoint : state.private_publication->endpoints) {
    if (endpoint.listener_binding)
      install.possible_listeners.insert(endpoint.registration.registration_id);
  }
  entry->second.applied_revision = revision;
  if (HasReadyPrivateContext(owner)) SettlePrivatePublication(state, true);
}
void AccessNetworkContextTransport::SettlePrivatePublication(PartitionState& state,
                                                            bool accepted) {
  auto callbacks = std::move(state.private_publication_waiters);
  state.private_publication_waiters.clear();
  for (auto& callback : callbacks) base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), accepted));
  if (accepted) SettleTestListenerRetirements(state, true);
}

void AccessNetworkContextTransport::SettleTestListenerRetirements(
    PartitionState& state, bool accepted) {
  for (auto& [group, listener] : state.retiring_test_listeners) {
    if (!listener.completion) continue;
    if (accepted) {
      if (!state.private_publication ||
          std::ranges::any_of(state.disconnected_private_contexts,
              [&](const auto& entry) {
                return entry.second.possible_listeners.contains(listener.registration_id);
              }) ||
          state.private_publication->common.network_epoch != network_epoch_ ||
          state.private_endpoints.contains(group) ||
          std::ranges::any_of(state.private_publication->endpoints,
              [&](const auto& endpoint) {
                return endpoint.registration.registration_id == listener.registration_id;
              })) {
        continue;
      }
      listener.acknowledged = true;
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(listener.completion), accepted));
  }
}

std::optional<aegis_access::RegisteredProxyEndpoint>
AccessNetworkContextTransport::CaptureSelectedProxyEndpoint(
    const aegis_access::OwnershipKey& owner,
    const std::string& proxy_group_id,
    const std::string& exact_host) const {
  if (!aegis_access::IsCompleteOwner(owner) || proxy_group_id.empty() ||
      !IsCanonicalExactHost(exact_host) || !OwnsConfiguredPartition(owner) ||
      network_epoch_ == 0) {
    return std::nullopt;
  }

  const auto it = partitions_.find(owner.storage_partition_token);
  if (it != partitions_.end()) {
    const auto entry = it->second.private_endpoints.find(proxy_group_id);
    if (entry != it->second.private_endpoints.end()) {
      const auto& endpoint = entry->second.registration;
      return endpoint.owner == owner && endpoint.generations.network_epoch == network_epoch_
          ? std::make_optional(endpoint) : std::nullopt;
    }
  }
  // PublishProxySelection() sorts exact_hosts before committing PartitionState.
  if (it == partitions_.end() || !it->second.endpoint.has_value() ||
      !std::binary_search(it->second.exact_hosts.begin(),
                          it->second.exact_hosts.end(), exact_host)) {
    return std::nullopt;
  }

  const aegis_access::RegisteredProxyEndpoint& endpoint = *it->second.endpoint;
  if (endpoint.owner != owner || endpoint.proxy_group_id != proxy_group_id ||
      endpoint.generations.network_epoch != network_epoch_) {
    return std::nullopt;
  }
  return endpoint;
}

AccessNetworkConfigAckResult
AccessNetworkContextTransport::RepublishCurrentConfigWithAck(
    const aegis_access::OwnershipKey& owner,
    base::OnceCallback<void(bool)> all_clients_settled) {
  if (!all_clients_settled) {
    return {AccessNetworkConfigAckStatus::kInvalidOwner, 0};
  }
  auto fail = [&](AccessNetworkConfigAckStatus status) {
    std::move(all_clients_settled).Run(false);
    return AccessNetworkConfigAckResult{status, 0};
  };
  if (!OwnsConfiguredPartition(owner)) {
    return fail(AccessNetworkConfigAckStatus::kInvalidOwner);
  }
  auto it = partitions_.find(owner.storage_partition_token);
  if (it == partitions_.end()) {
    return fail(AccessNetworkConfigAckStatus::kMissingPartition);
  }
  PartitionState& state = it->second;
  if (state.clients.empty()) {
    return fail(AccessNetworkConfigAckStatus::kNoClients);
  }
  network::mojom::CustomProxyConfigPtr config = BuildConfig(state);
  if (!config) {
    return fail(AccessNetworkConfigAckStatus::kBuildFailed);
  }

  const size_t required_acks = state.clients.size();
  UpdateClientConfigs(state, config, std::move(all_clients_settled));
  return {AccessNetworkConfigAckStatus::kStarted, required_acks};
}

AccessNetworkConfigAckResult
AccessNetworkContextTransport::PublishPolicyCandidateWithAck(
    const aegis_access::PolicyPublicationIdentity& identity,
    const aegis_access::OwnershipKey& owner,
    base::OnceCallback<void(bool)> all_clients_settled) {
  if (!all_clients_settled) {
    return {AccessNetworkConfigAckStatus::kInvalidPublication, 0};
  }
  auto fail = [&](AccessNetworkConfigAckStatus status) {
    std::move(all_clients_settled).Run(false);
    return AccessNetworkConfigAckResult{status, 0};
  };
  if (!OwnsConfiguredPartition(owner) || identity.selector.owner != owner) {
    return fail(AccessNetworkConfigAckStatus::kInvalidOwner);
  }
  if (!IsCandidateIdentityValid(identity, network_epoch_)) {
    return fail(AccessNetworkConfigAckStatus::kInvalidPublication);
  }
  auto it = partitions_.find(owner.storage_partition_token);
  if (it == partitions_.end()) {
    return fail(AccessNetworkConfigAckStatus::kMissingPartition);
  }
  PartitionState& state = it->second;
  if (state.clients.empty()) {
    return fail(AccessNetworkConfigAckStatus::kNoClients);
  }
  if (!SelectionMatchesIdentity(state, identity)) {
    return fail(AccessNetworkConfigAckStatus::kInvalidPublication);
  }

  const size_t required_acks = state.clients.size();
  PublishPolicyCandidateToClients(state, identity, owner,
                                  std::move(all_clients_settled));
  return {AccessNetworkConfigAckStatus::kStarted, required_acks};
}

std::optional<aegis_access::RegisteredProxyEndpoint>
AccessNetworkContextTransport::CurrentEndpoint(
    const aegis_access::OwnershipKey& owner) const {
  if (!OwnsConfiguredPartition(owner)) {
    return std::nullopt;
  }
  return partitions_.at(owner.storage_partition_token).endpoint;
}

std::optional<AccessTransportSelection>
AccessNetworkContextTransport::CurrentSelection(
    const aegis_access::OwnershipKey& owner) const {
  if (!OwnsConfiguredPartition(owner)) {
    return std::nullopt;
  }
  const auto& state = partitions_.at(owner.storage_partition_token);
  return AccessTransportSelection{state.endpoint, state.exact_hosts};
}

bool AccessNetworkContextTransport::ReplaceSelection(
    const aegis_access::OwnershipKey& owner,
    const AccessTransportSelection& expected,
    const AccessTransportSelection& replacement) {
  const bool private_context = OwnsConfiguredPartition(owner) &&
      partitions_.at(owner.storage_partition_token).private_routing;
  if (CurrentSelection(owner) != expected ||
      (replacement.endpoint && replacement.endpoint->owner != owner) ||
      (!replacement.exact_hosts.empty() &&
       ((!replacement.endpoint && !private_context) ||
        !AreCanonicalExactHosts(replacement.exact_hosts)))) {
    return false;
  }
  auto& state = partitions_.at(owner.storage_partition_token);
  state.endpoint = replacement.endpoint;
  state.exact_hosts = replacement.exact_hosts;
  if (replacement.endpoint) {
    auto found = state.private_endpoints.find(replacement.endpoint->proxy_group_id);
    if (found != state.private_endpoints.end() &&
        found->second.registration.registration_id == replacement.endpoint->registration_id)
      found->second.registration = *replacement.endpoint;
  }
  // The policy metadata is sent after this config on the same Mojo pipe.
  if (!private_context) Broadcast(state);
  return true;
}

bool AccessNetworkContextTransport::SelectionMatchesIdentity(
    const PartitionState& state,
    const aegis_access::PolicyPublicationIdentity& identity) {
  if (identity.selection_generation == 0) {
    return true;
  }
  return state.endpoint && state.endpoint->owner == identity.selector.owner &&
         state.endpoint->proxy_group_id == identity.proxy_group_id &&
         state.endpoint->generations.selection_generation ==
             identity.selection_generation &&
         state.endpoint->generations.network_epoch == identity.network_epoch &&
         std::binary_search(state.exact_hosts.begin(), state.exact_hosts.end(),
                            identity.selector.exact_host);
}

bool AccessNetworkContextTransport::PublicationStillCurrent(
    const aegis_access::PolicyPublicationIdentity& identity,
    uint64_t clients_generation,
    size_t client_count) const {
  if (network_epoch_ != identity.network_epoch) {
    return false;
  }
  const auto it =
      partitions_.find(identity.selector.owner.storage_partition_token);
  return it != partitions_.end() &&
         it->second.clients_generation == clients_generation &&
         it->second.clients.size() == client_count &&
         SelectionMatchesIdentity(it->second, identity);
}

void AccessNetworkContextTransport::PublishPolicyCandidateToClients(
    PartitionState& state,
    const aegis_access::PolicyPublicationIdentity& identity,
    const aegis_access::OwnershipKey& owner,
    base::OnceCallback<void(bool)> all_clients_settled) {
  auto metadata = network::mojom::AegisAccessPolicyPublicationMetadata::New();
  metadata->operation_id = identity.operation_id;
  metadata->operation_sequence = identity.operation_sequence;
  metadata->policy_generation = identity.policy_generation;
  metadata->proxy_group_id = identity.proxy_group_id;
  metadata->selection_generation = identity.selection_generation;
  metadata->network_epoch = identity.network_epoch;
  metadata->channel = static_cast<uint32_t>(owner.channel);
  metadata->profile_token = owner.profile_token;
  metadata->storage_partition_token = owner.storage_partition_token;

  auto all_succeeded = std::make_shared<bool>(true);
  base::RepeatingClosure barrier = base::BarrierClosure(
      state.clients.size(),
      base::BindOnce(
          [](base::WeakPtr<AccessNetworkContextTransport> transport,
             aegis_access::PolicyPublicationIdentity identity,
             uint64_t clients_generation, size_t client_count,
             std::shared_ptr<bool> succeeded,
             base::OnceCallback<void(bool)> completion) {
            const bool current =
                transport && transport->PublicationStillCurrent(
                                 identity, clients_generation, client_count);
            std::move(completion).Run(*succeeded && current);
          },
          weak_factory_.GetWeakPtr(), identity, state.clients_generation,
          state.clients.size(), all_succeeded, std::move(all_clients_settled)));

  for (auto& client : state.clients) {
    base::OnceCallback<void(bool)> result =
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](std::shared_ptr<bool> succeeded,
                   base::RepeatingClosure completion, bool client_acked) {
                  *succeeded = *succeeded && client_acked;
                  completion.Run();
                },
                all_succeeded, barrier),
            false);
    client->OnAegisAccessPolicyPublished(metadata->Clone(), std::move(result));
  }
}

void AccessNetworkContextTransport::UpdateClientConfigs(
    PartitionState& state,
    const network::mojom::CustomProxyConfigPtr& config,
    base::OnceCallback<void(bool)> all_clients_settled) {
  auto all_succeeded = std::make_shared<bool>(true);
  base::RepeatingClosure barrier = base::BarrierClosure(
      state.clients.size(),
      base::BindOnce(
          [](std::shared_ptr<bool> succeeded,
             base::OnceCallback<void(bool)> completion) {
            std::move(completion).Run(*succeeded);
          },
          all_succeeded, std::move(all_clients_settled)));

  for (auto& client : state.clients) {
    base::OnceCallback<void(bool)> result =
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](std::shared_ptr<bool> succeeded,
                   base::RepeatingClosure completion, bool client_acked) {
                  *succeeded = *succeeded && client_acked;
                  completion.Run();
                },
                all_succeeded, barrier),
            false);
    client->OnCustomProxyConfigUpdated(
        config->Clone(),
        base::BindOnce(
            [](base::OnceCallback<void(bool)> result_callback) {
              std::move(result_callback).Run(true);
            },
            std::move(result)));
  }
}

network::mojom::CustomProxyConfigPtr
AccessNetworkContextTransport::BuildConfigForTesting(
    const base::FilePath& relative_partition_path) const {
  const std::optional<std::string> key = PartitionKey(relative_partition_path);
  if (!key) {
    return nullptr;
  }
  auto it = partitions_.find(*key);
  if (it == partitions_.end()) {
    return network::mojom::CustomProxyConfig::New();
  }
  return BuildConfig(it->second);
}

void AccessNetworkContextTransport::FlushClientsForTesting(
    const base::FilePath& relative_partition_path) {
  const std::optional<std::string> key = PartitionKey(relative_partition_path);
  if (!key) {
    return;
  }
  auto it = partitions_.find(*key);
  if (it != partitions_.end()) {
    it->second.clients.FlushForTesting();
  }
}

// static
std::optional<std::string> AccessNetworkContextTransport::PartitionKey(
    const base::FilePath& relative_partition_path) {
  if (relative_partition_path.IsAbsolute() ||
      relative_partition_path.ReferencesParent()) {
    return std::nullopt;
  }
  const std::string value = relative_partition_path.AsUTF8Unsafe();
  if (value.size() > kMaxPartitionKeyBytes) {
    return std::nullopt;
  }
  return value.empty() ? std::optional<std::string>("<default>")
                       : std::optional<std::string>("partition:" + value);
}

std::optional<std::string> AccessNetworkContextTransport::PartitionToken(
    const base::FilePath& relative_partition_path) const {
  return PartitionKey(relative_partition_path);
}

bool AccessNetworkContextTransport::IsEndpointCurrentForPartition(
    const base::FilePath& relative_partition_path,
    const aegis_access::RegisteredProxyEndpoint& endpoint) const {
  const std::optional<aegis_access::OwnershipKey> expected_owner =
      OwnerForPartition(endpoint.owner.channel, relative_partition_path);
  return network_epoch_ != 0 && expected_owner.has_value() &&
         endpoint.owner == *expected_owner &&
         endpoint.generations.network_epoch == network_epoch_;
}

network::mojom::CustomProxyConfigPtr AccessNetworkContextTransport::BuildConfig(
    const PartitionState& state) const {
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
  if (state.account_config) return state.account_config->Clone();
#endif
  auto config = network::mojom::CustomProxyConfig::New();
  if (state.private_routing || !state.endpoint.has_value() ||
      state.exact_hosts.empty()) {
    return config;
  }

  net::ProxyInfo proxy_info;
  proxy_info.UseDirect();
  const aegis_access::ProxyRouteApplyStatus status =
      aegis_access::ApplyRoutePlanToProxyInfo(RoutePlanForEndpoint(*state.endpoint),
                                              &*state.endpoint, &proxy_info);
  if (status != aegis_access::ProxyRouteApplyStatus::kAppliedProxy ||
      proxy_info.is_empty() || proxy_info.is_direct()) {
    return nullptr;
  }

  config->rules.type =
      net::ProxyConfig::ProxyRules::Type::PROXY_LIST_PER_SCHEME;
  config->rules.proxies_for_http = proxy_info.proxy_list();
  config->rules.proxies_for_https = proxy_info.proxy_list();
  for (const std::string& host : state.exact_hosts) {
    if (!config->rules.bypass_rules.AddRuleFromString(host)) {
      return nullptr;
    }
  }
  // Positive rules become the only destinations that do NOT bypass this
  // custom proxy. A non-selected URL resolves to DIRECT inside the custom
  // config, causing NetworkServiceProxyDelegate to preserve the native result.
  config->rules.reverse_bypass = true;
  config->should_override_existing_config = true;
  config->allow_non_idempotent_methods = true;
  return config;
}

void AccessNetworkContextTransport::Broadcast(PartitionState& state) {
  network::mojom::CustomProxyConfigPtr config = BuildConfig(state);
  if (!config) {
    return;
  }
  for (auto& client : state.clients) {
    client->OnCustomProxyConfigUpdated(config->Clone(), base::DoNothing());
  }
}

}  // namespace aegis::access
