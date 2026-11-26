// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_VAULT_MAC_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_VAULT_MAC_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/sequence_checker.h"
#include "components/aegis_access/access_test_node.h"

namespace aegis::access {

enum class AccessTestNodeVaultStatus {
  kOk,
  kNotAdmitted,
  kUnavailable,
  kNotFound,
  kInvalidReference,
  kInvalidNode,
  kLimitReached,
  kInUse,
};

// An opaque reference, never a node field or an account/subscription credential.
struct AccessTestNodeSecretReference {
  std::string id;
  bool operator==(const AccessTestNodeSecretReference&) const = default;
};

struct AccessTestNodeVaultStageResult {
  AccessTestNodeVaultStatus status;
  std::optional<AccessTestNodeSecretReference> reference;
};

// Blocking, sequence-bound Profile vault. The native caller derives scope_hash
// from the installation identity, channel and real Profile. Renderer data may
// not choose the scope or editor owner. Only internal Debug/Alpha scopes exist.
class AccessTestNodeVaultMac {
 public:
  // Metadata-only native reservation. It grants neither read permission nor
  // upstream authorization. Keep it across preparation, active use and unknown
  // retirement; release only when this core has confirmed retirement. Copies
  // of the shared handle retain the same reservation for compatible users.
  class RuntimeLease {
   public:
    ~RuntimeLease();
    RuntimeLease(const RuntimeLease&) = delete;
    RuntimeLease& operator=(const RuntimeLease&) = delete;
    bool Matches(std::string_view channel, std::string_view scope_hash,
                 const AccessTestNodeSecretReference& reference) const;
   private:
    friend class AccessTestNodeVaultMac;
    RuntimeLease(std::string service, std::string reference);
    const std::string service_;
    const std::string reference_;
  };
  // Nonblocking metadata operation, safe before queuing the owning core worker.
  // A delete already in progress returns no lease; it never waits on Keychain
  // I/O. Only trusted native bindings may reserve a scope/reference.
  static std::shared_ptr<RuntimeLease> ReserveRuntimeReference(
      std::string_view channel, std::string_view scope_hash,
      const AccessTestNodeSecretReference& reference);
  class Backend {
   public:
    virtual ~Backend() = default;
    virtual AccessTestNodeVaultStatus Add(std::string_view service,
                                         std::string_view account,
                                         std::string_view secret) = 0;
    virtual std::optional<std::string> Read(std::string_view service,
                                           std::string_view account) = 0;
    virtual AccessTestNodeVaultStatus Delete(std::string_view service,
                                            std::string_view account) = 0;
    // Enumerates account names for this exact service without reading secrets.
    // Unavailable enumeration supplies no authority to delete any item.
    virtual std::optional<std::vector<std::string>> ListAccounts(
        std::string_view service);
  };

  AccessTestNodeVaultMac(std::string channel,
                         std::string scope_hash,
                         std::unique_ptr<Backend> backend = nullptr);
  ~AccessTestNodeVaultMac();
  AccessTestNodeVaultMac(const AccessTestNodeVaultMac&) = delete;
  AccessTestNodeVaultMac& operator=(const AccessTestNodeVaultMac&) = delete;

  AccessTestNodeVaultStageResult Stage(
      const std::string& editor,
      const aegis_access::AccessTestNode& node);
  // Discard is allowed only for this editor's own, still-uncommitted reference.
  AccessTestNodeVaultStatus Discard(
      const std::string& editor,
      const AccessTestNodeSecretReference& reference);
  AccessTestNodeVaultStatus CloseEditor(const std::string& editor);
  std::optional<aegis_access::AccessTestNode> Read(
      const AccessTestNodeSecretReference& reference);
  bool IsRuntimeReferenced(const AccessTestNodeSecretReference& reference) const;
  bool HasRuntimeReferences() const;

  // Reserve before the asynchronous database commit. A closed editor must not
  // delete a reference whose commit outcome is unknown. Recovery resolves such
  // references against the durable operation journal before later cleanup.
  bool HoldForCommit(const std::string& editor,
                     const AccessTestNodeSecretReference& reference,
                     const std::string& operation);
  void ResolveCommit(const std::string& operation, bool committed);
  // Called only with a complete validated durable reference set while the
  // native owner holds its commit/recovery fence. Live pending/unknown refs
  // are retained independently. Other service namespaces are never queried.
  AccessTestNodeVaultStatus ReconcileReferences(
      const std::set<std::string>& durable_references);

 private:
  struct Pending {
    std::string editor;
    std::string operation;
    bool editor_closed = false;
  };
  std::string service_;
  const bool allows_extended_vless_;
  std::unique_ptr<Backend> backend_;
  std::map<std::string, Pending> pending_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace aegis::access
#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_VAULT_MAC_H_
