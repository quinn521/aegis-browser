// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_test_node_qr_mac.h"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Vision/Vision.h>

#include <cstdint>
#include <utility>

namespace aegis::access {
namespace {

bool IsSupportedImage(NSData* image) {
  CGImageSourceRef source =
      CGImageSourceCreateWithData((__bridge CFDataRef)image, nullptr);
  if (!source) {
    return false;
  }
  NSDictionary* properties = nil;
  if (CGImageSourceGetCount(source) == 1) {
    properties = CFBridgingRelease(
        CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
  }
  CFRelease(source);

  // Bound the decoded raster before Vision sees it.
  constexpr uint64_t kMaxPixels = 24 * 1024 * 1024;
  const uint64_t width =
      [properties[(__bridge NSString*)kCGImagePropertyPixelWidth]
          unsignedLongLongValue];
  const uint64_t height =
      [properties[(__bridge NSString*)kCGImagePropertyPixelHeight]
          unsignedLongLongValue];
  return width > 0 && height > 0 && width <= kMaxPixels / height;
}

}  // namespace

AccessTestNodeQrResult DecodeAccessTestNodeQrImage(
    std::string_view image, aegis_access::AccessTestNodeImportMode mode) {
  using Status = AccessTestNodeQrStatus;
  if (image.empty() || image.size() > kMaxAccessTestNodeQrImageBytes) {
    return {Status::kInvalidImage, std::nullopt};
  }

  @autoreleasepool {
    NSData* bytes = [NSData dataWithBytes:image.data() length:image.size()];
    if (!IsSupportedImage(bytes)) {
      return {Status::kInvalidImage, std::nullopt};
    }

    VNDetectBarcodesRequest* request = [[VNDetectBarcodesRequest alloc] init];
    request.symbologies = @[ VNBarcodeSymbologyQR ];
    VNImageRequestHandler* handler =
        [[VNImageRequestHandler alloc] initWithData:bytes options:@{}];
    if (![handler performRequests:@[ request ] error:nullptr]) {
      return {Status::kInvalidImage, std::nullopt};
    }
    if (request.results.count == 0) {
      return {Status::kNoQrCode, std::nullopt};
    }
    if (request.results.count != 1) {
      return {Status::kMultipleQrCodes, std::nullopt};
    }

    NSString* payload = request.results.firstObject.payloadStringValue;
    const char* utf8 = payload.UTF8String;
    if (!utf8) {
      return {Status::kInvalidShare, std::nullopt};
    }
    auto result = aegis_access::ParseAccessTestNodeShare(
        std::string_view(
            utf8, [payload lengthOfBytesUsingEncoding:NSUTF8StringEncoding]),
        mode);
    switch (result.error) {
      case aegis_access::AccessTestNodeError::kNone:
        return {Status::kDecoded, std::move(result.node)};
      case aegis_access::AccessTestNodeError::kInvalidShare:
        return {Status::kInvalidShare, std::nullopt};
      case aegis_access::AccessTestNodeError::kUnsupportedOutbound:
        return {Status::kUnsupportedOutbound, std::nullopt};
    }
  }
}

}  // namespace aegis::access
