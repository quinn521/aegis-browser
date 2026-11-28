// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_internal_test_core_mac.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <limits>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/process/launch.h"
#include "base/posix/unix_domain_socket.h"
#include "base/strings/string_number_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/aegis/access/access_test_node_native_config.h"
#include "crypto/hash.h"
#include "openssl/mem.h"

namespace aegis::access {
namespace {
using Status = AccessInternalTestCoreLaunchStatusMac;
bool ValidBinding(const AccessInternalTestCoreBindingMac& binding) {
  return base::Uuid::ParseLowercase(binding.profile_id).is_valid() &&
      base::Uuid::ParseLowercase(binding.reference.id).is_valid() &&
      binding.profile_version > 0 &&
      binding.profile_version <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
      binding.authorization_generation > 0 && binding.vault_scope_hash.size() == 64 &&
      std::ranges::all_of(binding.vault_scope_hash, [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
      }) && aegis_access::ValidateAccessInboundPreferences(binding.preferences) ==
                 aegis_access::AccessInboundPreferenceError::kNone;
}
bool WriteConfiguration(int output, base::span<const uint8_t> bytes,
                        base::TimeTicks deadline) {
  while (!bytes.empty()) {
    if (base::TimeTicks::Now() >= deadline) return false;
    pollfd writable{output, POLLOUT, 0};
    const int waited = poll(&writable, 1, 20);
    if (waited < 0 && errno == EINTR) continue;
    if (waited < 0 || (writable.revents & (POLLERR | POLLHUP | POLLNVAL))) return false;
    if (waited == 0 || !(writable.revents & POLLOUT)) continue;
    const auto count = write(output, bytes.data(), bytes.size());
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) return false;
    bytes = bytes.subspan(static_cast<size_t>(count));
  }
  return base::TimeTicks::Now() < deadline;
}
}  // namespace

AccessInternalTestCoreLaunchStatusMac LaunchAccessInternalTestCoreMac(
    const aegis_access::AccessBuildIdentity& identity,
    const base::FilePath& manifest_path,
    const AccessInternalTestCoreBindingMac& binding,
    base::TimeTicks deadline,
    base::ScopedTempDir& directory,
    base::Process& process,
    std::optional<AccessOwnedChildMac>& child_identity,
    const std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease>& reference_lease,
    base::ScopedFD& listener_lease_channel) {
  if (!aegis_access::CanUseAccessOptions(identity, aegis_access::AccessOptionsOperation::kActivate))
    return Status::kNotAdmitted;
  const std::string expected_channel = identity.product == aegis_access::AccessProductChannel::kDebug
      ? "debug" : "alpha";
  const auto remaining = deadline - base::TimeTicks::Now();
  if (!ValidBinding(binding) || binding.channel != expected_channel ||
      directory.IsValid() || process.IsValid() || child_identity || listener_lease_channel.is_valid() ||
      deadline.is_null() ||
      remaining <= base::TimeDelta() || remaining > base::Seconds(25))
    return Status::kInvalidBinding;
  if (!reference_lease || !reference_lease->Matches(binding.channel,
          binding.vault_scope_hash, binding.reference)) return Status::kSecretUnavailable;
  std::string manifest_json;
  if (!manifest_path.IsAbsolute() || manifest_path.ReferencesParent() ||
      !base::ReadFileToStringWithMaxSize(manifest_path, &manifest_json, 65536))
    return Status::kInvalidManifest;
  const auto manifest = base::JSONReader::ReadDict(manifest_json, base::JSON_PARSE_RFC);
  if (!manifest || manifest->FindInt("schemaVersion") != 2 ||
      manifest->FindBool("exclusiveLoopbackTcp") != true ||
      manifest->FindBool("parentExitWatch") != true ||
      manifest->FindBool("parentHeldListenerLease") != true ||
      (binding.preferences.port_mode == aegis_access::AccessInboundPortMode::kSystemAllocated &&
       manifest->FindBool("systemAllocatedTcpPort") != true))
    return Status::kInvalidManifest;
  using Transport = aegis_access::RegisteredProxyTransport;
  if ((binding.selected_transport != Transport::kHttp && binding.selected_transport != Transport::kSocks5) ||
      manifest->FindBool(binding.selected_transport == Transport::kSocks5
                            ? "localSocks5NoAuth" : "localHttpNoAuth") != true)
    return Status::kUnsupportedProtocol;
  const auto* binary_path = manifest->FindString("binaryPath");
  const auto* pin = manifest->FindString("binarySha256");
  if (!binary_path || binary_path->find('\0') != std::string::npos || !pin || pin->size() != 64)
    return Status::kInvalidManifest;
  const auto original = base::FilePath::FromUTF8Unsafe(*binary_path);
  if (!original.IsAbsolute() || original.ReferencesParent()) return Status::kInvalidManifest;
  std::string binary;
  if (!base::ReadFileToStringWithMaxSize(original, &binary, 128 * 1024 * 1024) ||
      binary.empty() || base::HexEncodeLower(crypto::hash::Sha256(base::as_byte_span(binary))) != *pin)
    return Status::kInvalidCorePin;
  AccessTestNodeVaultMac vault(binding.channel, binding.vault_scope_hash);
  const auto node = vault.Read(binding.reference);
  if (!node) return Status::kSecretUnavailable;
  auto configuration = BuildAccessTestNodeCoreConfiguration(*node, binding.preferences, binding.selected_transport);
  if (!configuration) return Status::kConfigurationInvalid;
  base::ScopedClosureRunner clear_configuration(base::BindOnce([](base::span<uint8_t> bytes) {
    OPENSSL_cleanse(bytes.data(), bytes.size());
  }, base::as_writable_byte_span(*configuration)));
  if (base::TimeTicks::Now() >= deadline) return Status::kDeadlineExceeded;
  if (!directory.CreateUniqueTempDir() || !base::SetPosixFilePermissions(directory.GetPath(), 0700))
    return Status::kStageFailed;
  const auto executable = directory.GetPath().AppendASCII("xray");
  {
    base::File file(executable, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
    if (!file.IsValid() || !base::SetPosixFilePermissions(executable, 0700) ||
        !file.WriteAtCurrentPosAndCheck(base::as_byte_span(binary))) return Status::kStageFailed;
  }
  if (base::TimeTicks::Now() >= deadline) return Status::kDeadlineExceeded;
  base::ScopedFD input, output;
  if (!base::CreatePipe(&input, &output, false) ||
      fcntl(output.get(), F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(output.get(), F_SETNOSIGPIPE, 1) < 0) return Status::kStageFailed;
  base::File sink(base::FilePath("/dev/null"), base::File::FLAG_OPEN | base::File::FLAG_WRITE);
  if (!sink.IsValid()) return Status::kStageFailed;
  base::ScopedFD child_lease_channel;
  if (!base::CreateSocketPair(&listener_lease_channel, &child_lease_channel) ||
      fcntl(listener_lease_channel.get(), F_SETFL, O_NONBLOCK) < 0 ||
      fcntl(listener_lease_channel.get(), F_SETFD, FD_CLOEXEC) < 0)
    return Status::kStageFailed;
  base::CommandLine command(executable);
  command.AppendArg("run");
  command.AppendArg("-config");
  command.AppendArg("stdin:");
  base::LaunchOptions options;
  options.clear_environment = true;
  options.environment["AEGIS_OWNED_PARENT_PID"] = base::NumberToString(getpid());
  options.environment["AEGIS_OWNED_LISTENER_LEASE_FD"] = "3";
  options.current_directory = directory.GetPath();
  options.fds_to_remap = {{input.get(), STDIN_FILENO}, {sink.GetPlatformFile(), STDOUT_FILENO},
                         {sink.GetPlatformFile(), STDERR_FILENO}, {child_lease_channel.get(), 3}};
  process = base::LaunchProcess(command, options);
  if (!process.IsValid()) return Status::kLaunchFailed;
  input.reset();
  child_identity = CaptureAccessOwnedChildMac(process.Pid());
  if (!child_identity) return Status::kChildIdentityMismatch;
  if (!WriteConfiguration(output.get(), base::as_byte_span(*configuration), deadline))
    return base::TimeTicks::Now() >= deadline ? Status::kDeadlineExceeded : Status::kWriteFailed;
  return Status::kLaunched;
}
}  // namespace aegis::access
