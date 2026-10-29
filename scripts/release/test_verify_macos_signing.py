import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import macos_signing

spec = importlib.util.spec_from_file_location("signing_check", Path(__file__).with_name("verify-macos-signing.py"))
signing_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signing_check)


class SigningPolicyTests(unittest.TestCase):
    def policy(self, text):
        with tempfile.TemporaryDirectory() as temporary:
            browser = Path(temporary)
            (browser / "scripts").mkdir()
            (browser / "scripts/sign-chromium-app.sh").write_text(text)
            return macos_signing.load_policy(browser)

    def test_existing_native_certificate_and_team_are_retained(self):
        policy = macos_signing.load_policy()
        self.assertEqual(policy["certificateSha1"], "7C33B517942161311A087CE893D2F1CE9B204B44")
        self.assertEqual(policy["teamIdentifier"], "R3JK7R29AC")
        self.assertIn("anchor apple generic", policy["requirement"])
        self.assertIn(policy["certificateSha1"], policy["requirement"])
        self.assertIn(policy["teamIdentifier"], policy["requirement"])

    def test_only_explicit_native_adhoc_source_uses_adhoc_policy(self):
        self.assertEqual(self.policy('codesign --force --sign - "$APP"\n'),
                         {"signing": "ad-hoc-not-notarized"})
        for text in ['# codesign --sign - app\n', 'codesign --sign "Some Certificate" app\n',
                     'SIGNING_IDENTITY="Some Certificate"\ncodesign --sign - app\n']:
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.policy(text)

    def test_certificate_signature_cannot_be_replaced_by_adhoc_or_another_team(self):
        policy = macos_signing.load_policy()
        for signature in [b"Signature=adhoc\nTeamIdentifier=R3JK7R29AC\n",
                          b"Authority=Apple Distribution\nTeamIdentifier=OTHERTEAM1\n"]:
            with self.subTest(signature=signature), patch.object(macos_signing, "command") as execute, \
                    patch.object(macos_signing.subprocess, "run", return_value=SimpleNamespace(stderr=signature)):
                with self.assertRaisesRegex(ValueError, "certificate/team"):
                    macos_signing.verify_signature(Path("fixture.app"), policy)
                self.assertEqual(execute.call_args.args[-2], policy["requirement"])
                self.assertIn("--test-requirement", execute.call_args.args)

    def test_exact_certificate_requirement_is_checked_before_accepting_metadata(self):
        policy = macos_signing.load_policy()
        with patch.object(macos_signing, "command", side_effect=[b"", subprocess.CalledProcessError(1, "codesign")]), \
                patch.object(macos_signing.subprocess, "run") as display:
            with self.assertRaises(subprocess.CalledProcessError):
                macos_signing.verify_signature(Path("fixture.app"), policy)
            display.assert_not_called()

    def test_legacy_adhoc_verification_rejects_a_different_signature_kind(self):
        policy = {"signing": "ad-hoc-not-notarized"}
        with patch.object(macos_signing, "command"), patch.object(macos_signing.subprocess, "run") as display:
            display.return_value = SimpleNamespace(stderr=b"Signature=adhoc\n")
            self.assertEqual(macos_signing.verify_signature(Path("fixture.app"), policy), policy)
            display.return_value = SimpleNamespace(stderr=b"Authority=Apple Distribution\n")
            with self.assertRaisesRegex(ValueError, "ad-hoc"):
                macos_signing.verify_signature(Path("fixture.app"), policy)

    def test_key_access_failure_cleans_probe_and_never_exports_a_private_key(self):
        for error in [subprocess.CalledProcessError(1, "codesign"), subprocess.TimeoutExpired("codesign", 120)]:
            with self.subTest(error=type(error).__name__):
                calls = []

                def execute(*args):
                    calls.append(args)
                    if args[0] == "/usr/bin/xcrun":
                        Path(args[-1]).write_bytes(b"fixture executable")
                        return b""
                    raise error

                with patch.object(signing_check, "command", side_effect=execute):
                    with self.assertRaisesRegex(ValueError, "signing probe failed"):
                        signing_check.verify()
                self.assertFalse(Path(calls[0][-1]).parent.exists())
                self.assertEqual([args[0] for args in calls], ["/usr/bin/xcrun", "/usr/bin/codesign"])


@unittest.skipUnless(sys.platform == "darwin" and os.environ.get("AEGIS_NATIVE_SIGNING_REGRESSION") == "true",
                     "real identity access is reserved for the protected macOS packaging job")
class NativeSigningTests(unittest.TestCase):
    def test_real_source_identity_signs_a_macho_and_passes_its_exact_requirement(self):
        result = signing_check.verify()
        self.assertEqual(result["status"], "macos_signing_verified")
        self.assertEqual(result["certificateSha1"], macos_signing.load_policy()["certificateSha1"])
        self.assertFalse(result["privateKeyExported"])
        self.assertFalse(result["productBuilt"])
        self.assertFalse(result["releaseReady"])


if __name__ == "__main__":
    unittest.main()
