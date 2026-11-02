# Android Go runtime native counterfactual

This unsigned diagnostic does not package or sign Aegis. It uses a dedicated
fork branch and a disposable GitHub-hosted Linux x64 worker. It does not change
the two frozen2926 application worktrees, release source refs/pins, readiness,
service-account permissions, protected secrets, default branch or shared runners.

The312-file fixture (2,357,908 bytes;508,733 compressed) preserves the original
`trace/helpers` project/configuration, loaded declarations and necessary source
inputs. The13 upstream tracked source files match DevTools commit
`cd7bfad9bba968ac23ba67e538f9e1d499991d25`; the rest are generated TypeScript
configurations/declarations from the preserved Ninja graph. Copyright and license
files are included. Unread reference source files and all compiler binaries are
excluded from the fixture. Manifest and provenance are retained next to it.

Local ARM64 Linux/QEMU with effective GOMAXPROCS=1 reproduced29 runtime failures
in64 calls to this compact fixture (35 passed), without source changes, OOM or
pid-limit events. This is not a product fix. The native control downloads the
public TypeScript7.0.2 CIPD archive, checks its SHA256 and the exact original
compiler SHA256 before execution, and runs the same noEmit project64 times.
Only existing runtime settings P1 and QEMU VA1TiB are supplied; no extra GC or
compiler-option mitigation is added. Two concurrent processes use two native CPUs.
Each call verifies `/proc/PID/exe` is the compiler rather than an emulator.

Core dumps are disabled. Compiler children receive a fixed minimal environment
with no GitHub, signing or Play credentials. Inputs are readonly; no Chromium
source fetch, build-cache mutation, tool replacement, signing, store upload or
release workflow runs. Artifact collection is limited to diagnostic outcomes
and compiler text logs. A passing native control would narrow the fault to the
local execution path; different kernels/runtime environments still preclude a
unique upstream QEMU/Go/TypeScript root-cause claim.
