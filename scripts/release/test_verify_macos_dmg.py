import importlib.util
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("dmg_check", Path(__file__).with_name("verify-macos-dmg.py"))
dmg_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dmg_check)
VERSION = "2.2.0.144"


def fixture(volume):
    app = volume / "GCSA-aegis.app"
    binary = app / "Contents/MacOS/GCSA Aegis"
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b"fixture executable")
    binary.chmod(0o755)
    (app / "Contents/Info.plist").write_bytes(plistlib.dumps({
        "CFBundleIdentifier": "com.gcsa.aegis", "CFBundleExecutable": "GCSA Aegis",
        "CFBundlePackageType": "APPL", "AegisProductVersion": VERSION,
        "AegisProductVersionLabel": "Ver 2.2 (144)"}))
    (volume / "Applications").symlink_to("/Applications")
    return app


class DmgVerificationTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.app = fixture(self.root)

    def test_wrong_product_identity_is_rejected_before_signature_commands(self):
        info_path = self.app / "Contents/Info.plist"
        original = plistlib.loads(info_path.read_bytes())
        for field, value in [("CFBundleIdentifier", "app.gcsa.aegis"),
                             ("AegisProductVersion", "2.2.0.143"),
                             ("AegisProductVersionLabel", "Ver 2.2 (143)"),
                             ("CFBundleExecutable", "../../another-program")]:
            with self.subTest(field=field), patch.object(dmg_check, "command") as command:
                info_path.write_bytes(plistlib.dumps({**original, field: value}))
                with self.assertRaisesRegex(ValueError, "identity"):
                    dmg_check.verify_app(self.root, VERSION)
                command.assert_not_called()

    def test_an_extra_or_symlink_app_is_rejected(self):
        (self.root / "stale.app").mkdir()
        with self.assertRaisesRegex(ValueError, "exactly one"):
            dmg_check.verify_app(self.root, VERSION)
        (self.root / "stale.app").rmdir()
        moved = self.root / "hidden"
        self.app.rename(moved)
        self.app.symlink_to(moved, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "exactly one"):
            dmg_check.verify_app(self.root, VERSION)

    def test_wrong_cpu_or_invalid_signature_is_rejected(self):
        with patch.object(dmg_check, "command", return_value=b"x86_64\n"):
            with self.assertRaisesRegex(ValueError, "arm64"):
                dmg_check.verify_app(self.root, VERSION)
        with patch.object(dmg_check, "command", side_effect=[b"arm64\n", subprocess.CalledProcessError(1, "codesign")]):
            with self.assertRaises(subprocess.CalledProcessError):
                dmg_check.verify_app(self.root, VERSION)

    def test_invalid_inputs_never_mount_anything(self):
        dmg = self.root / "empty.dmg"
        dmg.touch()
        with patch.object(dmg_check, "command") as command:
            for path, version in [(dmg, VERSION), (dmg, "0.8.0"), (self.root / "absent.dmg", VERSION)]:
                with self.assertRaises(ValueError):
                    dmg_check.verify(path, version)
            command.assert_not_called()

    def test_mounted_image_is_detached_on_application_failure(self):
        dmg = self.root / "candidate.dmg"
        dmg.write_bytes(b"fixture image")
        calls = []

        def command(*args):
            calls.append(args)
            if args[1] == "attach":
                return plistlib.dumps({"system-entities": [{"dev-entry": "/dev/disk123"}, {"mount-point": args[-1]}]})
            return b""

        with patch.object(dmg_check, "command", side_effect=command), \
                patch.object(dmg_check, "verify_app", side_effect=ValueError("tampered app")):
            with self.assertRaisesRegex(ValueError, "tampered app"):
                dmg_check.verify(dmg, VERSION)
        self.assertEqual([args[1] for args in calls], ["verify", "attach", "detach"])
        self.assertIn("-readonly", calls[1])
        self.assertEqual(calls[2][-1], "/dev/disk123")
        self.assertFalse(Path(calls[1][-1]).exists())

    def test_busy_eject_has_bounded_retries_and_only_forces_the_owned_device(self):
        busy = subprocess.CalledProcessError(16, "hdiutil")
        with patch.object(dmg_check, "command", side_effect=[busy, b""]) as command, \
                patch.object(dmg_check.time, "sleep"):
            dmg_check.detach("/dev/disk123")
            self.assertEqual(command.call_count, 2)
            self.assertNotIn("-force", command.call_args.args)
        with patch.object(dmg_check, "command", side_effect=[busy, busy, busy, b""]) as command, \
                patch.object(dmg_check.time, "sleep"):
            dmg_check.detach("/dev/disk123")
            self.assertEqual(command.call_count, 4)
            self.assertEqual(command.call_args.args, ("/usr/bin/hdiutil", "detach", "/dev/disk123", "-force"))
        with patch.object(dmg_check, "command", side_effect=subprocess.CalledProcessError(1, "hdiutil")) as command:
            with self.assertRaises(subprocess.CalledProcessError):
                dmg_check.detach("/dev/disk123")
            self.assertEqual(command.call_count, 1)


@unittest.skipUnless(sys.platform == "darwin", "real hdiutil/codesign regression requires macOS")
class NativeDmgTests(unittest.TestCase):
    def test_real_image_and_signed_app_then_corruption(self):
        with tempfile.TemporaryDirectory(prefix="aegis-dmg-regression-") as temporary:
            root = Path(temporary).resolve()
            stage = root / "stage"
            stage.mkdir()
            app = fixture(stage)
            binary = app / "Contents/MacOS/GCSA Aegis"
            # Compile a tiny real arm64 executable for the native image test.
            source = root / "main.c"
            source.write_text("int main(void) { return 0; }\n")
            subprocess.run(["/usr/bin/xcrun", "clang", "-arch", "arm64", str(source), "-o", str(binary)],
                           check=True, capture_output=True)
            policy = {"signing": "ad-hoc-not-notarized"}
            subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-",
                            "--timestamp=none", str(app)], check=True, capture_output=True)
            image = root / "fixture.dmg"
            subprocess.run(["/usr/bin/hdiutil", "create", "-srcfolder", str(stage), "-format", "UDZO", str(image)],
                           check=True, capture_output=True)
            result = dmg_check.verify(image, VERSION, policy)
            self.assertEqual(result["status"], "macos_dmg_verified")
            self.assertEqual(result["signing"], policy["signing"])
            self.assertFalse(result["releaseReady"])
            self.assertFalse(result["runtimeAcceptance"])
            with self.assertRaisesRegex(ValueError, "identity"):
                dmg_check.verify(image, "2.2.0.145", policy)
            image.write_bytes(b"corrupt image")
            with self.assertRaises(subprocess.CalledProcessError):
                dmg_check.verify(image, VERSION, policy)


if __name__ == "__main__":
    unittest.main()
