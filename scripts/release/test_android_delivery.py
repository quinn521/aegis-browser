"""Real file/ZIP and form boundary regressions; fixtures are never product AABs."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch
import zipfile


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name+'.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


plan = load('android-delivery-plan')
transport = load('android-delivery')
NATIVE = ('6.1.0.266', '2006000266')
PINS = dict(profile='alpha', version=NATIVE[0], build=NATIVE[1], package_run='11', package_attempt='2', state_run='11', state_attempt='2', manifest_sha256='a'*64)


class FormTest(unittest.TestCase):
    def normalize(self, **inputs):
        return plan.normalize(inputs, NATIVE)

    def full(self, profile='alpha', **options):
        return self.normalize(operation='full', product=profile, target=plan.encode(dict(PINS, profile=profile)), advanced=json.dumps(options))

    def test_package_default_and_explicit_native_identity(self):
        default = self.normalize()
        self.assertEqual((default['profile'], default['format'], default['destination']), ('alpha', 'aab', 'artifacts'))
        self.assertEqual(default, self.normalize(operation=plan.OPERATIONS['package'], product=plan.PRODUCTS['auto'], target='6.1.0.266(2006000266)'))
        for target in ['1.0.0(33)', '6.1.0.266(1)', '../bad', '6.1.0.266(2006000266)\n']:
            with self.subTest(target=target), self.assertRaises(ValueError):
                self.normalize(target=target)

    def test_upload_status_copy_receipt_and_profile_pin(self):
        for op in ['upload', 'status']:
            p = self.normalize(operation=op, target=plan.encode(PINS))
            self.assertEqual(p['package_attempt'], '2')
            self.assertEqual(p['destination'], 'play-internal')
        with self.assertRaises(ValueError):
            self.normalize(operation='upload', product='beta', target=plan.encode(PINS))
        with self.assertRaises(ValueError):
            self.normalize(operation='upload', target='123')

    def test_beta_closed_open_and_alpha_limits(self):
        for dest in ['play-closed', 'play-open']:
            self.assertEqual(self.full('beta', operation='distribute', destination=dest)['destination'], dest)
            with self.assertRaises(ValueError):
                self.full(operation='distribute', destination=dest)
        self.full('release', operation='submit', destination='play-production', play_release_policy='managed')
        for profile, policy in [('beta', 'managed'), ('alpha', 'automatic'), ('release', '')]:
            with self.assertRaises(ValueError):
                self.full(profile, operation='submit', destination='play-production', play_release_policy=policy)

    def test_full_is_explicit_and_strict(self):
        for raw in ['[]', '{"operation":"status","operation":"upload"}', '{"operation":"status","secret":"x"}', '{"operation":NaN}', '{"operation":12}', '{"operation":"status","track":"x\\n"}']:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                self.normalize(operation='full', target=plan.encode(PINS), advanced=raw)
        with self.assertRaises(ValueError):
            self.normalize(operation='upload', target=plan.encode(PINS), advanced='{"operation":"status"}')
        with self.assertRaises(ValueError):
            self.normalize(operation='distribute', target=plan.encode(PINS))

    def test_tracks_and_draft(self):
        self.full('beta', operation='upload', destination='play-closed', track='qa')
        for track in ['production', 'internal', 'beta', '../qa']:
            with self.assertRaises(ValueError):
                self.full('beta', operation='upload', destination='play-closed', track=track)
        self.full(operation='upload', destination='github-draft', tag='candidate/android/6.1.0.266-alpha.2006000266')
        with self.assertRaises(ValueError):
            self.full(operation='upload', destination='github-draft', tag='candidate/ios/1.0.0-alpha.33')

    def test_receipt_roundtrip_and_malformed(self):
        self.assertEqual(plan.decode(plan.encode(PINS)), PINS)
        for change in [dict(package_attempt='0'), dict(state_run='../x'), dict(manifest_sha256='x'*64), dict(version='1.0.0'), dict(profile='dev')]:
            with self.assertRaises(ValueError):
                plan.encode(dict(PINS, **change))


class TransportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.folder = self.root/'package'
        self.folder.mkdir()
        self.source = dict(sha='1'*40, tree='2'*40, dirty=False)
        payload = self.folder/'Aegis.aab'
        payload.write_bytes(b'NON PRODUCT TRANSPORT FIXTURE')
        self.m = dict(schemaVersion=1, repository='gcsagroup/aegis-browser', source=self.source,
                      releaseReady=False, applicationId='com.gcsa.aegis', platform='android', format='aab',
                      profile='alpha', version=NATIVE[0], build=NATIVE[1],
                      details=dict(signing='android-upload-key', certificateSha256='b'*64),
                      assets=[dict(name=payload.name, size=payload.stat().st_size, sha256=transport.digest(payload))])
        self.write_manifest()

    def write_manifest(self):
        manifest = self.folder/'android-manifest.json'
        manifest.write_text(json.dumps(self.m))
        self.digest = transport.digest(manifest)
        (self.folder/'android-manifest.json.sha256').write_text(self.digest+'  android-manifest.json\n')

    def verify(self):
        return transport.verify_package(self.folder, PINS, self.digest, self.source)

    def test_bytes_and_original_manifest_binding(self):
        self.assertIn('Aegis.aab', self.verify())
        (self.folder/'Aegis.aab').write_bytes(b'changed')
        with self.assertRaises(ValueError):
            self.verify()

    def test_source_format_certificate_profile_and_single_aab(self):
        original = copy.deepcopy(self.m)
        for change in [dict(format='apk'), dict(profile='beta'), dict(source=dict(self.source, tree='3'*40)), dict(details=dict(signing='fixture')), dict(assets=[]), dict(assets=self.m['assets']*2)]:
            self.m = dict(original, **change)
            self.write_manifest()
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.verify()

    def test_stage_intent_digest_binding(self):
        record = dict(self.m, manifestSha256=self.digest)
        path = self.folder/'play-upload-started.json'
        path.write_text(json.dumps(record))
        self.assertIn(path.name, self.verify())
        record['manifestSha256'] = 'c'*64
        path.write_text(json.dumps(record))
        with self.assertRaises(ValueError):
            self.verify()

    def test_manifest_path_escape_and_symlink_rejected(self):
        self.m['assets'][0]['name'] = '../outside.aab'
        self.write_manifest()
        with self.assertRaises(ValueError):
            self.verify()
        self.m['assets'][0]['name'] = 'Aegis.aab'
        self.write_manifest()
        (self.folder/'Aegis.aab').unlink()
        (self.root/'outside').write_bytes(b'NON PRODUCT TRANSPORT FIXTURE')
        (self.folder/'Aegis.aab').symlink_to(self.root/'outside')
        with self.assertRaises(ValueError):
            self.verify()

    def test_real_zip_roundtrip_and_traversal(self):
        archive = self.root/'state.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            for path in self.folder.iterdir():
                z.write(path, path.name)
        dest = self.root/'restored'
        transport.extract_flat(archive, dest)
        transport.verify_package(dest, PINS, self.digest, self.source)
        with self.assertRaises(ValueError):
            transport.extract_flat(archive, dest)
        for member in ['../escape', 'sub/Aegis.aab', 'credential.json']:
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr(member, 'x')
            with self.subTest(member=member), self.assertRaises(ValueError):
                transport.extract_flat(archive, self.root/'bad')
            self.assertFalse((self.root/'bad').exists())

    def test_zip_symlink_rejected_before_writing(self):
        archive = self.root/'symlink.zip'
        member = zipfile.ZipInfo('Aegis.aab')
        member.create_system = 3
        member.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr(member, '/outside')
        with self.assertRaises(ValueError):
            transport.extract_flat(archive, self.root/'bad')

    def test_producer_latest_attempt_source_and_workflow(self):
        row = dict(id=11, run_attempt=2, head_sha=self.source['sha'], repository=dict(full_name='fixture/repo'),
                   head_repository=dict(full_name='fixture/repo'), path='.github/workflows/android-delivery.yml',
                   event='workflow_dispatch', status='completed', conclusion='success')
        transport.verify_run(row, '11', '2', self.source['sha'], 'fixture/repo')
        for change in [dict(run_attempt=3), dict(path='.github/workflows/ios-delivery.yml'), dict(conclusion='failure'), dict(head_sha='3'*40), dict(status='in_progress'), dict(head_repository=dict(full_name='other/repo'))]:
            with self.subTest(change=change), self.assertRaises(ValueError):
                transport.verify_run(dict(row, **change), '11', '2', self.source['sha'], 'fixture/repo')

    def test_export_preserves_original_digest_and_both_attempt_pins(self):
        env = dict(GITHUB_RUN_ID='11', GITHUB_RUN_ATTEMPT='2', GITHUB_REPOSITORY='fixture/repo', GITHUB_STEP_SUMMARY=str(self.root/'summary'))
        with patch.dict(transport.os.environ, env), patch.object(transport, 'ROOT', self.root), patch.object(transport, 'directory', return_value=self.folder), patch.object(transport, 'source', return_value=self.source):
            transport.export_state(dict(profile='alpha', version=NATIVE[0], build=NATIVE[1]), True)
        origin = json.loads((self.folder/'delivery-origin.json').read_text())
        self.assertEqual(origin['package_attempt'], '2')
        self.assertEqual(origin['state_attempt'], '2')
        self.assertEqual(origin['manifest_sha256'], self.digest)
        with zipfile.ZipFile(self.root/'.artifacts/android-delivery/package.zip') as z:
            self.assertEqual(z.read('Aegis.aab'), b'NON PRODUCT TRANSPORT FIXTURE')
        self.assertIn(plan.encode(dict(PINS, manifest_sha256=self.digest)), (self.root/'summary').read_text())

    def test_plan_revalidation_uses_committed_native_and_tamper_fails(self):
        p = plan.normalize(dict(operation='upload', target=plan.encode(PINS)), NATIVE)
        with patch.dict(transport.os.environ, ANDROID_DELIVERY_PLAN=json.dumps(p)):
            self.assertEqual(transport.plan(), p)
        for change in [dict(format='apk'), dict(operation='release'), dict(profile='dev'), dict(extra='x')]:
            with patch.dict(transport.os.environ, ANDROID_DELIVERY_PLAN=json.dumps(dict(p, **change))), self.assertRaises(ValueError):
                transport.plan()


if __name__ == '__main__':
    unittest.main()
