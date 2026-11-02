"""Remote metadata alone cannot satisfy Android Draft download acceptance."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('android_draft_verify', Path(__file__).with_name('android-draft-verify.py'))
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class DraftDownloadTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        self.source = dict(sha='1'*40, tree='2'*40, dirty=False)
        self.payload = b'NOT A PRODUCT AAB; DOWNLOAD TRUST-BOUNDARY FIXTURE'
        (self.folder/'Aegis.aab').write_bytes(self.payload)
        self.manifest = dict(schemaVersion=1, repository='gcsagroup/aegis-browser', source=self.source,
                             releaseReady=False, applicationId='com.gcsa.aegis', platform='android', format='aab',
                             profile='alpha', version='6.1.0.266', build='2006000266',
                             details=dict(signing='android-upload-key', certificateSha256='b'*64),
                             assets=[dict(name='Aegis.aab', size=len(self.payload), sha256=hashlib.sha256(self.payload).hexdigest())])
        path = self.folder/'android-manifest.json'
        path.write_text(json.dumps(self.manifest))
        self.digest = verify.delivery.digest(path)
        (self.folder/'android-manifest.json.sha256').write_text(self.digest+'  android-manifest.json\n')
        self.tag = 'candidate/android/6.1.0.266-alpha.2006000266'
        self.release = dict(id=7, tag_name=self.tag, draft=True, prerelease=True, target_commitish=self.source['sha'])
        self.assets = [dict(id=i+10, name=p.name, size=p.stat().st_size, digest='sha256:'+verify.delivery.digest(p), state='uploaded')
                       for i, p in enumerate(sorted(self.folder.iterdir()))]

    def client(self, *, release_change=None, corrupt=False, move_tag=False, extra=False, mutate_assets=False):
        outer = self
        class Client:
            def __init__(self):
                self.downloads, self.tag_reads, self.inventory_reads = [], 0, 0
            def get(self, path, **options):
                if '/commits/' in path:
                    self.tag_reads += 1
                    return dict(sha='f'*40 if move_tag and self.tag_reads > 1 else outer.source['sha'])
                if path.endswith('releases?per_page=100'):
                    return [dict(outer.release, **(release_change or {}))]
                if path.endswith('/assets?per_page=100'):
                    self.inventory_reads += 1
                    result = copy.deepcopy(outer.assets)
                    if extra:
                        result.append(dict(result[0], id=99, name='unexpected.aab'))
                    if mutate_assets and self.inventory_reads > 1:
                        result[0]['id'] = 99
                    return result
                return dict(outer.release, **(release_change or {}))
            def download(self, path, expected_size):
                self.downloads.append(path)
                asset = next(a for a in outer.assets if path.endswith('/'+str(a['id'])))
                data = (outer.folder/asset['name']).read_bytes()
                if corrupt and asset['name'].endswith('.aab'):
                    data = b'X'*len(data)
                return len(data), hashlib.sha256(data).hexdigest()
        return Client()

    def call(self, client):
        return verify.verify_downloads(self.folder, self.digest, 'fixture/aegis', self.tag, self.source, client)

    def test_every_signed_package_asset_manifest_and_checksum_is_downloaded(self):
        client = self.client()
        result = self.call(client)
        self.assertEqual(result['status'], 'draft_downloads_verified')
        self.assertEqual(len(client.downloads), 3)
        self.assertEqual({row['name'] for row in result['downloads']}, {row['name'] for row in self.assets})
        self.assertEqual(result['manifestSha256'], self.digest)
        self.assertTrue(result['draft'])
        self.assertFalse(result['playUploadPerformed'])
        self.assertFalse(result['releaseReady'])

    def test_correct_server_digest_cannot_hide_corrupted_download_bytes(self):
        with self.assertRaisesRegex(ValueError, 'Downloaded Draft bytes'):
            self.call(self.client(corrupt=True))

    def test_public_wrong_source_or_unexpected_assets_fail_before_download(self):
        for client in [self.client(release_change=dict(draft=False)), self.client(release_change=dict(prerelease=False)),
                       self.client(release_change=dict(target_commitish='f'*40)), self.client(extra=True)]:
            with self.assertRaises(ValueError):
                self.call(client)
            self.assertEqual(client.downloads, [])

    def test_tag_or_asset_id_race_after_download_is_not_acceptance(self):
        for client in [self.client(move_tag=True), self.client(mutate_assets=True)]:
            with self.assertRaises(ValueError):
                self.call(client)
            self.assertEqual(len(client.downloads), 3)


if __name__ == '__main__':
    unittest.main()
