// Copyright 2026 GCSA
#include "chrome/browser/aegis/access/access_local_core_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <vector>

#include "base/barrier_closure.h"
#include "base/base64.h"
#include "base/check_is_test.h"
#include "base/strings/string_util.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/base_paths.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/process/launch.h"
#include "base/process/kill.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/aegis/access/access_build_channel.h"
#include "chrome/browser/aegis/access/access_buildflags.h"
#include "crypto/hash.h"
#include "net/base/auth.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/load_flags.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/url_util.h"
#include "net/log/net_log_source.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"

#if BUILDFLAG(IS_WIN)
#include "chrome/browser/aegis/access/access_local_core_win.h"
#else
#include <signal.h>
#include <unistd.h>

#include "base/files/file_descriptor_watcher_posix.h"
#include "base/posix/eintr_wrapper.h"
#endif
#include "chrome/browser/aegis/access/access_core_log_share.h"

namespace aegis::access {
std::optional<AccessLocalCoreInput> ResolveAccessLocalCoreInput() {
#if BUILDFLAG(AEGIS_PRIVATE_TEST_NODE_ENABLED)
  if (AccessChannelForCurrentBuild() != BundledAccessTestChannel())
    return std::nullopt;
  base::FilePath assets;
  if (!base::PathService::Get(base::DIR_ASSETS, &assets)) return std::nullopt;
  assets = assets.AppendASCII("AegisAlphaAccess");
  return AccessLocalCoreInput{assets.AppendASCII("core-manifest.json"),
      assets.AppendASCII("node.json"), AccessLocalCoreSource::kPrivateTestBundle};
#elif BUILDFLAG(AEGIS_DEV_LOCAL_NODE_ALLOWED)
  if (AccessChannelForCurrentBuild() != aegis_access::ChannelNamespace::kDev)
    return std::nullopt;
  const auto* command = base::CommandLine::ForCurrentProcess();
  auto manifest = command->GetSwitchValuePath("aegis-access-dev-core-manifest");
  auto node = command->GetSwitchValuePath("aegis-access-dev-node-config");
  if (manifest.empty() || node.empty()) return std::nullopt;
  return AccessLocalCoreInput{std::move(manifest), std::move(node)};
#else
  return std::nullopt;
#endif
}
#if BUILDFLAG(IS_MAC)
bool CanResolveAccessInternalTestCoreInput() {
#if BUILDFLAG(AEGIS_DEV_LOCAL_NODE_ALLOWED)
  if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kActivate) ||
      AccessBuildIdentityForCurrentBuild().product != aegis_access::AccessProductChannel::kDebug)
    return false;
  const auto* command = base::CommandLine::ForCurrentProcess();
  const auto manifest = command->GetSwitchValuePath("aegis-access-dev-core-manifest");
  const GURL canary(command->GetSwitchValueASCII("aegis-access-dev-canary-base-url"));
  return manifest.IsAbsolute() && !manifest.ReferencesParent() && canary.is_valid() &&
         canary.SchemeIs("https") && canary.has_host() && !canary.has_username() &&
         !canary.has_password() && !canary.has_ref() && !canary.has_query() &&
         canary.spec().size() <= 1900;
#else
  return false;
#endif
}
std::optional<AccessLocalCoreInput> ResolveAccessInternalTestCoreInput(
    AccessInternalTestCoreBindingMac binding) {
#if BUILDFLAG(AEGIS_DEV_LOCAL_NODE_ALLOWED)
  if (!CanResolveAccessInternalTestCoreInput()) return std::nullopt;
  const auto* command = base::CommandLine::ForCurrentProcess();
  const auto manifest = command->GetSwitchValuePath("aegis-access-dev-core-manifest");
  const GURL canary(command->GetSwitchValueASCII("aegis-access-dev-canary-base-url"));
  binding.canary_marker = base::Uuid::GenerateRandomV4().AsLowercaseString();
  std::erase(binding.canary_marker, '-');
  binding.canary_url = net::AppendQueryParameter(canary, "nonce", binding.canary_marker);
  AccessLocalCoreInput input;
  input.source = AccessLocalCoreSource::kInternalTestProfile;
  input.manifest = manifest;
  input.internal_test_profile = std::move(binding);
  return input;
#else
  return std::nullopt;
#endif
}
#endif
namespace {

const base::DictValue* SelectedOutbound(const base::DictValue& node) {
  const auto* outbounds = node.FindList("outbounds");
  if (!outbounds || outbounds->empty() || !outbounds->front().is_dict()) {
    return nullptr;
  }
  const auto& outbound = outbounds->front().GetDict();
  const auto* protocol = outbound.FindString("protocol");
  const auto* transport = outbound.FindStringByDottedPath("streamSettings.network");
  const auto* security = outbound.FindStringByDottedPath("streamSettings.security");
  return protocol && *protocol == "vless" && transport && *transport == "tcp" &&
                 security && *security == "reality" ? &outbound : nullptr;
}

}  // namespace

std::string ReadConfiguredAccessNodeId(const AccessLocalCoreInput& input) {
  std::string node_json;
  if (!base::ReadFileToStringWithMaxSize(input.node_config, &node_json, 1024 * 1024)) {
    return {};
  }
  auto node = base::JSONReader::ReadDict(node_json, base::JSON_PARSE_RFC);
  const auto* outbound = node ? SelectedOutbound(*node) : nullptr;
  const auto* servers = outbound ? outbound->FindListByDottedPath("settings.vnext") : nullptr;
  if (!servers || servers->empty() || !servers->front().is_dict()) {
    return {};
  }
  const auto& server = servers->front().GetDict();
  const auto* address = server.FindString("address");
  const int port = server.FindInt("port").value_or(0);
  if (!address || address->empty() || port <= 0 || port > 65535) {
    return {};
  }
  const auto label = base::ToLowerASCII(*address) + "\n" + base::NumberToString(port);
  const auto digest = crypto::hash::Sha256(base::as_byte_span(label));
  return base::HexEncodeLower(base::span(digest).first<6>());
}

namespace {
constexpr auto kCanaryAnnotation = net::DefineNetworkTrafficAnnotation(
    "aegis_access_local_core_canary", R"(
      semantics {
        sender: "Aegis Access local core"
        description: "Checks the authorized Alpha or DEV proxy before retrying a failed navigation."
        trigger: "User asks to retry through the proxy."
        data: "A fixed HTTPS connectivity request and process-local proxy authentication. No browsing URL or cookies."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        cookies_allowed: NO
        setting: "Only the explicitly enabled DEV local-node binding can start this check."
        policy_exception_justification: "User-initiated DEV proxy validation."
      })");
constexpr char kCanary[] = "https://www.gstatic.com/generate_204";
constexpr auto kInternalCanaryAnnotation = net::DefineNetworkTrafficAnnotation(
    "aegis_access_internal_node_canary", R"(
      semantics {
        sender: "Aegis internal test Profile"
        description: "Validates the explicitly selected internal node and owned listener using a controlled HTTPS marker."
        trigger: "User explicitly prepares an internal test Profile."
        data: "A fresh non-secret diagnostic marker. No browsing URL, cookies or node secrets."
        destination: OTHER
        destination_other: "The controlled HTTPS canary configured by the trusted internal test binding."
      }
      policy {
        cookies_allowed: NO
        setting: "Only native DEV/internal Alpha activation can prepare this test binding."
        policy_exception_justification: "Explicit internal test Profile validation."
      })");
std::string Token() { return base::Uuid::GenerateRandomV4().AsLowercaseString(); }
// Refuse control characters before Xray can print a multiline node field as
// an independent diagnostic. Bound redaction work even for large configs.
bool CollectProtectedValues(const base::Value& value,
                            std::vector<std::string>& values,
                            std::string_view field = {}) {
  if (value.is_dict()) {
    for (auto entry : value.GetDict()) {
      if (!CollectProtectedValues(entry.second, values, entry.first))
        return false;
    }
  } else if (value.is_list()) {
    for (const auto& item : value.GetList()) {
      if (!CollectProtectedValues(item, values))
        return false;
    }
  } else if (value.is_string()) {
    const auto& text = value.GetString();
    if (std::any_of(text.begin(), text.end(), [](unsigned char c) {
          return c < 0x20 || c == 0x7f;
        }))
      return false;
    const bool structural = (field == "protocol" && text == "vless") ||
                            (field == "network" && text == "tcp") ||
                            (field == "security" && text == "reality") ||
                            (field == "flow" && text == "xtls-rprx-vision") ||
                            (field == "encryption" && text == "none");
    if (!structural && !text.empty() && values.size() < 256)
      values.push_back(base::ToLowerASCII(text));
  }
  return true;
}
}  // namespace

class AccessLocalCoreManager::ShutdownHandle
    : public std::enable_shared_from_this<ShutdownHandle> {
 public:
  explicit ShutdownHandle(scoped_refptr<base::SequencedTaskRunner> runner)
      : runner_(std::move(runner)) {}

  // Registry access stays on the owning Browser sequence. The worker retains
  // its handle through asynchronous destruction, so removing a service cannot
  // hide a still-running child from session-ending cleanup.
  static std::vector<std::weak_ptr<ShutdownHandle>>& Registry() {
    static base::NoDestructor<std::vector<std::weak_ptr<ShutdownHandle>>> handles;
    return *handles;
  }
  bool stop_requested() const { return stop_requested_.load(); }
  void SetWorker(Worker* worker) { worker_ = worker; }  // Core thread only.

  // A session-ending stop completes once forwarding has stopped and the child
  // is reaped, even while a published Ready address must stay held.
  void RequestStop(base::OnceClosure completion, bool session_ending = false) {
    if (session_ending) session_ending_.store(true);
    stop_requested_.store(true);
    bool stopped;
    {
      base::AutoLock lock(lock_);
      stopped = stopped_;
      if (!stopped) completions_.push_back(std::move(completion));
    }
    if (stopped) {
      std::move(completion).Run();
      return;
    }
    // If the thread is already stopping and rejects the task, destruction of
    // its queued SequenceBound worker completes the same cleanup barrier.
    runner_->PostTask(FROM_HERE, base::BindOnce(
        [](std::shared_ptr<ShutdownHandle> handle) {
          handle->StopOnCoreThread();
        }, shared_from_this()));
  }
  void CompleteStop() {
    std::vector<base::OnceClosure> completions;
    {
      base::AutoLock lock(lock_);
      stopped_ = true;
      completions.swap(completions_);
    }
    for (auto& completion : completions) std::move(completion).Run();
  }

 private:
  void StopOnCoreThread();
  scoped_refptr<base::SequencedTaskRunner> runner_;
  raw_ptr<Worker> worker_ = nullptr;  // Accessed only on the core thread.
  std::atomic_bool stop_requested_{false};
  std::atomic_bool session_ending_{false};
  base::Lock lock_;
  bool stopped_ GUARDED_BY(lock_) = false;
  std::vector<base::OnceClosure> completions_ GUARDED_BY(lock_);
};

class AccessLocalCoreManager::Worker : public net::URLRequest::Delegate {
 public:
  Worker(AccessLocalCoreInput input, std::shared_ptr<ShutdownHandle> shutdown
#if BUILDFLAG(IS_MAC)
      , std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease> reference_lease
#endif
      ) : input_(std::move(input)), shutdown_(std::move(shutdown))
#if BUILDFLAG(IS_MAC)
      , runtime_reference_lease_(std::move(reference_lease))
#endif
      {
    shutdown_->SetWorker(this);
  }
  ~Worker() override {
    const bool retired = StopChild();
#if BUILDFLAG(IS_MAC)
    if (!retired) QuarantineInternalCore();
#endif
    shutdown_->SetWorker(nullptr);
    // The worker is going away. Record confirmed reap while its handle can
    // still complete callbacks. Quarantine holds the independent Network lease.
    if (child_reaped_)
      shutdown_->CompleteStop();
  }

  void Prepare(base::OnceCallback<void(AccessLocalCoreReady)> completion) {
    if (shutdown_->stop_requested()) {
      std::move(completion).Run({.error = "The local proxy is shutting down."});
      return;
    }
    if (completion_) {
      std::move(completion).Run({.error = "Proxy preparation is already running."});
      return;
    }
    if (retirement_pending_ && !StopChild()) {
      std::move(completion).Run({.error = "The previous core has not confirmed retirement.",
                                .retirement_pending = true});
      return;
    }
#if BUILDFLAG(IS_MAC)
    if (IsInternal() && internal_binding_spent_ && !Running()) {
      const bool retired = StopChild();
      std::move(completion).Run({.error = "A new native authorization binding is required.",
                                .retirement_pending = !retired});
      return;
    }
#endif
    if (Running()) {
      Verify(std::move(completion));
      return;
    }
    if (!StopChild(/*release_reference=*/false)) {
      std::move(completion).Run({.error = "The previous core has not confirmed retirement.",
                                .retirement_pending = true});
      return;
    }
    completion_ = std::move(completion);
    if (IsInternal()) {
#if BUILDFLAG(IS_MAC)
      internal_binding_spent_ = true;
#endif
      started_ = base::TimeTicks::Now();
      probe_deadline_ = started_ + base::Seconds(25);
#if BUILDFLAG(IS_MAC)
      if (!input_.internal_preparation_deadline.is_null())
        probe_deadline_ = std::min(probe_deadline_, input_.internal_preparation_deadline);
#endif
      if (probe_deadline_ <= started_) {
        Complete(false, "The original preparation deadline has expired.");
        return;
      }
      timeout_.Start(FROM_HERE, probe_deadline_ - started_, base::BindOnce(
          &Worker::Complete, base::Unretained(this), false,
          "Proxy connectivity check timed out."));
    }
    if (!Launch()) {
      Complete(false, "Cannot start the authorized local proxy.");
      return;
    }
    if (!IsInternal()) {
      started_ = base::TimeTicks::Now();
      probe_deadline_ = started_ + base::Seconds(25);
      timeout_.Start(FROM_HERE, base::Seconds(25), base::BindOnce(
          &Worker::Complete, base::Unretained(this), false,
          "Proxy connectivity check timed out."));
    }
    StartProbe();
  }

  void Verify(base::OnceCallback<void(AccessLocalCoreReady)> completion) {
    if (retirement_pending_) {
      std::move(completion).Run({.error = "The previous core has not confirmed retirement.",
                                .retirement_pending = true});
      return;
    }
    if (shutdown_->stop_requested()) {
      std::move(completion).Run({.error = "The local proxy is shutting down."});
      return;
    }
    if (completion_) {
      std::move(completion).Run({.error = "Proxy preparation is already running."});
      return;
    }
    completion_ = std::move(completion);
    health_.Stop();
    if (!Running()) {
      Complete(false, "The local proxy has stopped.");
      return;
    }
    started_ = base::TimeTicks();  // A recheck never relaunches or retries startup.
    probe_deadline_ = base::TimeTicks::Now() + base::Seconds(25);
    request_.reset();
    context_.reset();
    authenticated_ = false;  // Each readiness check proves a fresh authenticated connection.
    timeout_.Start(FROM_HERE, base::Seconds(25), base::BindOnce(
        &Worker::Complete, base::Unretained(this), false,
        "Proxy connectivity check timed out."));
    StartProbe();
  }

  void Stop(base::OnceClosure completion) {
    StopWithResult(base::BindOnce([](std::shared_ptr<ShutdownHandle> shutdown,
                                     base::OnceClosure done, bool retired) {
      if (retired) std::move(done).Run();
      else shutdown->RequestStop(std::move(done));
    }, shutdown_, std::move(completion)));
  }
  void StopWithResult(base::OnceCallback<void(bool)> completion,
                      bool consumers_revoked = false) {
    bool retired = StopChild(true, consumers_revoked);
#if BUILDFLAG(IS_MAC)
    if (retired && IsInternal() && input_.internal_test_profile) {
      // This stop proves removal only for this manager's consumers. A fresh
      // owner cannot certify an earlier owner's abandoned Network binding.
      retired = ClearInternalQuarantine(*input_.internal_test_profile);
      retirement_pending_ = !retired;
    }
#endif
    if (completion_) {
      AccessLocalCoreReady result;
      result.error = "Proxy preparation was cancelled.";
      result.retirement_pending = !retired;
      std::move(completion_).Run(std::move(result));
    }
    if (retired && shutdown_->stop_requested()) shutdown_->CompleteStop();
    std::move(completion).Run(retired);
  }
  // EndSession waits only until forwarding stops and the child is reaped. The
  // held listener and network_lease_pending_ stay in place, so the address is
  // released only by the Network fence or by process exit.
  void StopForSessionEnding() {
    StopWithResult(base::BindOnce(
        [](Worker* worker, bool) {
          if (worker->child_reaped_)
            worker->shutdown_->CompleteStop();
        },
        base::Unretained(this)));
  }
  void SetStoppedCallback(base::RepeatingCallback<void(std::string)> callback) {
    stopped_callback_ = std::move(callback);
  }

 private:
  bool IsInternal() const {
#if BUILDFLAG(IS_MAC)
    return input_.source == AccessLocalCoreSource::kInternalTestProfile;
#else
    return false;
#endif
  }
  bool Running() {
#if BUILDFLAG(IS_MAC)
    if (IsInternal()) return process_.IsValid() && owned_child_ &&
        AccessOwnedChildStillMatchesMac(*owned_child_);
#endif
    int code = 0;
    return process_.IsValid() &&
        base::GetTerminationStatus(process_.Handle(), &code) ==
            base::TERMINATION_STATUS_STILL_RUNNING;
  }
  bool Launch() {
#if BUILDFLAG(IS_MAC)
    if (IsInternal()) {
      if (!CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kActivate) ||
          !input_.internal_test_profile) return false;
      const auto& binding = *input_.internal_test_profile;
      if (!binding.canary_url.is_valid() || !binding.canary_url.SchemeIs("https") ||
          !binding.canary_url.has_host() || binding.canary_url.has_username() ||
          binding.canary_url.has_password() || binding.canary_url.has_ref() ||
          binding.canary_url.spec().size() > 2048 || binding.canary_marker.size() < 16 ||
          binding.canary_marker.size() > 128 ||
          !std::ranges::all_of(binding.canary_marker, [](unsigned char value) {
            return base::IsAsciiAlpha(value) || base::IsAsciiDigit(value) || value == '-' || value == '_';
          })) return false;
      credentials_.reset();
      registration_ = Token();
      if (!ClearInternalQuarantine(binding)) return false;
      return LaunchAccessInternalTestCoreMac(AccessBuildIdentityForCurrentBuild(),
          input_.manifest, binding, probe_deadline_, directory_, process_, owned_child_,
          runtime_reference_lease_, listener_lease_channel_) ==
              AccessInternalTestCoreLaunchStatusMac::kLaunched;
    }
#endif
    if (input_.source != AccessLocalCoreSource::kDevFiles &&
        input_.source != AccessLocalCoreSource::kPrivateTestBundle) return false;
    std::string manifest_json;
    std::string node_json;
    if (!base::ReadFileToStringWithMaxSize(input_.manifest, &manifest_json, 65536) ||
        !base::ReadFileToStringWithMaxSize(input_.node_config, &node_json, 1024 * 1024))
      return false;
    auto manifest = base::JSONReader::ReadDict(manifest_json, base::JSON_PARSE_RFC);
    auto node = base::JSONReader::ReadDict(node_json, base::JSON_PARSE_RFC);
    if (!manifest || !node) return false;
    base::FilePath binary;
    if (input_.source == AccessLocalCoreSource::kPrivateTestBundle) {
#if BUILDFLAG(AEGIS_PRIVATE_TEST_NODE_ENABLED)
      const auto* channel = manifest->FindString("channel");
      const auto* relative = manifest->FindString("binaryRelativePath");
#if BUILDFLAG(IS_WIN)
      constexpr char kPrivateExecutable[] = "xray.exe";
#else
      constexpr char kPrivateExecutable[] = "xray";
#endif
      const auto* node_name = manifest->FindString("nodeConfigRelativePath");
      const auto* node_pin = manifest->FindString("nodeConfigSha256");
      if (manifest->FindInt("schemaVersion") != 1 || !channel ||
          *channel != LocalAccessIdentityChannel(BundledAccessTestChannel()) ||
          !relative || *relative != kPrivateExecutable || !node_name || *node_name != "node.json" ||
          input_.node_config != input_.manifest.DirName().AppendASCII("node.json") ||
          !node_pin || base::HexEncodeLower(crypto::hash::Sha256(base::as_byte_span(node_json))) != *node_pin)
        return false;
      binary = input_.manifest.DirName().AppendASCII(kPrivateExecutable);
#else
      return false;
#endif
    } else {
      const auto* path = manifest->FindString("binaryPath");
      if (!path) return false;
      if (path->find('\0') != std::string::npos) return false;
      binary = base::FilePath::FromUTF8Unsafe(*path);
    }
    const auto* pin = manifest->FindString("binarySha256");
    if (!pin || !binary.IsAbsolute()) return false;
#if BUILDFLAG(IS_WIN)
    if (!binary.MatchesExtension(FILE_PATH_LITERAL(".exe"))) return false;
#endif
    auto bytes = base::ReadFileToBytes(binary);
    if (!bytes || base::HexEncodeLower(crypto::hash::Sha256(*bytes)) != *pin)
      return false;
    const auto* outbound = SelectedOutbound(*node);
    if (!outbound) return false;
    // The selected outbound is copied privately; no subscription, system
    // routing, original V2Ray process or original input file is changed.
#if BUILDFLAG(IS_WIN)
    if (!CreatePrivateCoreDirectory(directory_)) return false;
    // Launch the exact bytes that passed the pin, from the private directory.
    binary = directory_.GetPath().AppendASCII("xray.exe");
    {
      base::File executable(binary,
                             base::File::FLAG_CREATE | base::File::FLAG_WRITE);
      if (!executable.IsValid() ||
          !executable.WriteAtCurrentPosAndCheck(*bytes)) {
        return false;
      }
    }
#else
    if (!directory_.CreateUniqueTempDir() ||
        !base::SetPosixFilePermissions(directory_.GetPath(), 0700)) return false;
#endif
    auto reservation = std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    if (reservation->Listen(net::IPEndPoint(net::IPAddress::IPv4Localhost(), 0),
                            1, std::nullopt) != net::OK) return false;
    net::IPEndPoint address;
    if (reservation->GetLocalAddress(&address) != net::OK) return false;
    port_ = address.port();
    const auto user = Token();
    const auto password = Token();
    credentials_.emplace(base::UTF8ToUTF16(user), base::UTF8ToUTF16(password));
    registration_ = Token();
    std::string level = "none";
    protected_values_ = {user, password, registration_,
                         base::ToLowerASCII(base::Base64Encode(user + ":" + password))};
    if (!CollectProtectedValues(base::Value(outbound->Clone()), protected_values_))
      return false;
    if (protected_values_.size() >= 256)
      protected_values_ = {""};  // A saturated budget suppresses every record.
    if (input_.log_store) {
      input_.log_store->SetProtectedValues(protected_values_);
      std::tie(level, log_generation_) = input_.log_store->LaunchConfig();
    }
    auto log = base::DictValue().Set("loglevel", level);
    if (CoreLogRank(level) >= 2) log.Set("access", "none");
    auto config = base::DictValue()
        .Set("log", std::move(log))
        .Set("inbounds", base::ListValue().Append(base::DictValue()
            .Set("listen", "127.0.0.1").Set("port", port_).Set("protocol", "http")
            .Set("settings", base::DictValue().Set("accounts", base::ListValue()
                .Append(base::DictValue().Set("user", user).Set("pass", password))))))
        .Set("outbounds", base::ListValue().Append(outbound->Clone()));
    const auto config_path = directory_.GetPath().AppendASCII("config.json");
    std::string serialized;
    if (!base::JSONWriter::Write(config, &serialized)) return false;
    // Create private before writing secret bytes, rather than chmod after.
    base::File file(config_path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
    if (!file.IsValid()) return false;
#if BUILDFLAG(IS_MAC)
    if (!base::SetPosixFilePermissions(config_path, 0600)) return false;
#endif
    if (!file.WriteAtCurrentPosAndCheck(base::as_byte_span(serialized))) return false;
    file.Close();
    base::CommandLine command(binary);
    command.AppendArg("run");
    command.AppendArg("-config");
    command.AppendArgPath(config_path);
    // Release the reservation immediately before launching. A port race is a
    // preparation failure: only the authenticated HTTPS probe may admit it.
#if BUILDFLAG(IS_WIN)
    reservation.reset();
    process_ = LaunchOwnedCore(command, directory_.GetPath(), job_,
                               level != "none" ? &log_read_ : nullptr);
    if (log_read_.IsValid()) {
      log_clear_epoch_ = input_.log_store->RegisterPipe(log_read_.GetPlatformFile());
      log_poll_.Start(FROM_HERE, base::Milliseconds(100), this, &Worker::DrainLog);
    }
#else
    base::LaunchOptions options;
    options.clear_environment = true;
    options.current_directory = directory_.GetPath();
    reservation.reset();
    base::File null_output(base::FilePath("/dev/null"),
                           base::File::FLAG_OPEN | base::File::FLAG_WRITE);
    if (!null_output.IsValid()) return false;
    base::ScopedFD read_end, write_end;
    int output = null_output.GetPlatformFile();
    auto create_log_pipe = input_.create_log_pipe_for_testing
                               ? input_.create_log_pipe_for_testing : &base::CreatePipe;
    if (level != "none" && create_log_pipe(&read_end, &write_end, /*non_blocking=*/true)) {
      // Diagnostic capture is optional; a failed pipe keeps /dev/null.
      log_read_ = base::File(std::move(read_end));
      output = write_end.get();
      log_clear_epoch_ = input_.log_store->RegisterPipe(log_read_.GetPlatformFile());
      log_watch_ = base::FileDescriptorWatcher::WatchReadable(
          log_read_.GetPlatformFile(),
          base::BindRepeating(&Worker::DrainLog, base::Unretained(this)));
    }
    options.fds_to_remap = {{output, STDOUT_FILENO}, {output, STDERR_FILENO}};
    process_ = base::LaunchProcess(command, options);
#endif
    return process_.IsValid();
  }
  void DrainLog() {
    std::array<char, 4096> chunk;
    for (int i = 0; i < 16 && log_read_.IsValid(); ++i) {
      const auto [count, error, epoch, prefix] =
          input_.log_store->ReadPipe(log_read_, chunk);
      if (epoch != log_clear_epoch_) {
        log_clear_epoch_ = epoch;
        framer_.DiscardPartial();
        discard_until_empty_ = !prefix.has_value();
        discard_bytes_ = prefix.value_or(0);
      }
      if (error == EAGAIN || error == EWOULDBLOCK) {
        discard_until_empty_ = false;
        discard_bytes_ = 0;
        return;
      }
      if (!count || *count == 0) {
        CloseLogPipe();
        break;
      }
      std::string_view bytes(chunk.data(), *count);
      const size_t discard = discard_until_empty_
                                 ? bytes.size() : std::min(discard_bytes_, bytes.size());
      if (discard) {
        framer_.Discard(bytes.substr(0, discard));
        if (!discard_until_empty_)
          discard_bytes_ -= discard;
        bytes.remove_prefix(discard);
      }
      framer_.Feed(bytes, [this, epoch](std::string_view line) {
        input_.log_store->Add(log_generation_, epoch, line, protected_values_);
      });
    }
  }
  void CloseLogPipe() {
#if BUILDFLAG(IS_WIN)
    log_poll_.Stop();
#else
    log_watch_.reset();
#endif
    if (log_read_.IsValid()) {
      input_.log_store->UnregisterPipe(log_read_.GetPlatformFile());
      log_read_.Close();
    }
  }
  void StartProbe() {
    if (!Running()) {
      Complete(false, "The local proxy exited before becoming ready.");
      return;
    }
#if BUILDFLAG(IS_MAC)
    if (IsInternal()) {
      if (!owned_child_ || base::TimeTicks::Now() >= probe_deadline_) {
        Complete(false, "Owned listener readiness is unavailable.");
        return;
      }
      if (!listener_proof_) {
        const auto observed = InspectAccessOwnedListenerMac(*owned_child_);
        if (observed.status == AccessOwnedListenerStatusMac::kNotListening) {
          retry_.Start(FROM_HERE, base::Milliseconds(25),
              base::BindOnce(&Worker::StartProbe, base::Unretained(this)));
          return;
        }
        if (!observed.proof) {
          Complete(false, "The listener does not match this owned core.");
          return;
        }
        const auto& binding = *input_.internal_test_profile;
        if ((binding.selected_transport == aegis_access::RegisteredProxyTransport::kSocks5 &&
             observed.proof->port == 1080) ||
            (binding.preferences.port_mode == aegis_access::AccessInboundPortMode::kInternalFixed &&
             observed.proof->port != binding.preferences.requested_port)) {
          Complete(false, "The owned listener returned a disallowed port.");
          return;
        }
        listener_proof_ = observed.proof;
        port_ = listener_proof_->port;
      }
      if (!held_listener_lease_.is_valid()) {
        auto lease = ReceiveAccessListenerLeaseMac(listener_lease_channel_.get(),
                                                  *owned_child_, *listener_proof_);
        if (lease.status == AccessListenerLeaseStatusMac::kPending) {
          retry_.Start(FROM_HERE, base::Milliseconds(25),
              base::BindOnce(&Worker::StartProbe, base::Unretained(this)));
          return;
        }
        if (lease.status != AccessListenerLeaseStatusMac::kReady) {
          Complete(false, "The core did not provide its owned listener lease.");
          return;
        }
        held_listener_lease_ = std::move(lease.descriptor);
        listener_lease_channel_.reset();
      }
      if (!AccessOwnedListenerStillMatchesMac(*owned_child_, *listener_proof_)) {
        Complete(false, "The owned listener changed before validation.");
        return;
      }
      if (input_.skip_internal_canary_for_testing) {
        CHECK_IS_TEST();
        Complete(true, std::string());
        return;
      }
    }
#endif
    request_.reset();
    if (!context_) {
      net::ProxyConfig proxy;
      std::string protocol = "http";
#if BUILDFLAG(IS_MAC)
      if (IsInternal() && input_.internal_test_profile->selected_transport ==
                              aegis_access::RegisteredProxyTransport::kSocks5) protocol = "socks5";
#endif
      proxy.proxy_rules().ParseFromString(protocol + "://127.0.0.1:" + base::NumberToString(port_));
      if (IsInternal()) proxy.proxy_rules().bypass_rules.AddRuleFromString("<-loopback>");
      net::URLRequestContextBuilder builder;
      builder.DisableHttpCache();
      builder.set_proxy_config_service(std::make_unique<net::ProxyConfigServiceFixed>(
          net::ProxyConfigWithAnnotation(proxy, kCanaryAnnotation)));
      context_ = builder.Build();
    }
    auth_attempts_ = 0;
    GURL target(kCanary);
#if BUILDFLAG(IS_MAC)
    if (IsInternal()) target = input_.internal_test_profile->canary_url;
#endif
    canary_body_.clear();
    canary_buffer_.reset();
    request_ = context_->CreateRequest(target, net::DEFAULT_PRIORITY,
                                       this, IsInternal() ? kInternalCanaryAnnotation : kCanaryAnnotation,
                                       net::handles::kInvalidNetworkHandle);
    request_->SetLoadFlags(net::LOAD_DISABLE_CACHE | net::LOAD_DO_NOT_SAVE_COOKIES);
    request_->set_disallow_credentials();
    request_->set_site_for_cookies(net::SiteForCookies());
    request_->Start();
  }
  void OnAuthRequired(net::URLRequest* request,
                      const net::AuthChallengeInfo& challenge) override {
    if (IsInternal()) {
      Complete(false, "The internal listener unexpectedly requested authentication.");
      return;
    }
    if (!challenge.is_proxy || challenge.challenger.host() != "127.0.0.1" ||
        challenge.challenger.port() != port_ || ++auth_attempts_ > 1) {
      Complete(false, "Local proxy authentication failed.");
      return;
    }
    authenticated_ = true;
    request->SetAuth(*credentials_);
  }
  void OnReceivedRedirect(net::URLRequest*, const net::RedirectInfo&,
                          bool* defer_redirect) override {
    *defer_redirect = true;
    Complete(false, "Unexpected proxy connectivity response.");
  }
  void OnSSLCertificateError(net::URLRequest*, int,
                             const net::SSLInfo&, bool) override {
    Complete(false, "Proxy connectivity certificate verification failed.");
  }
  void OnResponseStarted(net::URLRequest* request, int error) override {
    if (IsInternal()) {
      if (error != net::OK || request->GetResponseCode() != 200) {
        Complete(false, "The internal node could not validate the controlled HTTPS origin.");
        return;
      }
      canary_buffer_ = base::MakeRefCounted<net::IOBufferWithSize>(4096);
      ReadCanaryBody();
      return;
    }
    if (error == net::ERR_PROXY_CONNECTION_FAILED && !started_.is_null() &&
        base::TimeTicks::Now() - started_ < base::Seconds(3) && Running()) {
      retry_.Start(FROM_HERE, base::Milliseconds(100),
                   base::BindOnce(&Worker::StartProbe, base::Unretained(this)));
      return;
    }
    Complete(error == net::OK && request->GetResponseCode() == 204 && authenticated_,
              "The authorized proxy could not complete the HTTPS check.");
  }
  bool AppendCanaryRead(int count) {
    if (!canary_buffer_) return false;
    if (count < 0 || count > canary_buffer_->size() ||
        canary_body_.size() + static_cast<size_t>(count) > 8192) {
      Complete(false, "The controlled HTTPS marker could not be verified.");
      return false;
    }
    if (count == 0) {
      bool verified = false;
#if BUILDFLAG(IS_MAC)
      verified = IsInternal() && owned_child_ && listener_proof_ &&
          AccessOwnedListenerStillMatchesMac(*owned_child_, *listener_proof_) &&
          canary_body_.find(input_.internal_test_profile->canary_marker) != std::string::npos;
#endif
      Complete(verified, "The controlled HTTPS marker could not be verified.");
      return false;
    }
    canary_body_.append(base::as_string_view(canary_buffer_->first(static_cast<size_t>(count))));
    return true;
  }
  void ReadCanaryBody() {
    while (request_ && canary_buffer_) {
      const int count = request_->Read(canary_buffer_.get(), canary_buffer_->size());
      if (count == net::ERR_IO_PENDING || !AppendCanaryRead(count)) return;
    }
  }
  void OnReadCompleted(net::URLRequest*, int count) override {
    if (IsInternal() && AppendCanaryRead(count)) ReadCanaryBody();
  }
  void Complete(bool ready, std::string error) {
    ready = ready && !retirement_pending_ && Running();
#if BUILDFLAG(IS_MAC)
    if (IsInternal()) ready = ready && owned_child_ && listener_proof_ &&
        AccessHeldListenerStillMatchesMac(held_listener_lease_.get(), *listener_proof_) &&
        AccessOwnedListenerStillMatchesMac(*owned_child_, *listener_proof_);
#endif
    timeout_.Stop();
    retry_.Stop();
    request_.reset();
    canary_buffer_.reset();
    canary_body_.clear();
    AccessLocalCoreReady result;
    result.ready = ready;
    if (ready) {
      result.port = port_;
      result.registration_id = registration_;
      result.local_credentials = credentials_;
      result.transport = aegis_access::RegisteredProxyTransport::kHttp;
#if BUILDFLAG(IS_MAC)
      if (IsInternal()) {
        const auto& binding = *input_.internal_test_profile;
        result.transport = binding.selected_transport;
        result.no_local_auth = true;
        result.profile_id = binding.profile_id;
        result.profile_version = binding.profile_version;
        result.authorization_generation = binding.authorization_generation;
        result.listener_socket_generation = listener_proof_->socket_generation;
        result.parent_listener_lease_held = true;
        // Once native callers can publish this Ready result, implicit cleanup
        // may stop the child but cannot release its address without a fence.
        network_lease_pending_ = true;
      }
#endif
      health_.Start(FROM_HERE, base::Seconds(1), base::BindRepeating(
          &Worker::CheckChild, base::Unretained(this)));
    } else {
      result.error = std::move(error);
      result.retirement_pending = !StopChild();
      if (result.retirement_pending)
        result.error = "The previous core has not confirmed retirement.";
    }
    if (completion_) std::move(completion_).Run(std::move(result));
  }
  void CheckChild() {
    bool alive = Running();
#if BUILDFLAG(IS_MAC)
    if (IsInternal()) alive = alive && owned_child_ && listener_proof_ &&
        AccessHeldListenerStillMatchesMac(held_listener_lease_.get(), *listener_proof_) &&
        AccessOwnedListenerStillMatchesMac(*owned_child_, *listener_proof_);
#endif
    if (alive) return;
    const auto registration = registration_;
    StopChild();
    if (stopped_callback_) stopped_callback_.Run(registration);
  }
#if BUILDFLAG(IS_MAC)
  using RetirementFunction = AccessOwnedChildRetirementMac (*)(const AccessOwnedChildMac&, uint32_t);
  static bool RetireInternalProcess(base::Process& process,
      std::optional<AccessOwnedChildMac>& child, RetirementFunction retire = nullptr) {
    if (!process.IsValid()) return true;
    if (!child) child = CaptureAccessOwnedChildMac(process.Pid());
    bool confirmed = false;
    if (child) {
      if (child->pid() != process.Pid()) return false;
      confirmed = (retire ? retire : &RetireAccessOwnedChildMac)(*child, 2000) ==
          AccessOwnedChildRetirementMac::kRetired;
    } else {
      // An already-exited launch may never have exposed an observable birth.
      // No signal is allowed when that identity could not be established.
      confirmed = ReapAccessExitedChildMac(process.Pid());
    }
    if (!confirmed) return false;
    process.Close();
    child.reset();
    return true;
  }
  struct QuarantinedCore {
    std::string scope;
    std::string profile;
    base::Process process;
    std::optional<AccessOwnedChildMac> child;
    base::ScopedTempDir directory;
    base::ScopedFD held_listener;
    base::ScopedFD listener_channel;
    bool network_lease_pending;
    std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease> reference_lease;
    std::shared_ptr<ShutdownHandle> shutdown;
  };
  static base::Lock& QuarantineLock() {
    static base::NoDestructor<base::Lock> lock;
    return *lock;
  }
  static std::vector<std::unique_ptr<QuarantinedCore>>& Quarantine() {
    // Intentionally retained until exact retirement or application exit. The
    // admitted core also watches its parent; closing a UI/Profile cannot hide
    // an unconfirmed live child or make this binding available again.
    static base::NoDestructor<std::vector<std::unique_ptr<QuarantinedCore>>> entries;
    return *entries;
  }
  void QuarantineInternalCore() {
    CHECK(IsInternal() && input_.internal_test_profile);
    const auto& binding = *input_.internal_test_profile;
    auto entry = std::make_unique<QuarantinedCore>(binding.vault_scope_hash,
        binding.profile_id, std::move(process_), std::move(owned_child_),
        std::move(directory_), std::move(held_listener_lease_), std::move(listener_lease_channel_),
        network_lease_pending_, std::move(runtime_reference_lease_), shutdown_);
    base::AutoLock lock(QuarantineLock());
    Quarantine().push_back(std::move(entry));
  }
  static bool ClearInternalQuarantine(const AccessInternalTestCoreBindingMac& binding) {
    std::vector<std::shared_ptr<ShutdownHandle>> completed;
    bool retired = true;
    {
      base::AutoLock lock(QuarantineLock());
      auto& entries = Quarantine();
      for (auto entry = entries.begin(); entry != entries.end();) {
        auto& old = **entry;
        if (old.scope != binding.vault_scope_hash || old.profile != binding.profile_id) {
          ++entry;
          continue;
        }
        if (!RetireInternalProcess(old.process, old.child)) {
          retired = false;
          break;
        }
        completed.push_back(old.shutdown);
        if ((old.directory.IsValid() && !old.directory.Delete()) ||
            old.network_lease_pending) {
          retired = false;
          break;
        }
        entry = entries.erase(entry);
      }
    }
    for (auto& handle : completed) handle->CompleteStop();
    return retired;
  }
  bool WaitForChildExit(base::TimeDelta maximum_wait) {
    auto deadline = maximum_wait.is_max()
        ? base::TimeTicks::Max() : base::TimeTicks::Now() + maximum_wait;
    bool session_bounded = false;
    for (;;) {
      // A session-ending request must also interrupt a Stop or destructor
      // already waiting on this thread. Retain the ordinary SIGTERM grace,
      // but bound each graceful/forced wait to one second during EndSession.
      if (!session_bounded && shutdown_->stop_requested()) {
        deadline = std::min(deadline, base::TimeTicks::Now() + base::Seconds(1));
        session_bounded = true;
      }
      const auto remaining = deadline - base::TimeTicks::Now();
      if (process_.WaitForExitWithTimeout(
              std::clamp(remaining, base::TimeDelta(), base::Milliseconds(100)),
              nullptr)) return true;
      if (remaining <= base::TimeDelta()) return false;
    }
  }
#endif
  bool StopChild(bool release_reference = true, bool consumers_revoked = false) {
    child_reaped_ = false;
    if (IsInternal()) retirement_pending_ = true;
#if BUILDFLAG(IS_MAC)
    if (IsInternal() && release_reference) internal_binding_spent_ = true;
#endif
    health_.Stop();
    timeout_.Stop();
    retry_.Stop();
    request_.reset();
    context_.reset();
    canary_buffer_.reset();
    canary_body_.clear();
#if BUILDFLAG(IS_WIN)
    StopOwnedCore(job_, directory_);
    process_.Close();
#else
    if (IsInternal()) {
      if (!RetireInternalProcess(process_, owned_child_, input_.retire_owned_child_for_testing))
        return false;
    } else if (process_.IsValid()) {
      if (Running()) {
        process_.Terminate(0, false);
        if (!WaitForChildExit(base::Seconds(60)) &&
            kill(process_.Pid(), SIGKILL) == 0) {
          WaitForChildExit(base::TimeDelta::Max());
        }
      }
      process_.Close();
    }
#endif
    child_reaped_ = true;
    DrainLog();
    CloseLogPipe();
    framer_ = CoreLogFramer();
    discard_bytes_ = 0;
    discard_until_empty_ = false;
    protected_values_.clear();
    credentials_.reset();
    authenticated_ = false;
#if BUILDFLAG(IS_MAC)
    if (IsInternal() && network_lease_pending_ && !consumers_revoked) {
      // The child is reaped; only the published address remains held.
      return false;
    }
    if (consumers_revoked) network_lease_pending_ = false;
    held_listener_lease_.reset();
    listener_lease_channel_.reset();
    owned_child_.reset();
    listener_proof_.reset();
    if (IsInternal()) {
      port_ = 0;
      registration_.clear();
      if (directory_.IsValid() && !directory_.Delete()) return false;
      if (release_reference) runtime_reference_lease_.reset();
    } else {
      const bool deleted = directory_.Delete();
      (void)deleted;
    }
#endif
    retirement_pending_ = false;
    return true;
  }

  AccessLocalCoreInput input_;
  std::shared_ptr<ShutdownHandle> shutdown_;
#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle job_;
  base::RepeatingTimer log_poll_;
#else
  std::unique_ptr<base::FileDescriptorWatcher::Controller> log_watch_;
#endif
  base::File log_read_;
  CoreLogFramer framer_;
  uint64_t log_clear_epoch_ = 0;
  size_t discard_bytes_ = 0;
  bool discard_until_empty_ = false;
  std::vector<std::string> protected_values_;
  uint64_t log_generation_ = 0;
  base::ScopedTempDir directory_;
  base::Process process_;
  uint16_t port_ = 0;
  std::string registration_;
  std::optional<net::AuthCredentials> credentials_;
  bool authenticated_ = false;
  bool retirement_pending_ = false;
  bool child_reaped_ = false;
  int auth_attempts_ = 0;
  base::TimeTicks started_;
  base::TimeTicks probe_deadline_;
#if BUILDFLAG(IS_MAC)
  std::optional<AccessOwnedChildMac> owned_child_;
  std::optional<AccessOwnedListenerProofMac> listener_proof_;
  std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease> runtime_reference_lease_;
  base::ScopedFD held_listener_lease_;
  base::ScopedFD listener_lease_channel_;
  bool network_lease_pending_ = false;
  bool internal_binding_spent_ = false;
#endif
  base::OneShotTimer timeout_;
  base::OneShotTimer retry_;
  base::RepeatingTimer health_;
  std::unique_ptr<net::URLRequestContext> context_;
  std::unique_ptr<net::URLRequest> request_;
  scoped_refptr<net::IOBufferWithSize> canary_buffer_;
  std::string canary_body_;
  base::OnceCallback<void(AccessLocalCoreReady)> completion_;
  base::RepeatingCallback<void(std::string)> stopped_callback_;
};

void AccessLocalCoreManager::ShutdownHandle::StopOnCoreThread() {
  if (!worker_) {
    // Destruction can leave a live child in quarantine. Only a confirmed reap
    // by the worker or quarantine cleanup can complete this handle.
    return;
  }
  if (session_ending_.load()) {
    worker_->StopForSessionEnding();
    return;
  }
  worker_->StopWithResult(base::BindOnce([](std::shared_ptr<ShutdownHandle> handle, bool retired) {
    if (retired) handle->CompleteStop();
  }, shared_from_this()));
}

AccessLocalCoreManager::AccessLocalCoreManager(AccessLocalCoreInput input) {
  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  thread_ = std::make_unique<base::Thread>("AccessLocalCore");
  CHECK(thread_->StartWithOptions(std::move(options)));
  shutdown_ = std::make_shared<ShutdownHandle>(thread_->task_runner());
  auto& handles = ShutdownHandle::Registry();
  std::erase_if(handles, [](const auto& handle) { return handle.expired(); });
  handles.emplace_back(shutdown_);
#if BUILDFLAG(IS_MAC)
  std::shared_ptr<AccessTestNodeVaultMac::RuntimeLease> reference_lease;
  if (input.source == AccessLocalCoreSource::kInternalTestProfile && input.internal_test_profile &&
      CanUseAccessOptionsForCurrentBuild(aegis_access::AccessOptionsOperation::kActivate)) {
    const auto& binding = *input.internal_test_profile;
    const std::string expected_channel =
        AccessBuildIdentityForCurrentBuild().product == aegis_access::AccessProductChannel::kDebug
            ? "debug" : "alpha";
    if (binding.channel == expected_channel)
      reference_lease = AccessTestNodeVaultMac::ReserveRuntimeReference(
          binding.channel, binding.vault_scope_hash, binding.reference);
  }
#endif
  worker_ = base::SequenceBound<Worker>(thread_->task_runner(), std::move(input),
                                      shutdown_
#if BUILDFLAG(IS_MAC)
                                      , std::move(reference_lease)
#endif
                                      );
}
AccessLocalCoreManager::~AccessLocalCoreManager() {
  worker_.Reset();
  thread_->DetachFromSequence();
  base::ThreadPool::PostTask(FROM_HERE,
      {base::MayBlock(), base::WithBaseSyncPrimitives(),
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce([](std::unique_ptr<base::Thread> thread) { thread->Stop(); },
                     std::move(thread_)));
}
void AccessLocalCoreManager::Prepare(
    base::OnceCallback<void(AccessLocalCoreReady)> completion) {
  worker_.AsyncCall(&Worker::Prepare).WithArgs(
      base::BindPostTaskToCurrentDefault(std::move(completion)));
}
void AccessLocalCoreManager::Verify(
    base::OnceCallback<void(AccessLocalCoreReady)> completion) {
  worker_.AsyncCall(&Worker::Verify).WithArgs(
      base::BindPostTaskToCurrentDefault(std::move(completion)));
}
void AccessLocalCoreManager::Stop(base::OnceClosure completion) {
  worker_.AsyncCall(&Worker::Stop).WithArgs(
      base::BindPostTaskToCurrentDefault(std::move(completion)));
}
#if BUILDFLAG(IS_MAC)
void AccessLocalCoreManager::StopForwarding() {
  worker_.AsyncCall(&Worker::StopWithResult).WithArgs(
      base::OnceCallback<void(bool)>(base::DoNothing()), false);
}

void AccessLocalCoreManager::StopConfirmed(base::OnceCallback<void(bool)> completion) {
  worker_.AsyncCall(&Worker::StopWithResult).WithArgs(
      base::BindPostTaskToCurrentDefault(std::move(completion)), true);
}
#endif
void AccessLocalCoreManager::StopAllForSessionEnding(base::OnceClosure completion) {
  std::vector<std::shared_ptr<ShutdownHandle>> handles;
  for (auto& weak : ShutdownHandle::Registry()) {
    if (auto handle = weak.lock()) handles.push_back(std::move(handle));
  }
  auto stopped = base::BarrierClosure(
      handles.size() + 1,
      base::BindOnce(&AccessCoreLogStore::FlushAllForSessionEnding,
                     std::move(completion)));
  CoreLogShareSession::StopAllForSessionEnding(base::OnceClosure(stopped));
  for (auto& handle : handles)
    handle->RequestStop(base::OnceClosure(stopped), /*session_ending=*/true);
}
void AccessLocalCoreManager::SetStoppedCallback(
    base::RepeatingCallback<void(std::string)> callback) {
  worker_.AsyncCall(&Worker::SetStoppedCallback).WithArgs(
      base::BindPostTaskToCurrentDefault(std::move(callback)));
}
}  // namespace aegis::access
