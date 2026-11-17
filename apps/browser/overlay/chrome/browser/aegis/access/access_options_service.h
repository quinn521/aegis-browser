// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OPTIONS_SERVICE_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_OPTIONS_SERVICE_H_
#include "base/supports_user_data.h"
#include "base/threading/sequence_bound.h"
#include "chrome/browser/aegis/access/access_options_store.h"
class Profile;
namespace aegis::access {
// Each actual Profile owns a separate actor, including each private Profile.
class AccessOptionsService : public base::SupportsUserData::Data {
 public:
  static AccessOptionsService* GetOrCreate(Profile* profile);
  ~AccessOptionsService() override;
  bool Admitted(aegis_access::AccessOptionsOperation operation) const;
  void Read(base::OnceCallback<void(StoreResult<AccessOptionsSnapshot>)> callback);
 private:
  explicit AccessOptionsService(Profile* profile);
  base::SequenceBound<AccessOptionsStore> store_;
};
}  // namespace aegis::access
#endif
