// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_core_log_disk.h"

#include <windows.h>

#include <string>
#include <string_view>

#include "base/containers/span.h"

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "chrome/browser/aegis/access/access_core_log_store.h"
#include "chrome/browser/aegis/access/access_private_files_win.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aegis::access {
namespace {

TEST(AccessCoreLogWindowsTest, KeepsPrivateAndRegularHistorySeparate) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const auto profile = temp.GetPath().AppendASCII("profile");
  ASSERT_TRUE(CreatePrivateAccessDirectory(profile));
  ASSERT_TRUE(SaveCoreLogHistory(profile, CoreLogProfile::kRegular, "regular"));
  ASSERT_TRUE(SaveCoreLogHistory(profile, CoreLogProfile::kPrivate, "private"));
  ASSERT_TRUE(SaveCoreLogHistory(profile, CoreLogProfile::kPrivate, ""));
  std::string history;
  ASSERT_TRUE(ReadCoreLogHistory(profile, CoreLogProfile::kPrivate, &history));
  EXPECT_TRUE(history.empty());
  ASSERT_TRUE(ReadCoreLogHistory(profile, CoreLogProfile::kRegular, &history));
  EXPECT_EQ(history, "regular");
}

TEST(AccessCoreLogWindowsTest, ClearFencesAlreadyQueuedPipeBytes) {
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  ASSERT_TRUE(::CreatePipe(&read, &write, nullptr, 0));
  base::File reader(read);
  base::File writer(write);
  auto store = base::MakeRefCounted<AccessCoreLogStore>();
  const uint64_t original = store->RegisterPipe(reader.GetPlatformFile());
  ASSERT_TRUE(writer.WriteAtCurrentPosAndCheck(base::as_byte_span(std::string_view("old\n"))));
  store->Clear();
  ASSERT_TRUE(writer.WriteAtCurrentPosAndCheck(base::as_byte_span(std::string_view("new\n"))));
  char buffer[64];
  const auto event = store->ReadPipe(reader, buffer);
  EXPECT_NE(event.clear_epoch, original);
  EXPECT_EQ(event.clear_prefix, 4u);
  EXPECT_EQ(event.count, 8u);
  store->UnregisterPipe(reader.GetPlatformFile());
}

}  // namespace
}  // namespace aegis::access
