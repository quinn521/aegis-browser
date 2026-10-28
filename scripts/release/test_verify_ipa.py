import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("verify_ipa", Path(__file__).with_name("verify-ipa.py"))
ipa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ipa)


class VerifyIPATests(unittest.TestCase):
    def fixture(self, path, *, identifier="com.gcsa.aegis", extension_build="70", profile=True):
        with zipfile.ZipFile(path, "w") as archive:
            for suffix, app_id, build in [("", identifier, "70"),
                                          ("PlugIns/SafariWebExtension.appex/", "com.gcsa.aegis.safari", extension_build),
                                          ("PlugIns/ShareExtension.appex/", "com.gcsa.aegis.share", "70")]:
                root = "Payload/Aegis.app/" + suffix
                archive.writestr(root + "Info.plist", plistlib.dumps({"CFBundleIdentifier": app_id,
                                 "CFBundleShortVersionString": "3.2", "CFBundleVersion": build,
                                 "AegisReleaseProfile": "alpha"}))
                if profile:
                    archive.writestr(root + "embedded.mobileprovision", "fixture")

    def test_identity_and_extensions(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "test.ipa"
            self.fixture(path)
            self.assertTrue(ipa.verify(path, "3.2", "70"))
            self.assertTrue(ipa.verify(path, "3.2", "70", "alpha"))
            with self.assertRaises(ValueError):
                ipa.verify(path, "3.2", "70", "release")

    def test_stale_identity_extension_build_or_unsigned_package(self):
        for kwargs in ({"identifier": "com.gcsa.aegis.ios.app"}, {"extension_build": "69"}, {"profile": False}):
            with self.subTest(kwargs=kwargs), tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / "test.ipa"
                self.fixture(path, **kwargs)
                with self.assertRaises(ValueError):
                    ipa.verify(path, "3.2", "70")


if __name__ == "__main__":
    unittest.main()
