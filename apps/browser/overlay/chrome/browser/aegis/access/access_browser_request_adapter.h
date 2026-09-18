// Copyright 2026 GCSA

#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_BROWSER_REQUEST_ADAPTER_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_BROWSER_REQUEST_ADAPTER_H_

#include <cstdint>
#include <optional>

#include "base/functional/callback.h"
#include "components/aegis_access/request_policy_context.h"
#include "content/public/browser/frame_tree_node_id.h"

class Profile;

namespace content {
class StoragePartition;
class WebContents;
}

namespace aegis::access {

enum class AccessBrowserRequestMetadataStatus {
  kOk,
  kUnsupportedProfile,
  kMissingTransport,
  kMissingTrustedContents,
  kBrowserContextMismatch,
  kMissingTrustedFrame,
  kMissingStoragePartition,
  kUnconfiguredPartition,
  kInvalidPartitionPath,
  kInvalidOwner,
  kInvalidAttribution,
  kInvalidMetadata,
};

struct AccessBrowserRequestMetadataResult {
  AccessBrowserRequestMetadataStatus status =
      AccessBrowserRequestMetadataStatus::kInvalidMetadata;
  std::optional<aegis_access::BrowserOwnedRequestMetadata> metadata;
};

// Builds Access metadata exclusively from browser-owned Profile/WebContents /
// RenderFrameHost / StoragePartition state. It never consumes renderer-supplied
// request_initiator/source-site strings and never creates transport state.
//
// A navigation id takes precedence over the currently committed document so a
// top-level navigation cannot inherit the old page's authorization. Pending
// subframe navigation in the primary Page additionally carries the browser-owned
// primary main-frame SchemefulSite, keeping iframe destination hosts scoped to
// the real top-level site. Prerender/BFCache/fenced/pending Pages are rejected.
// Requests without a trusted WebContents/frame are rejected rather than
// borrowing an active tab identity.
AccessBrowserRequestMetadataResult BuildBrowserOwnedRequestMetadata(
    Profile* profile,
    const base::RepeatingCallback<content::WebContents*()>& wc_getter,
    content::FrameTreeNodeId frame_tree_node_id,
    std::optional<int64_t> navigation_id);

// Builds browser-owned Profile-only metadata for a background Worker request
// only when the caller can supply the exact StoragePartition that owns the
// NetworkContext. The partition must already be configured by this Profile's
// Access transport. No WebContents, frame, top-level site, or renderer-provided
// initiator is consulted, so the resulting context cannot match site-scoped
// rules.
AccessBrowserRequestMetadataResult BuildBrowserOwnedProfileRequestMetadata(
    Profile* profile,
    content::StoragePartition* partition);

}  // namespace aegis::access

#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_BROWSER_REQUEST_ADAPTER_H_
