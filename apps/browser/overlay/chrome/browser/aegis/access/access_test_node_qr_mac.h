// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_QR_MAC_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_QR_MAC_H_

#include <cstddef>
#include <optional>
#include <string_view>

#include "components/aegis_access/access_test_node.h"

namespace aegis::access {

inline constexpr size_t kMaxAccessTestNodeQrImageBytes = 16 * 1024 * 1024;

enum class AccessTestNodeQrStatus {
  kDecoded,
  kInvalidImage,
  kNoQrCode,
  kMultipleQrCodes,
  kInvalidShare,
  kUnsupportedOutbound,
};

struct AccessTestNodeQrResult {
  AccessTestNodeQrStatus status;
  std::optional<aegis_access::AccessTestNode> node;
};

// Shared by trusted macOS text import and QR decoding so both use Foundation
// for IDNA conversion of DEV dial hostnames.
aegis_access::AccessTestNodeParseResult ParseAccessTestNodeShareMac(
    std::string_view share,
    aegis_access::AccessTestNodeImportMode mode);

// Worker-only local decoding of bytes from an explicitly selected image.
// The result owns only a validated native draft and never the input image or
// the decoded URI. Callers must check admission before reading the file.
AccessTestNodeQrResult DecodeAccessTestNodeQrImage(
    std::string_view image, aegis_access::AccessTestNodeImportMode mode);

}  // namespace aegis::access

#endif  // CHROME_BROWSER_AEGIS_ACCESS_ACCESS_TEST_NODE_QR_MAC_H_
