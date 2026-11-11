// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_core_log_share.h"

#include <sys/stat.h>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "third_party/zlib/google/compression_utils.h"

namespace aegis::access {
bool WriteCoreLogArchive(const base::FilePath& directory,
                         const std::string& text) {
  if (text.empty() || text.size() > 65536)
    return false;
  std::string gzip;
  if (!compression::GzipCompress(base::as_byte_span(text), &gzip))
    return false;
  base::File file(directory.AppendASCII(kCoreLogArchive),
                  base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  return file.IsValid() && fchmod(file.GetPlatformFile(), 0600) == 0 &&
         file.WriteAtCurrentPosAndCheck(base::as_byte_span(gzip));
}

class CoreLogShareSession::Writer {
 public:
  base::FilePath Write(base::FilePath parent, std::string text) {
    const bool created = parent.empty()
                             ? directory_.CreateUniqueTempDir()
                             : directory_.CreateUniqueTempDirUnderPath(parent);
    // Both calls use mkdtemp on macOS, creating a 0700 directory.
    if (!created || !WriteCoreLogArchive(directory_.GetPath(), text))
      return {};
    return directory_.GetPath().AppendASCII(kCoreLogArchive);
  }

 private:
  base::ScopedTempDir directory_;
};

CoreLogShareSession::CoreLogShareSession() = default;
CoreLogShareSession::~CoreLogShareSession() = default;

void CoreLogShareSession::Start(std::string preview, Ready ready) {
  StartOn(std::move(preview), std::move(ready),
          base::ThreadPool::CreateSequencedTaskRunner(
              {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN}),
          {});
}

void CoreLogShareSession::StartForTesting(
    std::string preview,
    Ready ready,
    scoped_refptr<base::SequencedTaskRunner> runner,
    base::FilePath parent) {
  StartOn(std::move(preview), std::move(ready), std::move(runner),
          std::move(parent));
}

void CoreLogShareSession::StartOn(
    std::string preview,
    Ready ready,
    scoped_refptr<base::SequencedTaskRunner> runner,
    base::FilePath parent) {
  if (shutting_down_ || writer_) {
    std::move(ready).Run({}, {});
    return;
  }
  writer_.emplace(std::move(runner));
  writer_.AsyncCall(&Writer::Write)
      .WithArgs(std::move(parent), std::move(preview))
      .Then(base::BindOnce(&CoreLogShareSession::OnWritten, weak_.GetWeakPtr(),
                           std::move(ready)));
}

void CoreLogShareSession::OnWritten(Ready ready, base::FilePath path) {
  base::ScopedClosureRunner release(
      base::BindOnce(&CoreLogShareSession::Finish, weak_.GetWeakPtr()));
  std::move(ready).Run(std::move(path), std::move(release));
}

void CoreLogShareSession::Finish() {
  weak_.InvalidateWeakPtrs();
  writer_.Reset();
}

void CoreLogShareSession::ShutDown() {
  shutting_down_ = true;
  // Called by Chrome's app-terminating notification while ThreadPool still
  // accepts work. Destruction is ordered behind any queued or running write
  // and blocks shutdown even if its UI reply or picker never completes.
  Finish();
}
}  // namespace aegis::access
