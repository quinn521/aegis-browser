// Copyright 2026 GCSA

#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_URL_LOADER_THROTTLE_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_URL_LOADER_THROTTLE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/aegis/access/access_request_dispatch_state.h"
#include "components/aegis_access/request_policy_context.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "url/gurl.h"

class Profile;

namespace content {
class WebContents;
}

namespace network {
struct ResourceRequest;
}

namespace aegis::access {

class AccessURLLoaderThrottleTerminationHandle;

// Browser-side gate for the first real Access URLLoader slice. Policy and live
// generation sources are evaluated on the UI thread during throttle creation.
// WillStartRequest performs the final thread-safe dispatch-registry handoff
// immediately before network send.
class AccessURLLoaderThrottle final : public blink::URLLoaderThrottle {
 public:
  static std::unique_ptr<AccessURLLoaderThrottle> MaybeCreate(
      Profile* profile,
      const network::ResourceRequest& request,
      const base::RepeatingCallback<content::WebContents*()>& wc_getter,
      content::FrameTreeNodeId frame_tree_node_id,
      std::optional<int64_t> navigation_id);

  AccessURLLoaderThrottle(const AccessURLLoaderThrottle&) = delete;
  AccessURLLoaderThrottle& operator=(const AccessURLLoaderThrottle&) = delete;
  ~AccessURLLoaderThrottle() override;

  void DetachFromCurrentSequence() override;
  void WillStartRequest(network::ResourceRequest* request, bool* defer) override;
  void WillRedirectRequest(
      net::RedirectInfo* redirect_info,
      const network::mojom::URLResponseHead& response_head,
      bool* defer,
      network::HttpRequestHeadersUpdateParams* headers_update_params) override;
  void WillProcessResponse(
      const GURL& response_url,
      network::mojom::URLResponseHead* response_head,
      bool* defer) override;
  void WillOnCompleteWithError(
      const network::URLLoaderCompletionStatus& status) override;

 private:
  friend class AccessURLLoaderThrottleTerminationHandle;

  enum class StartDisposition {
    kDispatchProxy,
    kDeny,
  };

  AccessURLLoaderThrottle(
      GURL expected_url,
      StartDisposition disposition,
      scoped_refptr<AccessRequestDispatchState> dispatch_state,
      aegis_access::RequestOwnershipRecord request_record);

  void CancelWithoutRegistry(std::string_view reason);
  void CancelRegistered(std::string_view reason);
  void CompleteRegistered();
  void TerminateFromRegistry();

  GURL expected_url_;
  StartDisposition disposition_;
  scoped_refptr<AccessRequestDispatchState> dispatch_state_;
  aegis_access::RequestOwnershipRecord request_record_;
  bool registered_ = false;
  bool termination_requested_ = false;
  base::WeakPtrFactory<AccessURLLoaderThrottle> weak_factory_{this};
};

}  // namespace aegis::access

#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_URL_LOADER_THROTTLE_H_
