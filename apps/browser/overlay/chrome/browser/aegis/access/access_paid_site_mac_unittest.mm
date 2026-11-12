// Copyright 2026 GCSA
#import <Foundation/Foundation.h>

#include "chrome/browser/aegis/access/access_paid_site_mac.h"
#include "base/base64.h"
#include "base/json/json_writer.h"
#include "base/files/file_util.h"
#include "net/log/test_net_log.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/test/run_until.h"
#include "chrome/browser/aegis/access/access_network_context_transport.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/browser_task_environment.h"
#include "crypto/hash.h"
#include "net/cert/test_root_certs.h"
#include "net/test/cert_test_util.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_test_util.h"
#include "services/network/network_service_proxy_delegate.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {
// Optional wire fixture: the target accepts only traffic owned by its Xray
// peer and returns a known payload. No process-global trust changes.
TEST(PaidSiteMac, ChromiumConnectThroughAssignedNode) {
  NSDictionary* env = NSProcessInfo.processInfo.environment;
  if (!env[@"AEGIS_S3_HTTPS_URL"]) GTEST_SKIP() << "requires controlled node fixture";
  content::BrowserTaskEnvironment task(base::test::TaskEnvironment::MainThreadType::IO);
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  TestingProfile::Builder profile_builder;
  profile_builder.SetPath(temp.GetPath());
  auto profile = profile_builder.Build();
  auto path = temp.GetPath().AppendASCII("AegisAccessAccount.json");
  NSData* binding = [NSJSONSerialization dataWithJSONObject:@{
      @"origin":env[@"AEGIS_S3_ORIGIN"], @"token":env[@"AEGIS_S3_TOKEN"],
      @"accountId":env[@"AEGIS_S3_ID"], @"generation":NSUUID.UUID.UUIDString }
      options:0 error:nil];
  ASSERT_TRUE(base::WriteFile(path, base::SysNSStringToUTF8([[NSString alloc] initWithData:binding encoding:NSUTF8StringEncoding])));
  ASSERT_TRUE(base::SetPosixFilePermissions(path, 0600));
  net::ScopedTestRoot root(net::ImportCertFromFile(
      base::FilePath(base::SysNSStringToUTF8(env[@"AEGIS_S3_CA_PEM"]))).get());
  network::mojom::NetworkContextParams params;
  ASSERT_TRUE(AccessNetworkContextTransport::ConfigureNetworkContext(profile.get(), {}, &params));
  auto builder = net::CreateTestURLRequestContextBuilder();
  builder->set_proxy_delegate(std::make_unique<network::NetworkServiceProxyDelegate>(
      std::move(params.initial_custom_proxy_config), std::move(params.custom_proxy_config_client_receiver),
      mojo::PendingRemote<network::mojom::CustomProxyConnectionObserver>()));
  auto network = builder->Build();
  GURL url(base::SysNSStringToUTF8(env[@"AEGIS_S3_HTTPS_URL"]));
  const std::string site(url.host());
  ASSERT_TRUE(UsesAccountNode(profile.get()));
  // Same entry point invoked by the sole trusted toolbar toggle.
  ToggleAccountNodeSite(profile.get(), site);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return AccountNodeForSite(profile.get(), site) == AccountNodeStatus::kAssigned;
  }));
  task.RunUntilIdle();
  net::RecordingNetLogObserver diagnostics(net::NetLogCaptureMode::kEverything);
  const auto authorization = AccessNetworkContextTransport::Get(profile.get())
      ->BuildConfigForTesting({})->connect_tunnel_headers.GetHeader("Proxy-Authorization");
  ASSERT_TRUE(authorization.has_value());
  net::TestDelegate result;
  auto request = network->CreateRequest(url, net::DEFAULT_PRIORITY, &result,
                                        TRAFFIC_ANNOTATION_FOR_TESTS, net::handles::kInvalidNetworkHandle);
  request->Start();
  result.RunUntilComplete();
  ASSERT_EQ(net::OK, result.request_status());
  ASSERT_EQ(200, request->GetResponseCode());
  EXPECT_EQ(base::HexEncodeLower(crypto::hash::Sha256(result.data_received())),
            base::SysNSStringToUTF8(env[@"AEGIS_S3_EXPECTED_SHA256"]));
  std::map<uint32_t, std::string> socket_bytes;
  for (const auto& entry : diagnostics.GetEntries()) {
    const std::string json = base::WriteJson(entry.params).value_or("");
    EXPECT_FALSE(json.contains(*authorization));
    if (const auto* bytes = entry.params.FindString("bytes")) {
      std::string decoded;
      if (base::Base64Decode(*bytes, &decoded)) socket_bytes[entry.source.id] += decoded;
    }
  }
  for (const auto& [id, bytes] : socket_bytes) EXPECT_FALSE(bytes.contains(*authorization));
  ToggleAccountNodeSite(profile.get(), "second.example");
  EXPECT_EQ(AccountNodeStatus::kAssigned, AccountNodeForSite(profile.get(), "second.example"));
  ToggleAccountNodeSite(profile.get(), "second.example");
  EXPECT_EQ(AccountNodeStatus::kAssigned, AccountNodeForSite(profile.get(), site));
  ASSERT_TRUE(base::DeleteFile(path));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return AccountNodeForSite(profile.get(), site) == AccountNodeStatus::kUnavailable;
  }));
  task.RunUntilIdle();
  net::TestDelegate denied;
  auto retry = network->CreateRequest(url, net::DEFAULT_PRIORITY, &denied, TRAFFIC_ANNOTATION_FOR_TESTS, net::handles::kInvalidNetworkHandle);
  retry->Start();
  denied.RunUntilComplete();
  EXPECT_NE(net::OK, denied.request_status());
}
}  // namespace
}  // namespace aegis::access
