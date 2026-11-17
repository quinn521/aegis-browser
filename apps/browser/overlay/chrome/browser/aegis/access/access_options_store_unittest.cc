// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_options_store.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "sql/statement.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
TEST(AccessOptionsStoreTest, BetaCannotOpenOldInternalConfiguration) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto beta = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kBeta,
      aegis_access::BrowserReleaseChannel::kUnknown,
      ChannelNamespace::kAlpha, true);
  AccessOptionsStore store(beta, dir.GetPath(), "profile", false);
  EXPECT_EQ(store.Open(), StoreStatus::kConflict);
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("AccessOptions")));
}
TEST(AccessOptionsStoreTest, InternalChannelsAndPrivateProfilesAreIsolated) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  for (auto product : {aegis_access::AccessProductChannel::kDebug,
                       aegis_access::AccessProductChannel::kAlpha}) {
    const auto identity = aegis_access::ResolveAccessBuildIdentity(product,
        aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
    AccessOptionsStore ordinary(identity, dir.GetPath(), "profile", false);
    ASSERT_EQ(ordinary.Open(), StoreStatus::kValid);
    ASSERT_EQ(ordinary.Write(0, "{\"theme\":\"dark\"}"), StoreStatus::kValid);
    EXPECT_EQ(ordinary.Read().value->revision, 1u);
    AccessOptionsStore private_store(identity, dir.GetPath(), "private", true);
    ASSERT_EQ(private_store.Open(), StoreStatus::kValid);
    EXPECT_EQ(private_store.Read().value->revision, 0u);
    EXPECT_TRUE(private_store.database_path().empty());
    EXPECT_EQ(ordinary.Write(0, "{}"), StoreStatus::kConflict);
  }
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("access.db")));
}
TEST(AccessOptionsStoreTest, InterruptedMigrationPreservesOriginalAndRecovers) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  const auto folder = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha");
  ASSERT_TRUE(base::CreateDirectory(folder));
  {
    sql::Database legacy("AegisAccess");
    ASSERT_TRUE(legacy.Open(folder.AppendASCII("options.db")));
    ASSERT_TRUE(legacy.Execute("CREATE TABLE identity(singleton INTEGER PRIMARY KEY,version INTEGER,channel INTEGER,profile_id TEXT)"));
    ASSERT_TRUE(legacy.Execute("INSERT INTO identity VALUES(1,1,1,'profile')"));
    ASSERT_TRUE(legacy.Execute("CREATE TABLE configuration(singleton INTEGER PRIMARY KEY,revision INTEGER,payload TEXT)"));
    ASSERT_TRUE(legacy.Execute("INSERT INTO configuration VALUES(1,7,'{}')"));
  }
  {
    AccessOptionsStore interrupted(identity, dir.GetPath(), "profile", false);
    interrupted.SetInterruptMigrationForTesting(true);
    EXPECT_EQ(interrupted.Open(), StoreStatus::kRecoveryRequired);
    EXPECT_FALSE(interrupted.Read().value);
  }
  EXPECT_TRUE(base::PathExists(folder.AppendASCII("options.v1.db")));
  AccessOptionsStore resumed(identity, dir.GetPath(), "profile", false);
  ASSERT_EQ(resumed.Open(), StoreStatus::kValid);
  ASSERT_EQ(resumed.Read().value->revision, 7u);
  ASSERT_EQ(resumed.Write(7, "{\"theme\":\"dark\"}"), StoreStatus::kValid);
  sql::Database source("AegisAccess");
  ASSERT_TRUE(source.Open(folder.AppendASCII("options.v1.db")));
  sql::Statement query(source.GetUniqueStatement("SELECT revision FROM configuration"));
  ASSERT_TRUE(query.Step());
  EXPECT_EQ(query.ColumnInt64(0), 7);
}
TEST(AccessOptionsStoreTest, ProductChannelConflictPreservesExistingDatabase) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  {
    auto binding = TrustedAccessStoreBindingFactory::CreatePersistent(
        ChannelNamespace::kBeta, "profile", "runtime", dir.GetPath());
    ASSERT_TRUE(binding);
    AccessRuleStore product(*binding);
    ASSERT_EQ(product.Open(), StoreStatus::kValid);
  }
  std::string original;
  ASSERT_TRUE(base::ReadFileToString(dir.GetPath().AppendASCII("access.db"), &original));
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  AccessOptionsStore options(identity, dir.GetPath(), "profile", false);
  EXPECT_EQ(options.Open(), StoreStatus::kConflict);
  EXPECT_FALSE(base::PathExists(dir.GetPath().AppendASCII("AccessOptions")));
  std::string current;
  ASSERT_TRUE(base::ReadFileToString(dir.GetPath().AppendASCII("access.db"), &current));
  EXPECT_EQ(current, original);
}
TEST(AccessOptionsStoreTest, WrongOwnerAndUnsupportedSchemaPreserveConfiguration) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const auto identity = aegis_access::ResolveAccessBuildIdentity(
      aegis_access::AccessProductChannel::kAlpha,
      aegis_access::BrowserReleaseChannel::kUnknown, ChannelNamespace::kInvalid, true);
  {
    AccessOptionsStore first(identity, dir.GetPath(), "profile", false);
    ASSERT_EQ(first.Open(), StoreStatus::kValid);
    EXPECT_EQ(first.Write(0, "[]"), StoreStatus::kInvalidArgument);
    EXPECT_EQ(first.Write(0, std::string(65537, 'a')), StoreStatus::kInvalidArgument);
    ASSERT_EQ(first.Write(0, "{\"theme\":\"dark\"}"), StoreStatus::kValid);
  }
  const auto path = dir.GetPath().AppendASCII("AccessOptions").AppendASCII("alpha").AppendASCII("options.db");
  {
    AccessOptionsStore other(identity, dir.GetPath(), "other-profile", false);
    EXPECT_EQ(other.Open(), StoreStatus::kConflict);
  }
  {
    sql::Database future("AegisAccess");
    ASSERT_TRUE(future.Open(path));
    ASSERT_TRUE(future.Execute("UPDATE identity SET version=999"));
  }
  std::string before;
  ASSERT_TRUE(base::ReadFileToString(path, &before));
  AccessOptionsStore future(identity, dir.GetPath(), "profile", false);
  EXPECT_EQ(future.Open(), StoreStatus::kIncompatible);
  std::string after;
  ASSERT_TRUE(base::ReadFileToString(path, &after));
  EXPECT_EQ(before, after);
}
}  // namespace aegis::access
