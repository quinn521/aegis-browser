import importlib.util
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

import macos_signing

spec = importlib.util.spec_from_file_location('temporary_signer', Path(__file__).resolve().parents[2] /
                                             'apps/browser/scripts/sign-temporary-app.py')
signer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signer)


class TemporaryPolicyTests(unittest.TestCase):
    def test_explicit_mode_leaves_default_certificate_requirement_intact(self):
        self.assertEqual(macos_signing.load_policy(mode='temporary-ad-hoc'),
                         {'signing': 'ad-hoc-not-notarized', 'temporary': True})
        self.assertIn('anchor apple generic', macos_signing.load_policy()['requirement'])
        with self.assertRaises(ValueError):
            macos_signing.load_policy(mode='auto')

    def test_external_symlink_is_rejected_before_any_signature(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            app = root / 'Test.app'
            app.mkdir()
            (app / 'escape').symlink_to(root)
            with self.assertRaisesRegex(ValueError, 'escapes'):
                signer.targets(app)


@unittest.skipUnless(sys.platform == 'darwin', 'requires actual macOS codesign')
class NativeTemporarySigningTests(unittest.TestCase):
    def make_bundle(self, bundle, executable):
        contents = bundle / 'Contents'
        (contents / 'MacOS').mkdir(parents=True)
        (contents / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleIdentifier': 'com.gcsa.aegis.fixture.' + executable.lower(),
            'CFBundleExecutable': executable, 'CFBundlePackageType': 'APPL',
            'CFBundleVersion': '1', 'CFBundleShortVersionString': '1.0'}))
        source = bundle.parent / (executable + '.c')
        source.write_text('int main(void) { return 0; }\n')
        subprocess.run(['/usr/bin/xcrun', 'clang', '-arch', 'arm64', str(source), '-o',
                        str(contents / 'MacOS' / executable)], check=True, capture_output=True)
        source.unlink()
        return contents / 'MacOS' / executable

    def test_real_nested_code_and_resources_are_sealed_and_tampering_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            app = root / 'Test.app'
            self.make_bundle(app, 'Main')
            framework = app / 'Contents/Frameworks/Test Framework.framework'
            version = framework / 'Versions/A'
            resources = version / 'Resources'
            resources.mkdir(parents=True)
            source = root / 'framework.c'
            source.write_text('int fixture(void) { return 1; }\n')
            subprocess.run(['/usr/bin/xcrun', 'clang', '-arch', 'arm64', '-dynamiclib', str(source),
                            '-o', str(version / 'Test Framework')], check=True, capture_output=True)
            (resources / 'Info.plist').write_bytes(plistlib.dumps({
                'CFBundleIdentifier': 'com.gcsa.aegis.fixture.framework', 'CFBundleExecutable': 'Test Framework',
                'CFBundlePackageType': 'FMWK', 'CFBundleVersion': '1'}))
            (framework / 'Versions/Current').symlink_to('A')
            (framework / 'Resources').symlink_to('Versions/Current/Resources')
            (framework / 'Test Framework').symlink_to('Versions/Current/Test Framework')
            helper = version / 'Helpers/Helper.app'
            helper.parent.mkdir()
            binary = self.make_bundle(helper, 'Helper')
            resources = app / 'Contents/Resources'
            resources.mkdir()
            resource = resources / 'fixture.txt'
            resource.write_text('original')
            result = signer.sign(app)
            self.assertTrue(result['temporary'])
            self.assertFalse(result['formalDistributionQualified'])
            self.assertEqual(result['targetsVerified'], 6)
            macos_signing.verify_signature(app, macos_signing.load_policy(mode='temporary-ad-hoc'))
            # The default certificate verifier must reject this actual ad-hoc app.
            with self.assertRaises(subprocess.CalledProcessError):
                macos_signing.verify_signature(app, macos_signing.load_policy())
            resource.write_text('changed')
            with self.assertRaises(subprocess.CalledProcessError):
                macos_signing.verify_signature(app, macos_signing.load_policy(mode='temporary-ad-hoc'))
            resource.write_text('original')
            binary.write_bytes(binary.read_bytes() + b'tamper')
            with self.assertRaises(subprocess.CalledProcessError):
                macos_signing.verify_signature(app, macos_signing.load_policy(mode='temporary-ad-hoc'))


if __name__ == '__main__':
    unittest.main()
