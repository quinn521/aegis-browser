// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_core_log_store.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {
using Scope = CoreLogProfile;
using Status = AccessCoreLogStore::DiskStatus;
constexpr char kTime[] = "2026/11/11 01:02:03 ";
std::string Entry(std::string_view target) {
  return std::string(kTime) + "from 127.0.0.1:3000 accepted https://" +
         std::string(target) +
         "/path?token=synthetic-secret [private-tag] email: private-user";
}

class AccessCoreLogDiskTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(profile_.CreateUniqueTempDir()); }
  auto Open(Scope scope = Scope::kRegular, std::string_view level = "info") {
    return AccessCoreLogStore::Open(profile_.GetPath(), scope, level);
  }
  base::FilePath Directory() {
    return profile_.GetPath().AppendASCII(kCoreLogDirectory);
  }
  base::FilePath Regular() { return Directory().AppendASCII("regular.log"); }
  base::FilePath Private() { return Directory().AppendASCII("private.log"); }
  base::test::TaskEnvironment environment_{
      base::test::TaskEnvironment::ThreadPoolExecutionMode::QUEUED};
  base::ScopedTempDir profile_;
};

TEST_F(AccessCoreLogDiskTest,
       RestoreBoundedSanitizedScopesWithPrivatePermissions) {
  auto regular = Open();
  auto priv = Open(Scope::kPrivate);
  for (int i = 0; i < 160; ++i)
    regular->Add(0, Entry(std::to_string(i) + ".example"), {});
  priv->Add(0, Entry("private.example"), {});
  environment_.RunUntilIdle();
  EXPECT_EQ(regular->disk_status(), Status::kReady);
  EXPECT_EQ(priv->disk_status(), Status::kReady);
  std::string history;
  ASSERT_TRUE(base::ReadFileToString(Regular(), &history));
  EXPECT_EQ(history, regular->Snapshot());
  EXPECT_EQ(std::count(history.begin(), history.end(), '\n'), 127);
  EXPECT_EQ(history.find("private.example"), history.npos);
  EXPECT_EQ(history.find("synthetic-secret"), history.npos);
  EXPECT_EQ(history.find("private-tag"), history.npos);
  EXPECT_LT(history.size(), 65536u);
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(Directory(), &mode));
  EXPECT_EQ(mode, 0700);
  for (auto path : {Regular(), Private()}) {
    ASSERT_TRUE(base::GetPosixFilePermissions(path, &mode));
    EXPECT_EQ(mode, 0600);
  }
  const auto private_text = priv->Snapshot();
  regular.reset();
  priv.reset();
  auto restored = Open();
  auto restored_private = Open(Scope::kPrivate);
  restored_private->Add(0, Entry("new.example"), {});
  environment_.RunUntilIdle();
  EXPECT_EQ(restored->Snapshot(), history);
  EXPECT_TRUE(restored_private->Snapshot().starts_with(private_text + "\n"));
  EXPECT_NE(restored_private->Snapshot().find("new.example"), history.npos);
}
TEST_F(AccessCoreLogDiskTest, ClearAndNoneDefeatOlderRestoreAndQueuedWrites) {
  auto regular = Open();
  auto priv = Open(Scope::kPrivate);
  regular->Add(0, Entry("old.example"), {});
  priv->Add(0, Entry("private.example"), {});
  environment_.RunUntilIdle();
  regular.reset();
  regular = Open();
  regular->Add(0, Entry("queued.example"), {});
  regular->Clear();
  regular->Add(0, Entry("after.example"), {});
  environment_.RunUntilIdle();
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(Regular(), &contents));
  EXPECT_NE(contents.find("after.example"), contents.npos);
  EXPECT_EQ(contents.find("old.example"), contents.npos);
  EXPECT_EQ(contents.find("queued.example"), contents.npos);
  regular->Clear();
  environment_.RunUntilIdle();
  EXPECT_FALSE(base::PathExists(Regular()));
  EXPECT_TRUE(base::PathExists(Private()));
  priv->SetLevel("none");
  priv->Add(0, Entry("stale.example"), {});
  environment_.RunUntilIdle();
  EXPECT_TRUE(priv->Snapshot().empty());
  EXPECT_FALSE(base::PathExists(Private()));
  auto reopened = Open(Scope::kPrivate, "none");
  environment_.RunUntilIdle();
  EXPECT_TRUE(reopened->Snapshot().empty());
}
TEST_F(AccessCoreLogDiskTest, UnsafeFilesShowFailureWithoutFollowingLinks) {
  auto store = Open();
  store->Add(0, Entry("seed.example"), {});
  environment_.RunUntilIdle();
  ASSERT_TRUE(base::DeleteFile(Regular()));
  auto unrelated = profile_.GetPath().AppendASCII("other");
  ASSERT_TRUE(base::WriteFile(unrelated, "do not touch"));
  ASSERT_TRUE(base::CreateSymbolicLink(unrelated, Regular()));
  store->Add(0, Entry("still-in-memory.example"), {});
  environment_.RunUntilIdle();
  EXPECT_EQ(store->disk_status(), Status::kError);
  EXPECT_NE(store->Snapshot().find("still-in-memory.example"),
            std::string::npos);
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(unrelated, &contents));
  EXPECT_EQ(contents, "do not touch");
  store->Clear();
  environment_.RunUntilIdle();
  EXPECT_EQ(store->disk_status(), Status::kReady);
  EXPECT_FALSE(base::IsLink(Regular()));
  EXPECT_TRUE(base::PathExists(unrelated));
  ASSERT_TRUE(base::SetPosixFilePermissions(Directory(), 0755));
  store->Clear();
  environment_.RunUntilIdle();
  EXPECT_EQ(store->disk_status(), Status::kError);
  ASSERT_TRUE(base::SetPosixFilePermissions(Directory(), 0700));
  store->Clear();
  environment_.RunUntilIdle();
  EXPECT_EQ(store->disk_status(), Status::kReady);
}
}  // namespace
}  // namespace aegis::access
