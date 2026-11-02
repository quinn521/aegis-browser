import json
from concurrent.futures import ThreadPoolExecutor
import os
import platform
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AndroidHostGuardTests(unittest.TestCase):
    def run_guard(self, architecture, experimental=None, *, free_gib=None, minimum="999999", reserved_va=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = root / "bin"
            tools.mkdir()
            uname = tools / "uname"
            uname.write_text('#!/bin/sh\nif [ "$1" = -s ]; then echo Linux; else echo ' + architecture + '; fi\n')
            uname.chmod(0o700)
            if free_gib is not None:
                df = tools / "df"
                df.write_text('#!/bin/sh\nprintf "%s" "${QEMU_RESERVED_VA-unset}" > "'+str(root/'emulation-env')+'"\nprintf "Filesystem 1024-blocks Used Available Capacity Mounted on\\nfixture 104857600 0 '
                              + str(free_gib * 1024 * 1024) + ' 0%% /fixture\\n"\n')
                df.chmod(0o700)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       CHROMIUM_ROOT=str(root))
            env.pop("AEGIS_ANDROID_MIN_FREE_GB", None)
            if minimum is not None:
                env["AEGIS_ANDROID_MIN_FREE_GB"] = minimum
            env.pop("AEGIS_ANDROID_EXPERIMENTAL_ARM64", None)
            env.pop("QEMU_RESERVED_VA", None)
            if reserved_va is not None:
                env['QEMU_RESERVED_VA'] = reserved_va
            if experimental is not None:
                env["AEGIS_ANDROID_EXPERIMENTAL_ARM64"] = experimental
            result = subprocess.run(["bash", str(ROOT / "apps/browser/scripts/build-android.sh")],
                                    env=env, capture_output=True, text=True, timeout=20)
            self.assertFalse((root / "src").exists())
            observed = root/'emulation-env'
            result.emulation_setting = observed.read_text() if observed.exists() else None
            return result

    def test_arm64_requires_explicit_opt_in_before_capacity_or_build(self):
        for cpu in ("aarch64", "arm64"):
            for value in (None, "false", "1"):
                with self.subTest(cpu=cpu, value=value):
                    result = self.run_guard(cpu, value)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("AEGIS_ANDROID_EXPERIMENTAL_ARM64=true", result.stderr)
                    self.assertNotIn("999999", result.stdout)

    def test_opt_in_preserves_capacity_gate_and_does_not_create_output(self):
        for cpu in ("x86_64", "aarch64", "arm64"):
            with self.subTest(cpu=cpu):
                result = self.run_guard(cpu, "true")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("999999GB", result.stdout)

    def test_opt_in_does_not_allow_an_unknown_architecture(self):
        result = self.run_guard("riscv64", "true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("不支持", result.stderr)

    def test_default_capacity_boundary_and_explicit_override(self):
        for free_gib, minimum, blocked in ((39, None, True), (40, None, False),
                                          (72, None, False), (40, "80", True)):
            with self.subTest(free_gib=free_gib, minimum=minimum):
                result = self.run_guard("aarch64", "true", free_gib=free_gib, minimum=minimum)
                self.assertEqual(result.returncode, 2 if blocked else 1)
                if blocked:
                    self.assertIn("先腾空间", result.stdout)
                else:
                    self.assertIn("Chromium src missing", result.stdout)

    def test_guest_address_reservation_is_scoped_to_opted_in_arm64_children(self):
        for cpu in ('aarch64', 'arm64', 'x86_64'):
            result = self.run_guard(cpu, 'true', free_gib=39, minimum=None)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.emulation_setting, 'unset' if cpu == 'x86_64' else '1099511627776')
        for setting in ('0', '64G', 'bad'):
            result = self.run_guard('aarch64', 'true', free_gib=40, minimum=None, reserved_va=setting)
            self.assertEqual(result.returncode, 2)
            self.assertIn('QEMU_RESERVED_VA=1099511627776', result.stderr)
            self.assertIsNone(result.emulation_setting)


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() in ("arm64", "aarch64")
                     and os.environ.get("AEGIS_ANDROID_EXPERIMENTAL_ARM64") == "true",
                     "Explicit Linux ARM64 VM experiment required")
class NativeArm64RuntimeTests(unittest.TestCase):
    def test_actual_pinned_x64_typescript_loader_with_reserved_guest_address_space(self):
        source = os.environ.get('AEGIS_ANDROID_NATIVE_SOURCE')
        if not source:
            self.skipTest('Provide native source for the actual locked TypeScript loader regression')
        compiler = Path(source)/'third_party/typescript/linux-amd64/src/lib/tsc'
        env = dict(os.environ, QEMU_RESERVED_VA='1099511627776')
        def invoke(_):
            result = subprocess.run([str(compiler), '--version'], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), 'Version 7.0.2')
        with ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(invoke, range(128)))

    def test_committed_gn_selects_available_pinned_rust_host_without_changing_android_abi(self):
        gn = os.environ.get("AEGIS_ANDROID_GN_BINARY")
        source = os.environ.get("AEGIS_ANDROID_NATIVE_SOURCE")
        if not gn or not source:
            self.skipTest("Provide the pinned GN binary and native source for the actual host-toolchain regression")
        source = Path(source)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".gn").write_text('buildconfig = "//config.gn"\n')
            (root / "config.gn").write_text('set_default_toolchain("//:tools")\n')
            (root / "BUILD.gn").write_text('''
write_file("$root_build_dir/selected.json", [host_cpu, target_os, target_cpu], "json")
toolchain("tools") {
  tool("stamp") {
    command = "touch {{output}}"
  }
}
group("selected") {}
''')
            args = (ROOT / "apps/browser/args/aegis-android.gn").read_text()
            result = subprocess.run([gn, "gen", str(root / "out"), "--root=" + str(root), "--args=" + args],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            host, target_os, target_cpu = json.loads((root / "out/selected.json").read_text())
            self.assertEqual((host, target_os, target_cpu), ("x64", "android", "arm64"))
            triple = "x86_64-unknown-linux-gnu"
            rust = source / "third_party/rust-toolchain/bin/rustc"
            result = subprocess.run([str(rust), "--print", "target-libdir", "--target", triple],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(Path(result.stdout.strip()).is_dir(), "Host stdlib must exist in the unchanged pinned toolchain")
            probe = root / "probe.rs"
            probe.write_text('pub fn probe() -> String { String::from("pinned Rust host") }\n')
            obj = root / "probe.o"
            result = subprocess.run([str(rust), "--target", triple, "--crate-type=lib", "--emit=obj",
                                     str(probe), "-o", str(obj)], capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = obj.read_bytes()
            self.assertEqual(data[:6], b"\x7fELF\x02\x01")
            self.assertEqual(struct.unpack_from("<H", data, 18)[0], 62)

    def test_vm_executes_the_x64_loader_required_by_pinned_chromium_tools(self):
        loader = Path("/lib64/ld-linux-x86-64.so.2")
        self.assertTrue(loader.is_file(), "Install the guest x86-64 runtime in the operator image")
        result = subprocess.run([str(loader), "--version"], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.strip(), "The loader must actually execute through VM binfmt")


if __name__ == "__main__":
    unittest.main()
