# Bounded tracker validation support

This opt-in test support preserves the existing two tracker GTest names and the
normal target entry when `aegis_current151_tracker_bounded = false` (the default).
It does not change production publication, network, lease or commit behavior.

The selected experiment tests durable publication facts and confirmation in the
fixed Chromium 151 candidate. It covers **two of the original 152 cases**. SQLite,
UI blocking, browser multiprocess behavior and the other 150 cases require their
own evidence. Packaging these files on a newer repository baseline does not
transfer native evidence between Chromium versions.

## Lightweight checks

Use the repository-pinned Python interpreter:

```sh
python3 -I -S -B apps/browser/overlay/tools/aegis/current151_tracker/regressions.py
```

The suite exercises the actual deadline, sticky stop, command boundary, input
budget, result identity, sequencing and accounting code. It includes a real
Python startup-hook positive control and isolation check. These checks invoke no
GN, Ninja, compiler or native tracker binary.

## Execution boundary

The implementation is a source candidate pending independent review and native
qualification. `driver.py` supplies admission/result adapters; it intentionally
has no public native dispatch entry. The private grant loader, actual build
dependency/producer reconciliation and controller-to-owner dispatch integration
are still required. Its build entry always refuses execution with blocker B1.

`current151_owned_leaf` is the sole creator and reaper of one direct child. It
accepts only an original monotonic deadline/boot identity, an admitted Seatbelt
profile, an absent terminal receipt, a working directory and a literal leaf
argv. It never imports a PID or signals a group. The controller keeps stdin open,
sends exactly one `G` after the second guard check, and cancels by closing it.
The child installs the profile, deadline timer and resource limits before exec;
the bounded entry consumes GO and retains an EOF/deadline watchdog through suite
and static destruction. A successful exit is provisional until retirement,
fresh XML, accounting, final input guards and the original deadline pass.

Python admission/checks explicitly use `clock_gettime_ns(CLOCK_MONOTONIC)` and
native checks use `clock_gettime(CLOCK_MONOTONIC)`. The original deadline and boot
identity are never reconstructed. Every guard is followed by a sticky-stop and
deadline check. Finalization preserves an earlier accounting error even if
cleanup also fails; census runs after retirement and again after validation and
the last guard's evidence writes, before the live-deadline acceptance transition.
Listing admission requires zero owner and leaf exit codes, verified retirement,
no cancellation and exact names. Printed names alone cannot advance the sequence.

Before admitting this mechanism, real Darwin qualification must establish timer
inheritance, fork/vfork/spawn denial, controller/owner loss behavior, retirement
and bounded cleanup. The supplied qualification sources cover process-creation
denial, its positive control and clock reconciliation; further loss/retirement
qualification remains open. No successful parser
test or configuration boolean can replace those native observations.

The support target has its own narrow argument mode; tracker entry still permits
only listing or either exact individual case. The no-argument support command denotes denial qualification; it is currently
blocked together with the matching positive control. Noncreating clock and
owner-boundary fixtures have separate strict argument modes:

```text
current151_tracker_support_regressions --aegis-qualification-positive-control
current151_tracker_support_regressions --aegis-qualification-clock-probe
current151_tracker_support_regressions --aegis-qualification-owner-boundaries
```

Both process controls are **REQUIRED/BLOCKED** with
`LOSS_SAFE_PROBE_EXECUTOR_MISSING`. The owner refuses both before profile setup
or child creation. The support entry independently refuses them and direct
`--aegis-spawn-probe` entry; no boolean, environment or receipt can override this.
The paired canary source remains in `RunProcessCanaries()`, behind the same
unconditional blocker. It has not passed, been waived, or been replaced by a
scan of an empty process set.

The missing component is an admitted Darwin probe executor that atomically
protects birth, pre-exec and dyld startup with the original deadline and loss
termination, and retains exact birth-derived identity/exclusive reaping even
when qualifier or owner disappears. The current package establishes no such
executor. A main-body exit73, inherited channel, heartbeat or another ordinary
watchdog does not establish that property. An imported probe PID does not grant
the owner signal or reap authority.

Future matching controls must execute the same canonical support binary, exact
argv/environment/input identities and exec permissions under separately
admitted profiles differing only in process-fork permission. Denial requires
EPERM/EACCES and a matching successful positive spawn whose exact child exits73
and is reaped. Qualification must cover birth-before-spawn-return, pre-exec,
dyld and running phases crossed with cancellation, original expiry, qualifier
watchdog `_exit`, qualifier SIGKILL and owner loss. Observation is at most five
seconds, probe retirement two seconds, within fixed ten-second owner cleanup.
ECHILD/uncertain retirement stays UNVERIFIED; foreign/reused PIDs must receive no
signals. Finding4 remains BLOCKED until that executor and native evidence exist.

For the cross-language clock regression, the controller records explicit
CLOCK_MONOTONIC samples before/after the admitted clock-probe command and passes
the original deadline. `validate_clock_probe` checks the native observation's
domain, sample bracket, unchanged deadline and boot identity. Python tests cover
this reconciliation with fixture observations; all actual native qualification,
positive-control and clock-probe sources are **SOURCE_ONLY_NOT_COMPILED_NOT_RUN**.
No profile, compiler, helper or spawn probe is executed by the lightweight suite.

**B1 remains open:** an owned executor for the actual GN → scripts / Ninja → shell
/ wrappers / compiler descendants and their input lineage has not been supplied.
The direct leaf owner cannot bootstrap itself or authorize that build chain.
Build and native dispatch remain sealed until the coordinator admits a concrete
executor, reviewed inputs, fresh output roots and the single heavy slot.

Known input grammar limits remain refusals: Darwin link filelists and complete
generated GN/Ninja/depfile producer ingestion need additional adapters. Unknown
inputs and historical objects must never be silently admitted.

## Repair02 terminal and input contract

The owner validates its nonblocking controller pipe and, after all setup I/O,
checks pending cancellation, original deadline and boot immediately before
creation. At most one queued GO is retained for forwarding; EOF, errors,
non-GO bytes and repeated GO latch failure. The same fresh boundary runs before
forwarding and after terminal write, fsync and close. Terminal I/O failures latch
the first failure. Cancellation during terminal I/O returns125 without rewriting
the receipt. Cleanup has one fixed ten-second budget; EINTR returns to that
budgeted loop and uncertain retirement permanently revokes signal authority.

The terminal schema is version1, kind `provisional`, exact full owner command,
original `deadline_ns`/`boot`, `reaped`/`stopped`/`go` observations and raw wait
`status`. It never contains `accepted`. This is an intentional incompatibility
with the prior receipt schema: old receipts are refused. `validate_terminal()`
rejects duplicate/unknown fields, incomplete observations, mismatched bindings
and receipts over32KiB. Listing, case result and finalization additionally
require completion from waiting for this invocation's directly owned owner,
actual owner exit0, owner-recorded verified leaf reap/exit0, and fresh final
consumer guards. The controller never imports/reaps the leaf. These completion
objects are trusted adapter inputs; there is still no admitted dispatch adapter,
so fixture values do not establish actual process completion.

Input admission walks the original path components with `lstat` before reducing
`..`, refusing symlinks and non-directory intermediate components. Relative and
absolute `alias/../input.o` and response-file paths cannot admit a lexical current
neighbor while concealing historical bytes. Ordinary real-directory parent paths
remain eligible. Generated producers and filesystem mutation guards remain
separate requirements.

The native owner-boundary fixtures use production gate transitions for terminal
write/fsync/close cancellation, EOF, expiry and I/O errors; pre-create fixtures
require zero creation calls after cancellation/expiry, with a queued-GO success
control. All native fixtures remain **SOURCE_ONLY_NOT_COMPILED_NOT_RUN**.
