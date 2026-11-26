// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_test_node_qr_mac.h"

#import <CoreImage/CIFilterBuiltins.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>

#include <cassert>
#include <iostream>
#include <string>
#include <utility>

#include "components/aegis_access/access_test_node_test_fixture.h"

namespace {

using aegis::access::AccessTestNodeQrStatus;
using aegis::access::DecodeAccessTestNodeQrImage;
using aegis::access::ParseAccessTestNodeShareMac;
using aegis_access::AccessTestNodeImportMode;

CIImage* MakeCode(std::string_view payload) {
  CIFilter<CIQRCodeGenerator>* generator = [CIFilter QRCodeGenerator];
  generator.message = [NSData dataWithBytes:payload.data()
                                     length:payload.size()];
  generator.correctionLevel = @"M";
  return [generator.outputImage
      imageByApplyingTransform:CGAffineTransformMakeScale(6, 6)];
}

NSData* DrawCodes(CIImage* first, CIImage* second = nil) {
  const auto canvas_rect = CGRectMake(0, 0, 840, 420);
  CIImage* canvas = [[CIImage imageWithColor:CIColor.whiteColor]
      imageByCroppingToRect:canvas_rect];
  if (first) {
    first = [first
        imageByApplyingTransform:CGAffineTransformMakeTranslation(56, 56)];
    canvas = [first imageByCompositingOverImage:canvas];
  }
  if (second) {
    second = [second
        imageByApplyingTransform:CGAffineTransformMakeTranslation(476, 56)];
    canvas = [second imageByCompositingOverImage:canvas];
  }
  CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
  NSData* data = [[CIContext contextWithOptions:@{
    kCIContextUseSoftwareRenderer : @YES
  }] PNGRepresentationOfImage:canvas
                       format:kCIFormatRGBA8
                   colorSpace:rgb
                      options:@{}];
  CGColorSpaceRelease(rgb);
  assert(data.length > 0);
  return data;
}

aegis::access::AccessTestNodeQrResult Decode(
    NSData* image,
    AccessTestNodeImportMode mode = AccessTestNodeImportMode::kRealityOnly) {
  return DecodeAccessTestNodeQrImage(
      std::string_view(static_cast<const char*>(image.bytes), image.length),
      mode);
}

void ExpectStatus(
    NSData* image,
    AccessTestNodeQrStatus status,
    AccessTestNodeImportMode mode = AccessTestNodeImportMode::kRealityOnly) {
  auto result = Decode(image, mode);
  assert(result.status == status);
  assert(!result.node);
}

}  // namespace

int main() {
  @autoreleasepool {
    const auto share = aegis_access::test::kSyntheticTestNodeShare;
    const auto typed = aegis_access::ParseAccessTestNodeShare(share);
    const auto scanned = Decode(DrawCodes(MakeCode(share)));
    assert(typed.node);
    assert(scanned.status == AccessTestNodeQrStatus::kDecoded);
    assert(scanned.node == typed.node);
    using namespace aegis_access::test;
    constexpr auto extended = AccessTestNodeImportMode::kAllowExtendedVless;
    for (const auto vector : {kSyntheticPinnedRealityVisionShare,
                              kSyntheticWebSocketTlsShare,
                              kSyntheticWebSocketTlsQueryShare,
                              kSyntheticTcpTlsVisionShare,
                              kSyntheticGrpcRealityShare}) {
      const auto text = ParseAccessTestNodeShareMac(vector, extended);
      const auto qr = Decode(DrawCodes(MakeCode(vector)), extended);
      assert(text.node && qr.status == AccessTestNodeQrStatus::kDecoded);
      assert(qr.node == text.node);
      if (vector != kSyntheticPinnedRealityVisionShare) {
        ExpectStatus(DrawCodes(MakeCode(vector)),
                     AccessTestNodeQrStatus::kUnsupportedOutbound);
      }
    }
    for (const auto vector : {kSyntheticWebSocketTlsQueryShare,
                              kSyntheticTcpTlsVisionShare}) {
      std::string unverified(vector);
      unverified.insert(unverified.find('#'), "&allowInsecure=1");
      const auto text = ParseAccessTestNodeShareMac(unverified, extended);
      const auto qr = Decode(DrawCodes(MakeCode(unverified)), extended);
      assert(text.error == aegis_access::AccessTestNodeError::kUnsupportedOutbound);
      assert(qr.status == AccessTestNodeQrStatus::kUnsupportedOutbound && !qr.node);
    }
    for (const auto vector :
         {kSyntheticWebSocketTlsUnknownFingerprintShare,
          kSyntheticWebSocketTlsEncodedAuthorityDelimiterShare,
          kSyntheticWebSocketTlsDoubleEncodedDialHostShare,
          kSyntheticWebSocketTlsInvalidUtf8DialHostShare,
          kSyntheticTcpRealityVisionConcatenatedShare,
          kSyntheticTcpRealityVisionInvalidFingerprintShare,
          kSyntheticTcpRealityVisionInvalidPublicKeyShare,
          kSyntheticTcpRealityVisionOddShortIdShare,
          kSyntheticTcpRealityVisionUnsafeSpiderXShare,
          kSyntheticTcpTlsVisionInvalidDialAddressShare,
          kSyntheticGrpcRealityInvalidDialAddressShare}) {
      const auto text = ParseAccessTestNodeShareMac(vector, extended);
      assert(text.error == aegis_access::AccessTestNodeError::kInvalidShare);
      ExpectStatus(DrawCodes(MakeCode(vector)),
                   AccessTestNodeQrStatus::kInvalidShare, extended);
    }
    for (const auto& [vector, expected_host] :
         {std::pair{kSyntheticWebSocketTlsEscapedDialHostShare, "route.example."},
          std::pair{kSyntheticWebSocketTlsIdnaDialHostShare,
                    "xn--bcher-kva.example"}}) {
      const auto text = ParseAccessTestNodeShareMac(vector, extended);
      const auto qr = Decode(DrawCodes(MakeCode(vector)), extended);
      assert(text.node && qr.status == AccessTestNodeQrStatus::kDecoded);
      assert(text.node->address == expected_host && qr.node == text.node);
    }
    for (const auto vector :
         {kSyntheticTcpRealityVisionSlashAndAliasesShare,
          kSyntheticWebSocketTlsSlashShare,
          kSyntheticTcpTlsVisionSlashShare,
          kSyntheticGrpcRealitySlashShare}) {
      const auto text = ParseAccessTestNodeShareMac(vector, extended);
      const auto qr = Decode(DrawCodes(MakeCode(vector)), extended);
      assert(text.node && qr.status == AccessTestNodeQrStatus::kDecoded);
      assert(text.node == qr.node);
    }
    for (const auto vector :
         {kSyntheticTcpRealityVisionMissingFlowShare,
          kSyntheticWebSocketTlsFingerprintAliasShare,
          kSyntheticTcpTlsVisionFingerprintAliasShare,
          kSyntheticGrpcRealityFingerprintAliasShare}) {
      const auto text = ParseAccessTestNodeShareMac(vector, extended);
      assert(text.error == aegis_access::AccessTestNodeError::kUnsupportedOutbound);
      ExpectStatus(DrawCodes(MakeCode(vector)),
                   AccessTestNodeQrStatus::kUnsupportedOutbound, extended);
    }
    std::string exporter_share(share);
    exporter_share.insert(exporter_share.find('#'), "&pcks=");
    const auto scanned_exporter = Decode(DrawCodes(MakeCode(exporter_share)));
    assert(scanned_exporter.status == AccessTestNodeQrStatus::kDecoded);
    assert(scanned_exporter.node == typed.node);
    exporter_share.insert(exporter_share.find('#'), "required-value");
    ExpectStatus(DrawCodes(MakeCode(exporter_share)),
                 AccessTestNodeQrStatus::kUnsupportedOutbound);
    std::string mlkem_share(share);
    const auto sid = mlkem_share.find("sid=abcd1234");
    assert(sid != std::string::npos);
    mlkem_share.replace(sid, std::string_view("sid=abcd1234").size(),
                        "sid=&spx=%2F&support-x25519mlkem768=true");
    const auto typed_mlkem = aegis_access::ParseAccessTestNodeShare(mlkem_share);
    assert(typed_mlkem.node && typed_mlkem.node->advertises_x25519_mlkem768 &&
           typed_mlkem.node->short_id.empty());
    const auto scanned_mlkem = Decode(DrawCodes(MakeCode(mlkem_share)));
    assert(scanned_mlkem.status == AccessTestNodeQrStatus::kDecoded);
    assert(scanned_mlkem.node == typed_mlkem.node);
    mlkem_share.replace(mlkem_share.find("mlkem768=true"),
                        std::string_view("mlkem768=true").size(),
                        "mlkem768=false");
    ExpectStatus(DrawCodes(MakeCode(mlkem_share)),
                 AccessTestNodeQrStatus::kUnsupportedOutbound);
    ExpectStatus(DrawCodes(nil), AccessTestNodeQrStatus::kNoQrCode);
    ExpectStatus(DrawCodes(MakeCode("https://example.test/subscription")),
                 AccessTestNodeQrStatus::kUnsupportedOutbound);
    ExpectStatus(DrawCodes(MakeCode("vless://invalid")),
                 AccessTestNodeQrStatus::kInvalidShare);
    ExpectStatus(DrawCodes(MakeCode(share), MakeCode(share)),
                 AccessTestNodeQrStatus::kMultipleQrCodes);
    ExpectStatus([@"not an image" dataUsingEncoding:NSUTF8StringEncoding],
                 AccessTestNodeQrStatus::kInvalidImage);
    const std::string oversized(
        aegis::access::kMaxAccessTestNodeQrImageBytes + 1, 'x');
    assert(DecodeAccessTestNodeQrImage(
               oversized, AccessTestNodeImportMode::kRealityOnly).status ==
           AccessTestNodeQrStatus::kInvalidImage);
    std::cout
        << "PASS: Access Mac QR image text equivalence and rejection cases\n";
  }
}
