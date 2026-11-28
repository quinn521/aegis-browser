// Copyright 2026 GCSA
#ifndef COMPONENTS_AEGIS_ACCESS_ACCESS_PREPARED_LISTENER_H_
#define COMPONENTS_AEGIS_ACCESS_ACCESS_PREPARED_LISTENER_H_

#include <cstdint>
#include <string>

namespace aegis_access {
// Version of this private IPC value, independent of the SDD freeze revision.
// Zero denotes absence/legacy input and must never imply no-auth admission.
inline constexpr uint32_t kAccessNoAuthListenerWireVersion = 1;

// Non-secret, immutable identity of an already prepared no-auth local listener.
// The containing registration binds owner, partition, group, actual protocol,
// numeric address/port and unique registration ID. This value proves neither
// authorization nor listener health; only the trusted native owner may supply
// it after those checks and before exact NetworkContext publication/ACK.
struct AccessPreparedListenerBinding {
  uint32_t wire_version = 0;
  std::string connection_profile_id;
  uint64_t profile_version = 0;
  uint64_t upstream_authorization_generation = 0;
  uint64_t listener_generation = 0;
  bool operator==(const AccessPreparedListenerBinding&) const = default;
};

bool IsValidAccessPreparedListenerBinding(const AccessPreparedListenerBinding& binding);
}  // namespace aegis_access
#endif  // COMPONENTS_AEGIS_ACCESS_ACCESS_PREPARED_LISTENER_H_
