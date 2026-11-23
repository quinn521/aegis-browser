// Copyright 2026 GCSA
#include "components/aegis_access/access_prepared_listener.h"

#include <limits>

#include "base/uuid.h"

namespace aegis_access {
bool IsValidAccessPreparedListenerBinding(const AccessPreparedListenerBinding& binding) {
  return binding.wire_version == kAccessNoAuthListenerWireVersion &&
      base::Uuid::ParseLowercase(binding.connection_profile_id).is_valid() &&
      binding.profile_version > 0 &&
      binding.profile_version <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
      binding.upstream_authorization_generation > 0 && binding.listener_generation > 0;
}
}  // namespace aegis_access
