import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("macos_toolchain", Path(__file__).with_name("verify-macos-toolchain.py"))
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class ToolchainTests(unittest.TestCase):
    def test_an_installed_xcrun_stub_does_not_qualify_a_missing_metal_toolchain(self):
        with patch.object(check, "command", side_effect=["/Applications/Xcode.app/Contents/Developer", "26.5",
                subprocess.CalledProcessError(1, "metal", stderr=b"private log fragment")]) as command:
            with self.assertRaisesRegex(ValueError, "xcodebuild -downloadComponent MetalToolchain") as error:
                check.verify()
            self.assertNotIn("private log fragment", str(error.exception))
            self.assertIn("metal", command.call_args.args)
            self.assertIn("-c", command.call_args.args)
            self.assertNotIn("--find", command.call_args.args)
            self.assertFalse(Path(command.call_args.args[command.call_args.args.index("-c") + 1]).exists())

    def test_success_exit_without_a_compiled_object_is_refused(self):
        with patch.object(check, "command", side_effect=["/Applications/Xcode.app/Contents/Developer", "26.5", ""]):
            with self.assertRaisesRegex(ValueError, "no AIR object"):
                check.verify()

    def test_timeout_leaves_no_temporary_shader(self):
        with patch.object(check, "command", side_effect=["/Applications/Xcode.app/Contents/Developer", "26.5",
                subprocess.TimeoutExpired("metal", 120)]) as command:
            with self.assertRaisesRegex(ValueError, "Metal shader compilation failed"):
                check.verify()
            self.assertFalse(Path(command.call_args.args[command.call_args.args.index("-c") + 1]).exists())


@unittest.skipUnless(sys.platform == "darwin", "real Metal compilation requires macOS and the Xcode component")
class NativeMetalTests(unittest.TestCase):
    def test_actual_shader_compiles_and_does_not_claim_a_product_build(self):
        result = check.verify()
        self.assertTrue(result["metalShaderCompiled"])
        self.assertGreater(result["airBytes"], 0)
        self.assertFalse(result["productBuilt"])
        self.assertFalse(result["releaseReady"])


if __name__ == "__main__":
    unittest.main()
