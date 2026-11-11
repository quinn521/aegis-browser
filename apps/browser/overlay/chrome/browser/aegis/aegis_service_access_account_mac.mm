// Copyright 2026 GCSA
#include "chrome/browser/aegis/aegis_service.h"

#import "chrome/browser/aegis/access/access_account_mac.h"

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "chrome/browser/profiles/profile.h"

namespace aegis {
void AegisService::QueryCurrentAccessAccount(
    base::OnceCallback<void(base::DictValue)> callback) {
  if (!profile_ || !initialized_ || !profile_->IsRegularProfile() ||
      profile_->IsOffTheRecord()) {
    std::move(callback).Run(base::DictValue().Set("state", "signed_out"));
    return;
  }
  // Bind delivery to this service and its original UI sequence. Destruction
  // and Shutdown invalidate the Profile capability before a reply can escape.
  __block auto reply = base::BindPostTaskToCurrentDefault(base::BindOnce(
      [](base::WeakPtr<AegisService> service,
         base::OnceCallback<void(base::DictValue)> callback, base::DictValue result) {
        if (service && service->profile() && service->IsInitializedForProfile(service->profile()))
          std::move(callback).Run(std::move(result));
      }, weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
  AegisQueryAccessAccount(base::SysUTF8ToNSString(profile_->GetPath().value()),
      ^(NSDictionary* result) {
        NSData* json = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
        auto parsed = json ? base::JSONReader::ReadDict(
            std::string_view(static_cast<const char*>(json.bytes), json.length),
            base::JSON_PARSE_RFC) : std::nullopt;
        std::move(reply).Run(parsed ? std::move(*parsed)
                                    : base::DictValue().Set("state", "unavailable"));
      });
}
}  // namespace aegis
