// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_log_export_win.h"

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <optional>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/win/scoped_co_mem.h"
#include "ui/views/win/hwnd_util.h"

namespace aegis::access {
namespace {

std::optional<base::FilePath> PickDestination(HWND owner) {
  if (!::IsWindow(owner)) return std::nullopt;
  Microsoft::WRL::ComPtr<IFileSaveDialog> picker;
  if (FAILED(::CoCreateInstance(__uuidof(FileSaveDialog), nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) {
    return std::nullopt;
  }
  if (FAILED(picker->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                                FOS_DONTADDTORECENT | FOS_NOCHANGEDIR |
                                FOS_NODEREFERENCELINKS)) ||
      FAILED(picker->SetTitle(L"Save compressed Access diagnostics")) ||
      FAILED(picker->SetFileName(L"Aegis-Access.log.gz"))) return std::nullopt;
  const auto result = picker->Show(owner);
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return base::FilePath();
  Microsoft::WRL::ComPtr<IShellItem> selected;
  base::win::ScopedCoMem<wchar_t> destination;
  if (FAILED(result) || FAILED(picker->GetResult(&selected)) ||
      FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &destination))) {
    return std::nullopt;
  }
  return base::FilePath(destination.get());
}

}  // namespace

void ExportAccessLogOnWindows(gfx::NativeWindow window,
                              base::FilePath archive,
                              base::RepeatingCallback<bool()> still_valid,
                              base::OnceCallback<void(bool)> done) {
  // Keep the OS modal loop off the Browser UI sequence, where account grants
  // and proxy readiness still need their timers and replies.
  auto sequence = base::ThreadPool::CreateCOMSTATaskRunner(
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::SingleThreadTaskRunnerThreadMode::DEDICATED);
  sequence->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&PickDestination, views::HWNDForNativeWindow(window)),
      base::BindOnce([](base::FilePath archive,
                        base::RepeatingCallback<bool()> still_valid,
                        base::OnceCallback<void(bool)> done,
                        std::optional<base::FilePath> destination) {
        if (!destination || !still_valid.Run()) {
          std::move(done).Run(false);
          return;
        }
        if (destination->empty()) {
          std::move(done).Run(true);
          return;
        }
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE, {base::MayBlock()},
            base::BindOnce([](const base::FilePath& from,
                              const base::FilePath& to) {
              return ::CopyFileW(from.value().c_str(), to.value().c_str(), TRUE) != FALSE;
            }, std::move(archive), std::move(*destination)), std::move(done));
      }, std::move(archive), std::move(still_valid), std::move(done)));
}

}  // namespace aegis::access
