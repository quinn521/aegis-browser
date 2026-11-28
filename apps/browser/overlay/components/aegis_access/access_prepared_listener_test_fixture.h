// Copyright 2026 GCSA
#ifndef COMPONENTS_AEGIS_ACCESS_ACCESS_PREPARED_LISTENER_TEST_FIXTURE_H_
#define COMPONENTS_AEGIS_ACCESS_ACCESS_PREPARED_LISTENER_TEST_FIXTURE_H_
#include "components/aegis_access/access_prepared_listener.h"
namespace aegis_access::test {
inline AccessPreparedListenerBinding SyntheticPreparedListenerBinding() {
  return {kAccessNoAuthListenerWireVersion, "11111111-2222-4333-8444-555555555555", 1, 1, 1};
}
}  // namespace aegis_access::test
#endif  // COMPONENTS_AEGIS_ACCESS_ACCESS_PREPARED_LISTENER_TEST_FIXTURE_H_
