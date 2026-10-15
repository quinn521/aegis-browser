// Copyright 2026 GCSA
// Native boundary canaries. NOT qualified until an admitted Darwin run occurs.
#include "tools/aegis/current151_tracker/bounded_runtime.h"

#include <mach-o/dyld.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <cstring>

extern char** environ;

namespace {
constexpr int kHarmlessProbeExit = 73;

// Called only with a positive PID just returned by our own creation operation.
// No other thread reaps; SIGCHLD auto-reaping is disabled below. A zombie pins
// identity until this function's waitpid; ECHILD/uncertain errors revoke signals.
bool RetireCreatedProbe(pid_t pid, uint64_t original_deadline, int expected_exit) {
  const uint64_t end = std::min<uint64_t>(original_deadline,
      aegis_access::bounded::MonotonicNowNs() + 5000000000ULL);
  bool killed = false;
  uint64_t cleanup_end = end;
  for (;;) {
    int status = 0;
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      return !killed && WIFEXITED(status) && WEXITSTATUS(status) == expected_exit;
    if (result < 0) {
      if (errno != EINTR)
        return false;
      const uint64_t now = aegis_access::bounded::MonotonicNowNs();
      if (!now || now >= cleanup_end)
        return false;
      continue;
    }
    const uint64_t now = aegis_access::bounded::MonotonicNowNs();
    if (!now)
      return false;
    if (now >= end && !killed) {
      // result==0 retains our unreaped-child authority at this point.
      if (kill(pid, SIGKILL) != 0)
        return false;
      killed = true;
      cleanup_end = now + 2000000000ULL;
    } else if (killed && now >= cleanup_end) {
      return false;
    }
    usleep(1000);
  }
}
}  // namespace

// SOURCE_ONLY_NOT_COMPILED_NOT_RUN: retained paired process controls. A future
// admitted executor must protect birth/pre-exec/dyld/running probe against
// cancellation, expiry, qualifier watchdog/_exit/SIGKILL and owner loss. Each
// case requires exact retirement, <=5s observation, <=2s probe retirement and
// <=10s owner cleanup, including ECHILD/foreign/reused-PID no-signal controls.
[[maybe_unused]] int RunProcessCanaries(bool positive_control, uint64_t original_deadline) {
  if (aegis_access::bounded::ProcessCanaryBlocker())
    return 126;
  char executable[PATH_MAX], self[PATH_MAX];
  uint32_t executable_size = sizeof(executable);
  if (_NSGetExecutablePath(executable, &executable_size) != 0 ||
      !realpath(executable, self))
    return 1;
  struct sigaction child_action{};
  child_action.sa_handler = SIG_DFL;
  sigemptyset(&child_action.sa_mask);
  if (sigaction(SIGCHLD, &child_action, nullptr) != 0)
    return 1;
  bool passed = true;
  // If a canary unexpectedly succeeds, reap only the child we created and fail.
  if (!positive_control) {
    const pid_t forked = fork();
    if (forked == 0)
      _exit(kHarmlessProbeExit);
    const int fork_error = errno;
    if (forked > 0) {
      RetireCreatedProbe(forked, original_deadline, kHarmlessProbeExit);
      return 1;
    }
    if (forked < 0 && fork_error != EPERM && fork_error != EACCES)
      return 1;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const pid_t vforked = vfork();
#pragma clang diagnostic pop
    if (vforked == 0)
      _exit(kHarmlessProbeExit);
    const int vfork_error = errno;
    if (vforked > 0) {
      RetireCreatedProbe(vforked, original_deadline, kHarmlessProbeExit);
      return 1;
    }
    if (vforked < 0 && vfork_error != EPERM && vfork_error != EACCES)
      return 1;
  }
  pid_t spawned = -1;
  char mode[] = "--aegis-spawn-probe";
  char* args[] = {self, mode, nullptr};
  const int spawn_result = posix_spawn(&spawned, self, nullptr, nullptr, args, environ);
  if (spawn_result == 0) {
    const bool reaped = spawned > 0 &&
        RetireCreatedProbe(spawned, original_deadline, kHarmlessProbeExit);
    passed = passed && positive_control && reaped;
  } else {
    passed = passed && !positive_control && (spawn_result == EPERM || spawn_result == EACCES);
  }
  printf("%s=%s\n", positive_control ? "self_spawn_positive_control" : "fork_vfork_self_spawn_denied",
         passed ? "true" : "false");
  return passed ? 0 : 1;
}

namespace {
// Pure source fixtures call the PRODUCTION transition helper; no processes or
// I/O are created here. Future native execution remains separately required.
bool OwnerBoundaryFixtures() {
  using aegis_access::bounded::ControllerObservation;
  using aegis_access::bounded::OwnerGate;
  using aegis_access::bounded::ValidateOwnerBoundary;
  using aegis_access::bounded::FinishTerminal;
  for (int phase = 0; phase < 3; ++phase) {  // write / fsync / close
    for (int event = 0; event < 4; ++event) {  // cancel / EOF / expiry / I/O error
      OwnerGate gate;
      gate.Observe(ControllerObservation::kGo);
      gate.go_sent = true;
      int operations = 0;
      bool pending = false;
      auto io = [&] {
        const bool selected = operations++ == phase;
        pending = pending || selected;
        return !(selected && event == 3);
      };
      const bool result = FinishTerminal(&gate, io, io, io, [&] {
        if (!pending || operations != 3)
          return false;
        if (event < 2)
          gate.Observe(event == 0 ? ControllerObservation::kCancel : ControllerObservation::kEof);
        return ValidateOwnerBoundary(&gate, 200, "boot", event == 2 ? 200 : 100, "boot");
      });
      if (result || operations != 3 || !gate.failure)
        return false;
      const int first = gate.failure;
      gate.Fail(126);
      if (gate.failure != first)
        return false;
    }
  }
  OwnerGate normal_terminal;
  normal_terminal.Observe(ControllerObservation::kGo);
  normal_terminal.go_sent = true;
  int normal_calls = 0;
  auto normal_io = [&] { ++normal_calls; return true; };
  if (!FinishTerminal(&normal_terminal, normal_io, normal_io, normal_io, [&] {
        return normal_calls == 3 && ValidateOwnerBoundary(&normal_terminal, 200, "boot", 199, "boot");
      }))
    return false;
  // Simulate cancellation/expiry after setup, at the production pre-create
  // gate. The guarded creation counter must remain zero.
  for (int scenario = 0; scenario < 6; ++scenario) {
    OwnerGate gate;
    gate.Observe(ControllerObservation::kGo);
    if (scenario < 3)
      gate.Observe(scenario == 0 ? ControllerObservation::kCancel :
                   scenario == 1 ? ControllerObservation::kEof : ControllerObservation::kGo);
    int creations = 0;
    if (ValidateOwnerBoundary(&gate, 200, "boot", scenario == 3 ? 200 :
                              scenario == 4 ? 0 : 100, scenario == 5 ? "other" : "boot"))
      ++creations;
    if (creations != 0)
      return false;
  }
  OwnerGate healthy;
  healthy.Observe(ControllerObservation::kGo);  // a single pending GO survives
  if (!ValidateOwnerBoundary(&healthy, 200, "boot", 100, "boot") ||
      !healthy.go_seen || healthy.go_sent)
    return false;
  healthy.go_sent = true;
  return ValidateOwnerBoundary(&healthy, 200, "boot", 199, "boot");
}
}  // namespace

int main(int argc, char** argv) {
  // Direct entry cannot bypass the missing loss-safe executor. Exit73 is a
  // future probe contract, not proof of pre-exec/startup supervision.
  if (argc == 2 && std::strcmp(argv[1], "--aegis-spawn-probe") == 0) {
    fprintf(stderr, "%s\n", aegis_access::bounded::ProcessCanaryBlocker());
    return 126;
  }
  uint64_t original_deadline = 0;
  if (!aegis_access::bounded::EnterQualification(&argc, argv, &original_deadline))
    return 126;
  if (argc == 2 && std::strcmp(argv[1], "--aegis-qualification-clock-probe") == 0) {
    printf("{\"clock\":\"%s\",\"now_ns\":%llu,\"deadline_ns\":%llu,\"boot\":\"%s\"}\n",
           aegis_access::bounded::kDeadlineClockName,
           static_cast<unsigned long long>(aegis_access::bounded::MonotonicNowNs()),
           static_cast<unsigned long long>(original_deadline),
           aegis_access::bounded::BootIdentity().c_str());
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--aegis-qualification-owner-boundaries") == 0)
    return OwnerBoundaryFixtures() ? 0 : 1;
  // Both denial and positive modes refuse; unexpected creation by a denial
  // canary needs the same loss protection. Neither control is waived/passed.
  fprintf(stderr, "%s\n", aegis_access::bounded::ProcessCanaryBlocker());
  return 126;
}
