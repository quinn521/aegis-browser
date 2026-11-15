// Copyright 2026 GCSA
#ifndef CHROME_BROWSER_AEGIS_ACCESS_ACCESS_LOG_EXPORT_WIN_H_
#define CHROME_BROWSER_AEGIS_ACCESS_ACCESS_LOG_EXPORT_WIN_H_

#include "base/functional/callback_forward.h"
#include "ui/gfx/native_ui_types.h"

namespace base {
class FilePath;
}
namespace aegis::access {
bool ExportAccessLogOnWindows(gfx::NativeWindow window,
                              const base::FilePath& archive,
                              const base::RepeatingCallback<bool()>& still_valid);
}
#endif
