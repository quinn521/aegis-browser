import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("unsigned_bundle", ROOT / "unsigned-bundle.py")
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleCopyTests(unittest.TestCase):
    def test_copy_removes_only_old_signature_and_preserves_payload(self):
        with tempfile.TemporaryDirectory() as temp:
            src, dst = Path(temp) / "source.aab", Path(temp) / "unsigned.aab"
            with zipfile.ZipFile(src, "w") as archive:
                for name in ("BundleConfig.pb", "base/dex/classes.dex", "META-INF/services/provider",
                             "META-INF/OLD.SF", "META-INF/OLD.RSA", "META-INF/MANIFEST.MF"):
                    archive.writestr(name, name.encode())
            bundle.copy_unsigned(src, dst)
            with zipfile.ZipFile(dst) as archive:
                self.assertEqual(set(archive.namelist()), {"BundleConfig.pb", "base/dex/classes.dex", "META-INF/services/provider"})
                self.assertEqual(archive.read("base/dex/classes.dex"), b"base/dex/classes.dex")
            with self.assertRaises(FileExistsError):
                bundle.copy_unsigned(src, dst)

    def test_bad_paths_and_non_bundles_are_rejected(self):
        for names in (["file"], ["BundleConfig.pb", "../escape"], ["BundleConfig.pb", "/escape"]):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as temp:
                src, dst = Path(temp) / "source.aab", Path(temp) / "unsigned.aab"
                with zipfile.ZipFile(src, "w") as archive:
                    for name in names:
                        archive.writestr(name, b"fixture")
                with self.assertRaises(ValueError):
                    bundle.copy_unsigned(src, dst)
                self.assertFalse(dst.exists())


@unittest.skipUnless(all(shutil.which(tool) for tool in ("java", "keytool", "jarsigner")), "JDK 17+ needed for real signing regression")
class BundleSignatureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.env = dict(os.environ, AEGIS_TEST_STORE_PASSWORD="temporary-test-password")
        cls.key = cls.root / "test.jks"
        cls.run_tool("keytool", "-genkeypair", "-alias", "fixture", "-keystore", str(cls.key),
                     "-storepass:env", "AEGIS_TEST_STORE_PASSWORD", "-keypass:env", "AEGIS_TEST_STORE_PASSWORD",
                     "-keyalg", "RSA", "-keysize", "2048", "-validity", "2", "-dname", "CN=Local Release Regression", "-noprompt")
        cert = cls.run_tool("keytool", "-exportcert", "-alias", "fixture", "-keystore", str(cls.key),
                            "-storepass:env", "AEGIS_TEST_STORE_PASSWORD").stdout
        cls.fingerprint = hashlib.sha256(cert).hexdigest()
        cls.signed = cls.root / "signed.aab"
        with zipfile.ZipFile(cls.signed, "w") as archive:
            archive.writestr("BundleConfig.pb", b"fixture config")
            archive.writestr("base/manifest/AndroidManifest.xml", b"fixture manifest")
        cls.run_tool("jarsigner", "-keystore", str(cls.key), "-storepass:env", "AEGIS_TEST_STORE_PASSWORD",
                     "-keypass:env", "AEGIS_TEST_STORE_PASSWORD", str(cls.signed), "fixture")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def run_tool(cls, *args, check=True):
        result = subprocess.run(args, capture_output=True, env=cls.env, check=False, timeout=40)
        if check and result.returncode:
            raise AssertionError(result.stdout.decode() + result.stderr.decode())
        return result

    def verify(self, path, fingerprint):
        return self.run_tool("java", str(ROOT / "VerifyAndroidBundle.java"), str(path), fingerprint, check=False)

    def test_real_signed_payload_is_verified(self):
        result = self.verify(self.signed, self.fingerprint)
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_different_certificate_or_unsigned_payload_is_rejected(self):
        self.assertNotEqual(self.verify(self.signed, "0" * 64).returncode, 0)
        extra = self.root / "extra.aab"
        shutil.copyfile(self.signed, extra)
        with zipfile.ZipFile(extra, "a") as archive:
            archive.writestr("base/extra-untrusted", b"not signed")
        self.assertNotEqual(self.verify(extra, self.fingerprint).returncode, 0)

    def test_tampered_signed_payload_is_rejected(self):
        tampered = self.root / "tampered.aab"
        with zipfile.ZipFile(self.signed) as src, zipfile.ZipFile(tampered, "w") as dst:
            for name in src.namelist():
                dst.writestr(name, b"tampered" if name == "BundleConfig.pb" else src.read(name))
        self.assertNotEqual(self.verify(tampered, self.fingerprint).returncode, 0)
