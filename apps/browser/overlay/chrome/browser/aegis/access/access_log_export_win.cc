// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_log_export_win.h"

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/win/scoped_co_mem.h"
#include "ui/views/win/hwnd_util.h"

namespace aegis::access {

bool ExportAccessLogOnWindows(gfx::NativeWindow window,
                              const base::FilePath& archive,
                              const base::RepeatingCallback<bool()>& still_valid) {
  Microsoft::WRL::ComPtr<IFileSaveDialog> picker;
  if (FAILED(::CoCreateInstance(__uuidof(FileSaveDialog), nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) {
    return false;
  }
  if (FAILED(picker->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                                FOS_DONTADDTORECENT | FOS_NOCHANGEDIR | FOS_NODEREFERENCELINKS)) ||
      FAILED(picker->SetTitle(L"Save compressed Access diagnostics")) ||
      FAILED(picker->SetFileName(L"Aegis-Access.log.gz"))) return false;
  const auto result = picker->Show(views::HWNDForNativeWindow(window));
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return true;
  Microsoft::WRL::ComPtr<IShellItem> selected;
  base::win::ScopedCoMem<wchar_t> destination;
  return SUCCEEDED(result) && still_valid.Run() &&
      SUCCEEDED(picker->GetResult(&selected)) &&
      SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &destination)) &&
      ::CopyFileW(archive.value().c_str(), destination.get(), TRUE);
}

}  // namespace aegis::access
