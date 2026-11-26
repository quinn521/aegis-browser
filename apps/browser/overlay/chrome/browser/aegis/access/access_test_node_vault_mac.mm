// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_test_node_vault_mac.h"

#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>

#include <algorithm>
#include <limits>
#include <utility>

#include "base/apple/bridging.h"
#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/strings/sys_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/aegis/access/access_test_node_native_config.h"
#include "crypto/apple/keychain_v2.h"

namespace aegis::access {
namespace {
using Status = AccessTestNodeVaultStatus;
using Node = aegis_access::AccessTestNode;

std::string ServiceForScope(std::string_view channel, std::string_view scope_hash) {
  if ((channel != "debug" && channel != "alpha") || scope_hash.size() != 64 ||
      !std::ranges::all_of(scope_hash, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })) return {};
  return "app.gcsa.aegis.access.test-node.v1." + std::string(channel) + "." + std::string(scope_hash);
}
using ReferenceKey = std::pair<std::string, std::string>;
struct ReferenceState {
  size_t holders = 0;
  bool deleting = false;
};
base::Lock& ReferenceLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}
std::map<ReferenceKey, ReferenceState>& References() {
  static base::NoDestructor<std::map<ReferenceKey, ReferenceState>> references;
  return *references;
}
// The lock covers metadata only. No native UI thread can wait here for a
// Keychain operation. A racing reservation fails while deletion owns the item.
class ScopedReferenceDeletion {
 public:
  ScopedReferenceDeletion(std::string_view service, std::string_view reference);
  ~ScopedReferenceDeletion();
  ScopedReferenceDeletion(const ScopedReferenceDeletion&) = delete;
  ScopedReferenceDeletion& operator=(const ScopedReferenceDeletion&) = delete;
  bool allowed() const { return allowed_; }
 private:
  ReferenceKey key_;
  bool allowed_ = false;
};
ScopedReferenceDeletion::ScopedReferenceDeletion(std::string_view service,
                                               std::string_view reference)
    : key_(service, reference) {
  base::AutoLock lock(ReferenceLock());
  auto& state = References()[key_];
  if (state.holders || state.deleting) return;
  state.deleting = true;
  allowed_ = true;
}
ScopedReferenceDeletion::~ScopedReferenceDeletion() {
  if (!allowed_) return;
  base::AutoLock lock(ReferenceLock());
  auto found = References().find(key_);
  CHECK(found != References().end() && found->second.deleting);
  found->second.deleting = false;
  if (!found->second.holders) References().erase(found);
}
Status DeleteUnpinned(AccessTestNodeVaultMac::Backend& backend,
                      std::string_view service, std::string_view reference) {
  ScopedReferenceDeletion deletion(service, reference);
  return deletion.allowed() ? backend.Delete(service, reference) : Status::kInUse;
}

class KeychainBackend final : public AccessTestNodeVaultMac::Backend {
 public:
  Status Add(std::string_view service,
             std::string_view account,
             std::string_view secret) override {
    @autoreleasepool {
      NSMutableDictionary* attributes = Query(service, account);
      attributes[(__bridge id)kSecValueData] =
          [NSData dataWithBytes:secret.data() length:secret.size()];
      attributes[(__bridge id)kSecAttrAccessible] =
          (__bridge id)kSecAttrAccessibleWhenUnlockedThisDeviceOnly;
      // ItemAdd is create-only; never replace another operation's record.
      return crypto::apple::KeychainV2::GetInstance().ItemAdd(
                 (__bridge CFDictionaryRef)attributes, nullptr) == errSecSuccess
                 ? Status::kOk
                 : Status::kUnavailable;
    }
  }

  std::optional<std::string> Read(std::string_view service,
                                  std::string_view account) override {
    @autoreleasepool {
      NSMutableDictionary* query = Query(service, account);
      query[(__bridge id)kSecReturnData] = @YES;
      query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
      base::apple::ScopedCFTypeRef<CFTypeRef> value;
      if (crypto::apple::KeychainV2::GetInstance().ItemCopyMatching(
              (__bridge CFDictionaryRef)query, value.InitializeInto()) !=
          errSecSuccess) {
        return std::nullopt;
      }
      const auto data = base::apple::CFCast<CFDataRef>(value.get());
      if (!data || CFDataGetLength(data) <= 0 || CFDataGetLength(data) > 8192) {
        return std::nullopt;
      }
      return std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                         static_cast<size_t>(CFDataGetLength(data)));
    }
  }

  Status Delete(std::string_view service, std::string_view account) override {
    @autoreleasepool {
      const auto result = crypto::apple::KeychainV2::GetInstance().ItemDelete(
          (__bridge CFDictionaryRef)Query(service, account));
      return result == errSecSuccess || result == errSecItemNotFound
                 ? Status::kOk
                 : Status::kUnavailable;
    }
  }

  std::optional<std::vector<std::string>> ListAccounts(
      std::string_view service) override {
    @autoreleasepool {
      NSMutableDictionary* query = Query(service, "");
      [query removeObjectForKey:(__bridge id)kSecAttrAccount];
      query[(__bridge id)kSecReturnAttributes] = @YES;
      query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitAll;
      base::apple::ScopedCFTypeRef<CFTypeRef> value;
      const auto status = crypto::apple::KeychainV2::GetInstance().ItemCopyMatching(
          (__bridge CFDictionaryRef)query, value.InitializeInto());
      if (status == errSecItemNotFound) return std::vector<std::string>{};
      if (status != errSecSuccess) return std::nullopt;
      const auto items = base::apple::CFCast<CFArrayRef>(value.get());
      if (!items || CFArrayGetCount(items) > 1024) return std::nullopt;
      std::vector<std::string> accounts;
      for (CFIndex index = 0; index < CFArrayGetCount(items); ++index) {
        const auto attributes = base::apple::CFCast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, index));
        if (!attributes) return std::nullopt;
        const auto account = base::apple::CFCast<CFStringRef>(
            CFDictionaryGetValue(attributes, kSecAttrAccount));
        if (!account || CFStringGetLength(account) != 36) return std::nullopt;
        accounts.push_back(base::SysCFStringRefToUTF8(account));
      }
      return accounts;
    }
  }

 private:
  static NSMutableDictionary* Query(std::string_view service,
                                     std::string_view account) {
    LAContext* context = [[LAContext alloc] init];
    context.interactionNotAllowed = YES;
    return [@{
      (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
      (__bridge id)kSecAttrService : base::SysUTF8ToNSString(service),
      (__bridge id)kSecAttrAccount : base::SysUTF8ToNSString(account),
      (__bridge id)kSecAttrSynchronizable : @NO,
      (__bridge id)kSecUseAuthenticationContext : context,
    } mutableCopy];
  }
};

bool ValidId(std::string_view value) {
  return base::Uuid::ParseLowercase(value).is_valid();
}

bool RequiresDevChannel(const Node& node) {
  return node.web_socket_tls || node.tcp_tls_vision ||
         node.grpc_reality_service_name;
}

std::optional<std::string> Encode(const Node& node) {
  if (!ValidateAccessTestNodeNative(node)) return std::nullopt;
  auto value = base::DictValue()
      .Set("address", node.address).Set("port", node.port)
      .Set("userId", node.user_id).Set("serverName", node.server_name)
      .Set("fingerprint", node.fingerprint);
  if (!RequiresDevChannel(node)) {
    value.Set("schemaVersion", 1);
    value.Set("publicKey", node.public_key);
    value.Set("shortId", node.short_id);
    value.Set("spiderX", node.spider_x);
    value.Set("shareSupportsX25519Mlkem768", node.advertises_x25519_mlkem768);
  } else {
    value.Set("schemaVersion", 3);
    if (const auto& ws = node.web_socket_tls) {
      value.Set("outbound", "vless.ws.tls");
      value.Set("wsHost", ws->host);
      value.Set("wsPath", ws->path);
      value.Set("wsAlpn", ws->alpn);
    } else if (node.tcp_tls_vision) {
      value.Set("outbound", "vless.raw.tls");
    } else {
      value.Set("outbound", "vless.grpc.reality");
      value.Set("publicKey", node.public_key);
      value.Set("shortId", node.short_id);
      value.Set("serviceName", *node.grpc_reality_service_name);
    }
  }
  std::string encoded;
  if (!base::JSONWriter::Write(value, &encoded) || encoded.size() > 8192)
    return std::nullopt;
  return encoded;
}

std::optional<Node> Decode(std::string_view encoded,
                           bool allows_extended_vless) {
  if (encoded.size() > 8192) return std::nullopt;
  auto value = base::JSONReader::ReadDict(encoded, base::JSON_PARSE_RFC);
  if (!value) return std::nullopt;
  const auto* address = value->FindString("address");
  const auto port = value->FindInt("port").value_or(0);
  const auto* user = value->FindString("userId");
  const auto* server = value->FindString("serverName");
  const auto* fingerprint = value->FindString("fingerprint");
  if (!address || port < 1 || port > 65535 || !user || !server || !fingerprint)
    return std::nullopt;
  const auto version = value->FindInt("schemaVersion");
  Node node{*address,     static_cast<uint16_t>(port),
            *user,        *server,
            {},           {},
            *fingerprint, {}};
  if (version == 2 && value->size() == 8 && allows_extended_vless) {
    const auto* host = value->FindString("wsHost");
    const auto* path = value->FindString("wsPath");
    if (!host || !path) return std::nullopt;
    node.web_socket_tls =
        aegis_access::AccessTestNodeWebSocketTls{*host, *path};
    return ValidateAccessTestNodeNative(node);
  }
  if (version == 3 && allows_extended_vless) {
    const auto* outbound = value->FindString("outbound");
    if (!outbound)
      return std::nullopt;
    if (*outbound == "vless.ws.tls" && value->size() == 10) {
      const auto* host = value->FindString("wsHost");
      const auto* path = value->FindString("wsPath");
      const auto* alpn = value->FindString("wsAlpn");
      if (!host || !path || !alpn)
        return std::nullopt;
      node.web_socket_tls =
          aegis_access::AccessTestNodeWebSocketTls{*host, *path, *alpn};
    } else if (*outbound == "vless.raw.tls" && value->size() == 7) {
      node.tcp_tls_vision = true;
    } else if (*outbound == "vless.grpc.reality" && value->size() == 10) {
      const auto* key = value->FindString("publicKey");
      const auto* short_id = value->FindString("shortId");
      const auto* service_name = value->FindString("serviceName");
      if (!key || !short_id || !service_name)
        return std::nullopt;
      node.public_key = *key;
      node.short_id = *short_id;
      node.grpc_reality_service_name = *service_name;
    } else {
      return std::nullopt;
    }
    return ValidateAccessTestNodeNative(node);
  }
  if (version != 1 || value->size() != 10) return std::nullopt;
  const auto* key = value->FindString("publicKey");
  const auto* short_id = value->FindString("shortId");
  const auto* spider = value->FindString("spiderX");
  const auto mlkem = value->FindBool("shareSupportsX25519Mlkem768");
  if (!key || !short_id || !spider || !mlkem) return std::nullopt;
  node.public_key = *key;
  node.short_id = *short_id;
  node.spider_x = *spider;
  node.advertises_x25519_mlkem768 = *mlkem;
  return ValidateAccessTestNodeNative(node);
}
}  // namespace

std::optional<std::vector<std::string>> AccessTestNodeVaultMac::Backend::ListAccounts(
    std::string_view service) {
  return std::nullopt;
}

AccessTestNodeVaultMac::AccessTestNodeVaultMac(std::string channel,
                                               std::string scope_hash,
                                               std::unique_ptr<Backend> backend)
    : allows_extended_vless_(channel == "debug"),
      backend_(backend ? std::move(backend)
                       : std::make_unique<KeychainBackend>()) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
  service_ = ServiceForScope(channel, scope_hash);
}

AccessTestNodeVaultMac::RuntimeLease::RuntimeLease(std::string service, std::string reference)
    : service_(std::move(service)), reference_(std::move(reference)) {}
AccessTestNodeVaultMac::RuntimeLease::~RuntimeLease() {
  base::AutoLock lock(ReferenceLock());
  auto found = References().find({service_, reference_});
  CHECK(found != References().end() && found->second.holders > 0);
  if (--found->second.holders == 0 && !found->second.deleting) References().erase(found);
}
bool AccessTestNodeVaultMac::RuntimeLease::Matches(std::string_view channel,
    std::string_view scope_hash, const AccessTestNodeSecretReference& reference) const {
  return ServiceForScope(channel, scope_hash) == service_ && reference.id == reference_;
}
std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease>
AccessTestNodeVaultMac::ReserveRuntimeReference(std::string_view channel,
    std::string_view scope_hash, const AccessTestNodeSecretReference& reference) {
  auto service = ServiceForScope(channel, scope_hash);
  if (service.empty() || !ValidId(reference.id)) return nullptr;
  {
    base::AutoLock lock(ReferenceLock());
    auto& state = References()[{service, reference.id}];
    if (state.deleting || state.holders == std::numeric_limits<size_t>::max()) return nullptr;
    ++state.holders;
  }
  return std::shared_ptr<RuntimeLease>(new RuntimeLease(std::move(service), reference.id));
}
bool AccessTestNodeVaultMac::IsRuntimeReferenced(const AccessTestNodeSecretReference& reference) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (service_.empty() || !ValidId(reference.id)) return false;
  base::AutoLock lock(ReferenceLock());
  const auto found = References().find({service_, reference.id});
  return found != References().end() && (found->second.holders || found->second.deleting);
}
bool AccessTestNodeVaultMac::HasRuntimeReferences() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (service_.empty()) return false;
  base::AutoLock lock(ReferenceLock());
  return std::ranges::any_of(References(), [&](const auto& entry) {
    return entry.first.first == service_ && (entry.second.holders || entry.second.deleting);
  });
}

AccessTestNodeVaultMac::~AccessTestNodeVaultMac() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (const auto& [reference, pending] : pending_) {
    if (pending.operation.empty()) DeleteUnpinned(*backend_, service_, reference);
  }
}

AccessTestNodeVaultStageResult AccessTestNodeVaultMac::Stage(
    const std::string& editor, const Node& node) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (service_.empty()) return {Status::kNotAdmitted, std::nullopt};
  if (RequiresDevChannel(node) && !allows_extended_vless_)
    return {Status::kNotAdmitted, std::nullopt};
  if (!ValidId(editor)) return {Status::kInvalidReference, std::nullopt};
  if (pending_.size() >= 32) return {Status::kLimitReached, std::nullopt};
  const auto encoded = Encode(node);
  if (!encoded) return {Status::kInvalidNode, std::nullopt};
  auto id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  const auto status = backend_->Add(service_, id, *encoded);
  if (status != Status::kOk) return {status, std::nullopt};
  pending_.emplace(id, Pending{editor, {}, false});
  return {Status::kOk, AccessTestNodeSecretReference{std::move(id)}};
}

AccessTestNodeVaultStatus AccessTestNodeVaultMac::Discard(
    const std::string& editor, const AccessTestNodeSecretReference& reference) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const auto found = pending_.find(reference.id);
  if (service_.empty() || found == pending_.end() ||
      found->second.editor != editor || !found->second.operation.empty())
    return Status::kInvalidReference;
  const auto status = DeleteUnpinned(*backend_, service_, reference.id);
  if (status == Status::kOk) pending_.erase(found);
  return status;
}

AccessTestNodeVaultStatus AccessTestNodeVaultMac::CloseEditor(
    const std::string& editor) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto result = Status::kOk;
  for (auto it = pending_.begin(); it != pending_.end();) {
    auto& [reference, pending] = *it;
    if (pending.editor != editor) { ++it; continue; }
    pending.editor_closed = true;
    if (pending.operation.empty()) {
      const auto status = DeleteUnpinned(*backend_, service_, reference);
      if (status == Status::kOk) {
        it = pending_.erase(it);
        continue;
      }
      if (result == Status::kOk || status != Status::kInUse) result = status;
    }
    ++it;
  }
  return result;
}

std::optional<Node> AccessTestNodeVaultMac::Read(
    const AccessTestNodeSecretReference& reference) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (service_.empty() || !ValidId(reference.id)) return std::nullopt;
  auto encoded = backend_->Read(service_, reference.id);
  return encoded ? Decode(*encoded, allows_extended_vless_) : std::nullopt;
}

bool AccessTestNodeVaultMac::HoldForCommit(
    const std::string& editor, const AccessTestNodeSecretReference& reference,
    const std::string& operation) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!ValidId(operation)) return false;
  const auto found = pending_.find(reference.id);
  if (found == pending_.end() || found->second.editor != editor ||
      found->second.editor_closed ||
      (!found->second.operation.empty() && found->second.operation != operation))
    return false;
  for (const auto& [id, pending] : pending_) {
    if (id != reference.id && pending.operation == operation) return false;
  }
  found->second.operation = operation;
  return true;
}

void AccessTestNodeVaultMac::ResolveCommit(const std::string& operation,
                                           bool committed) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!ValidId(operation)) return;
  for (auto it = pending_.begin(); it != pending_.end();) {
    auto& [reference, pending] = *it;
    if (pending.operation != operation) { ++it; continue; }
    if (committed) {
      it = pending_.erase(it);
    } else {
      pending.operation.clear();
      if (pending.editor_closed &&
          DeleteUnpinned(*backend_, service_, reference) == Status::kOk)
        it = pending_.erase(it);
      else
        ++it;
    }
  }
}

AccessTestNodeVaultStatus AccessTestNodeVaultMac::ReconcileReferences(
    const std::set<std::string>& durable_references) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (service_.empty()) return Status::kNotAdmitted;
  if (durable_references.size() > 129 ||
      !std::ranges::all_of(durable_references, ValidId)) return Status::kInvalidReference;
  const auto accounts = backend_->ListAccounts(service_);
  if (!accounts || accounts->size() > 1024) return Status::kUnavailable;
  // Validate the whole inventory before the first mutation. Corruption or a
  // foreign account shape cannot become a partial broad cleanup operation.
  const std::set<std::string> unique(accounts->begin(), accounts->end());
  if (unique.size() != accounts->size() || !std::ranges::all_of(unique, ValidId))
    return Status::kInvalidReference;
  for (const auto& account : unique) {
    if (!durable_references.contains(account) && !pending_.contains(account)) {
      const auto status = DeleteUnpinned(*backend_, service_, account);
      if (status != Status::kOk && status != Status::kInUse) return Status::kUnavailable;
    }
  }
  return Status::kOk;
}
}  // namespace aegis::access
