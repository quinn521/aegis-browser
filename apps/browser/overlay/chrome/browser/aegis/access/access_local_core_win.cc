// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_local_core_win.h"

#include <windows.h>

#include <string>
#include <utility>

#include "base/base_paths_win.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/uuid.h"
#include "base/win/access_token.h"
#include "base/win/scoped_process_information.h"
#include "base/win/security_descriptor.h"
#include "base/win/startup_information.h"

namespace aegis::access {

bool CreatePrivateCoreDirectory(base::ScopedTempDir& directory) {
  base::FilePath root;
  auto token = base::win::AccessToken::FromCurrentProcess();
  if (!token || !base::PathService::Get(base::DIR_LOCAL_APP_DATA, &root)) {
    return false;
  }

  auto descriptor = base::win::SecurityDescriptor::CreateWithEmptyDacl();
  descriptor.set_dacl_protected(true);
  constexpr DWORD kInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
  if (!descriptor.SetDaclEntry(*token, base::win::SecurityAccessMode::kGrant,
                               FILE_ALL_ACCESS, kInheritance)) {
    return false;
  }
  auto absolute = descriptor.ToAbsolute();
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), &absolute, FALSE};
  const auto path = root.AppendASCII(
      "AegisAccess-" + base::Uuid::GenerateRandomV4().AsLowercaseString());
  if (!::CreateDirectoryW(path.value().c_str(), &attributes)) {
    return false;
  }
  if (!directory.Set(path)) {
    base::DeletePathRecursively(path);
    return false;
  }
  // A filesystem without persistent ACLs may ignore the creation descriptor.
  const auto applied = base::win::SecurityDescriptor::FromFile(
      path, DACL_SECURITY_INFORMATION);
  return applied && applied->dacl_protected() && applied->dacl() &&
         !applied->dacl()->is_null() && applied->dacl()->get()->AceCount == 1;
}

base::Process LaunchOwnedCore(const base::CommandLine& command,
                              const base::FilePath& directory,
                              base::win::ScopedHandle& job) {
  base::win::ScopedHandle owned_job(::CreateJobObjectW(nullptr, nullptr));
  if (!owned_job.is_valid() ||
      !base::SetJobObjectLimitFlags(owned_job.get(),
                                  JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)) {
    return {};
  }
  HANDLE job_handle = owned_job.get();
  base::win::StartupInformation startup;
  if (!startup.InitializeProcThreadAttributeList(1) ||
      !startup.UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                         &job_handle, sizeof(job_handle))) {
    return {};
  }

  PROCESS_INFORMATION child = {};
  std::wstring command_line = command.GetCommandLineString();
  wchar_t empty_environment[2] = {};
  if (!::CreateProcessW(command.GetProgram().value().c_str(), command_line.data(),
                        nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                            CREATE_NO_WINDOW,
                        empty_environment, directory.value().c_str(),
                        startup.startup_info(), &child)) {
    return {};
  }
  base::win::ScopedProcessInformation child_handles(child);
  job = std::move(owned_job);
  return base::Process(child_handles.TakeProcessHandle());
}

}  // namespace aegis::access
