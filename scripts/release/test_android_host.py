import os
import platform
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AndroidHostGuardTests(unittest.TestCase):
    def run_guard(self, architecture, experimental=None):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = root / "bin"
            tools.mkdir()
            uname = tools / "uname"
            uname.write_text('#!/bin/sh\nif [ "$1" = -s ]; then echo Linux; else echo ' + architecture + '; fi\n')
            uname.chmod(0o700)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       CHROMIUM_ROOT=str(root), AEGIS_ANDROID_MIN_FREE_GB="999999")
            env.pop("AEGIS_ANDROID_EXPERIMENTAL_ARM64", None)
            if experimental is not None:
                env["AEGIS_ANDROID_EXPERIMENTAL_ARM64"] = experimental
            result = subprocess.run(["bash", str(ROOT / "apps/browser/scripts/build-android.sh")],
                                    env=env, capture_output=True, text=True, timeout=20)
            self.assertFalse((root / "src").exists())
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


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() in ("arm64", "aarch64")
                     and os.environ.get("AEGIS_ANDROID_EXPERIMENTAL_ARM64") == "true",
                     "Explicit Linux ARM64 VM experiment required")
class NativeArm64RuntimeTests(unittest.TestCase):
    def test_vm_executes_the_x64_loader_required_by_pinned_chromium_tools(self):
        loader = Path("/lib64/ld-linux-x86-64.so.2")
        self.assertTrue(loader.is_file(), "Install the guest x86-64 runtime in the operator image")
        result = subprocess.run([str(loader), "--version"], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.strip(), "The loader must actually execute through VM binfmt")


if __name__ == "__main__":
    unittest.main()
