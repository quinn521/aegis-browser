"""Synthetic fixtures only: exercise the build hook, staging, and IPA byte boundary."""
import base64
from contextlib import redirect_stderr
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import private_alpha_inputs as alpha
import test_verify_ipa

RELEASE = Path(__file__).resolve().parent
HOOK = RELEASE.parents[1] / 'apps/ios/scripts/private_alpha_nodes.py'
HELPER = RELEASE / 'private_alpha_inputs.py'
NODE = 'AegisPrivateTestNode.vless'
QUOTA = 'AegisPrivateTestSubscription.json'
NODES = 'AegisAlphaTestNodes.json'
UID = 'aa000000-0000-4000-8000-000000000001'
REALITY_KEY = base64.urlsafe_b64encode(bytes(range(32))).decode().rstrip('=')
ANCHOR = (f'vless://{UID}@anchor.invalid:443?security=reality&sni=cert.invalid'
          f'&pbk={REALITY_KEY}&sid=')
LIST = [
    f'vless://{UID}@tcp.invalid:443?type=tcp&security=tls&flow=xtls-rprx-vision&sni=cert.invalid'
    '&fp=chrome&headerType=none&host=hint.invalid#样例1',
    f'vless://{UID}@ws.invalid:443?type=ws&security=tls&host=inside.invalid%3A8443&sni=cert.invalid'
    '&path=%2Fapp%3Fv%3D2&alpn=http%2F1.1&fp=safari#样例2',
    f'vless://{UID}@reality.invalid:443?type=tcp&security=reality&flow=xtls-rprx-vision'
    f'&serverName=cert.invalid&publicKey={REALITY_KEY}&shortId=abcd1234#样例3',
    f'vless://{UID}@[2001:db8::30]:8443?type=grpc&security=reality&sni=cert.invalid'
    f'&pbk={REALITY_KEY}&sid=aabb&serviceName=%2Fservice%2FTun&path=/service/Tun#样例4',
]
DESCRIPTOR = dict(subscriptionInfoURL='https://quota.invalid/fixture-token?format=info',
                  allowedTestClient='fixture-client', expectedQuotaBytes=100, trafficReset='daily')
ROUTE = dict(GITHUB_ACTIONS='true', GITHUB_EVENT_NAME='workflow_dispatch', GITHUB_REF='refs/heads/main',
             GITHUB_WORKFLOW_REF='gcsagroup/aegis-browser/.github/workflows/ios-delivery.yml@refs/heads/main',
             GITHUB_REPOSITORY='gcsagroup/aegis-browser', GITHUB_RUN_ID='456', GITHUB_RUN_ATTEMPT='1',
             RUNNER_OS='macOS', RUNNER_ENVIRONMENT='self-hosted', DELIVERY_MODE='simple', RELEASE_OPERATION='package-upload',
             RELEASE_PROFILE='alpha', RELEASE_SIGNING_MODE='manual', RELEASE_DESTINATION='testflight',
             AEGIS_IOS_ALPHA_NODES_ENABLED='true', RELEASE_VERSION='3.2', RELEASE_BUILD='70')


def encode(value):
    return base64.b64encode(value if isinstance(value, bytes) else json.dumps(value).encode()).decode()


def secrets(nodes=LIST, anchor=ANCHOR, descriptor=DESCRIPTOR):
    return dict(AEGIS_IOS_ALPHA_NODES_BASE64=encode(nodes),
                AEGIS_IOS_TEST_NODE_BASE64=encode(anchor.encode()),
                AEGIS_IOS_TEST_SUBSCRIPTION_BASE64=encode(descriptor))


class PrivateAlphaInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.output = self.root / 'output'
        self.env = dict(ROUTE, RUNNER_TEMP=str(self.root), GITHUB_OUTPUT=str(self.output))

    def cli(self, command, env):
        return subprocess.run([sys.executable, '-B', str(command[0]), *command[1:]],
                              env=env, capture_output=True, text=True)

    def hook(self, channel, source, output):
        return self.cli([HOOK], dict(AEGIS_ACCESS_RELEASE_CHANNEL=channel, SCRIPT_INPUT_FILE_1=str(source),
                                     SCRIPT_OUTPUT_FILE_0=str(output)))

    def stage(self, **overrides):
        result = self.cli([HELPER, 'stage'], dict(self.env, **(secrets() | overrides)))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(UID, result.stdout + result.stderr + self.output.read_text())
        return Path(self.output.read_text().strip().removeprefix('directory='))

    def test_hook_preserves_order_and_bytes_then_removes_stale_output(self):
        source = self.root / 'input'
        source.write_bytes(json.dumps(LIST, ensure_ascii=False).encode())
        source.chmod(0o600)
        target = self.root / NODES
        result = self.hook('alpha', source, target)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(target.read_bytes()), LIST)
        for channel in ('alpha', 'dev', 'beta', 'release'):
            target.write_text('stale')
            self.assertEqual(self.hook(channel, '/dev/null', target).returncode, 0)
            self.assertFalse(target.exists())
        target.write_text('stale')
        self.assertNotEqual(self.hook('beta', source, target).returncode, 0)
        self.assertFalse(target.exists())

    def test_hook_uses_utf8_limit_and_refuses_unsafe_input(self):
        source = self.root / 'input'
        target = self.root / NODES
        prefix = LIST[1].split('#', 1)[0] + '#'
        remaining = 8192 - len(prefix) - 2
        label = 'é' * (remaining // 2) + 'a' * (remaining % 2)
        edge = [prefix + label + f'{i:02d}' for i in range(16)]
        source.write_text(json.dumps(edge))
        source.chmod(0o600)
        self.assertEqual(self.hook('alpha', source, target).returncode, 0)
        self.assertEqual(json.loads(target.read_bytes()), edge)
        for nodes in ([], edge + [LIST[0]], [edge[0] + 'é'], [4], ['vless://no-host'],
                      [prefix + chr(92) + 'suffix'], [' https://invalid.example'], [prefix + chr(0)]):
            with self.subTest(case=type(nodes).__name__, count=len(nodes)):
                source.write_text(json.dumps(nodes))
                result = self.hook('alpha', source, target)
                self.assertIn('private Alpha', result.stderr)
                self.assertNotIn(UID, result.stderr + result.stdout)
                self.assertFalse(target.exists())
        source.write_text(json.dumps(LIST))
        alias = self.root / 'alias'
        alias.symlink_to(source)
        self.assertNotEqual(self.hook('alpha', alias, target).returncode, 0)
        source.chmod(0o644)
        self.assertNotEqual(self.hook('alpha', source, target).returncode, 0)
        self.assertFalse(target.exists())

    def test_hook_and_protected_stage_reject_ambiguous_routes(self):
        tcp, ws, reality, grpc = (item.split('#', 1)[0] for item in LIST)
        bad = {
            'duplicated resource entry': [LIST[0], LIST[0]],
            'unicode equivalent entry': [tcp + '#é', tcp + '#é'],
            'duplicated key': [tcp + '&type=tcp'],
            'percent encoded duplicate key': [tcp + '&%66low=xtls-rprx-vision'],
            'bare query key': [tcp + '&encryption'],
            'missing vision': [tcp.replace('&flow=xtls-rprx-vision', '')],
            'wrong header': [tcp.replace('headerType=none', 'headerType=http')],
            'unexpected query option': [tcp + '&allowInsecure=1'],
            'wrong fingerprint': [tcp.replace('fp=chrome', 'fp=unsafe-synthetic')],
            'transport/security mismatch': [ws.replace('security=tls', 'security=reality')],
            'ws must not have a flow': [ws + '&flow=xtls-rprx-vision'],
            'unsafe ws target': [ws.replace('path=%2Fapp%3Fv%3D2', 'path=%2Finside%3F%2565d%3D1')],
            'nested proxy share': [ws.replace('path=%2Fapp%3Fv%3D2', 'path=%2Fvless%3A%2F%2Fopaque')],
            'wrong ws ALPN': [ws.replace('alpn=http%2F1.1', 'alpn=h2')],
            'conflicting aliases': [reality + '&sni=cert.invalid'],
            'invalid reality key': [reality.replace(REALITY_KEY, 'synthetic-invalid')],
            'odd short ID': [reality.replace('shortId=abcd1234', 'shortId=abc')],
            'conflicting gRPC service': [grpc.replace('path=/service/Tun', 'path=/different/Tun')],
            'wrong gRPC service': [grpc.replace('/service/Tun', '/../Tun').replace('%2Fservice%2FTun', '%2F..%2FTun')],
            'unknown gRPC mode': [grpc + '&mode=multi'],
        }
        source = self.root / 'input'
        target = self.root / NODES
        source.touch(mode=0o600)
        for label, links in bad.items():
            with self.subTest(label=label):
                source.write_text(json.dumps(links))
                target.write_bytes(b'stale')
                result = self.hook('alpha', source, target)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(UID, result.stdout + result.stderr)
                self.assertFalse(target.exists())
        for label in ('duplicated resource entry', 'percent encoded duplicate key', 'conflicting gRPC service'):
            with self.subTest(stage=label):
                result = self.cli([HELPER, 'stage'], dict(self.env, **secrets(bad[label])))
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(UID, result.stdout + result.stderr)
                self.assertEqual(list(self.root.glob('aegis-private-alpha-*')), [])

    def test_canonical_resource_size_has_the_same_bound_as_the_app(self):
        prefix = LIST[0].split('#', 1)[0] + '#'
        def links_at(extra):
            links = [prefix + f'{i:02}' + 'a' * (8192 - len(prefix) - 2) for i in range(16)]
            links[0] = links[0][:-extra] + '"' * extra
            return links
        source = self.root / 'input'
        source.touch(mode=0o600)
        target = self.root / NODES
        source.write_text(json.dumps(links_at(379)))
        self.assertEqual(self.hook('alpha', source, target).returncode, 0)
        self.assertEqual(len(target.read_bytes()), 131500)
        source.write_text(json.dumps(links_at(380)))
        self.assertNotEqual(self.hook('alpha', source, target).returncode, 0)
        self.assertFalse(target.exists())

    def test_stage_and_cleanup_confine_private_data_to_runner_temp(self):
        root = self.stage()
        self.assertEqual(root.parent, self.root)
        for directory in (root, root / 'inputs'):
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
        inputs = root / 'inputs'
        for resource in (NODES, NODE, QUOTA):
            self.assertEqual(stat.S_IMODE((inputs / resource).stat().st_mode), 0o600)
        self.assertEqual(json.loads((inputs / NODES).read_bytes()), LIST)
        self.assertEqual((inputs / NODE).read_text(), ANCHOR)
        quota = json.loads((inputs / QUOTA).read_bytes())
        self.assertEqual(quota['allowedTestClient'], DESCRIPTOR['allowedTestClient'])
        self.assertEqual(quota['nodeSHA256'], hashlib.sha256(ANCHOR.encode()).hexdigest())
        keep = self.root / 'unrelated'
        keep.mkdir()
        result = self.cli([HELPER, 'cleanup'], dict(self.env, AEGIS_PRIVATE_ALPHA_DIRECTORY=str(keep)))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(keep.is_dir())
        cleanup = self.cli([HELPER, 'cleanup'], dict(self.env, AEGIS_PRIVATE_ALPHA_DIRECTORY=str(root)))
        self.assertEqual(cleanup.returncode, 0, cleanup.stderr)
        self.assertFalse(root.exists())

    def test_stage_fails_closed_without_leaving_private_bytes(self):
        cases = [{key: ''} for key in secrets()]
        cases += [dict(AEGIS_IOS_ALPHA_NODES_BASE64='not base64'),
                  secrets([LIST[0].replace(UID, 'bb000000-0000-4000-8000-000000000002')]),
                  secrets(anchor=LIST[1]),
                  secrets(descriptor=dict(DESCRIPTOR, nodeSHA256='0' * 64)),
                  secrets(descriptor=dict(DESCRIPTOR, expectedQuotaBytes=-1))]
        for bad in cases:
            with self.subTest(keys=sorted(bad)):
                result = self.cli([HELPER, 'stage'], dict(self.env, **(secrets() | bad)))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Private Alpha', result.stderr)
                self.assertNotIn(UID, result.stdout + result.stderr)
                self.assertEqual(list(self.root.glob('aegis-private-alpha-*')), [])
        result = self.cli([HELPER, 'stage'], dict(self.env | dict(RELEASE_PROFILE='beta'), **secrets()))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(self.root.glob('aegis-private-alpha-*')), [])

    def test_exact_ipa_rejects_missing_mismatch_copy_and_noninternal_audience(self):
        root = self.stage()
        directory = root / 'inputs'
        path = self.root / 'app.ipa'
        resources = {name: (directory / name).read_bytes() for name in (NODES, NODE, QUOTA)}
        for case in ('correct', 'missing-list', 'missing-anchor', 'missing-quota', 'changed-order',
                     'changed-quota', 'copy-in-extension', 'symlink', 'wrong-channel', 'not-internal'):
            with self.subTest(case=case):
                test_verify_ipa.VerifyIPATests().fixture(path, channel='beta' if case == 'wrong-channel' else 'alpha',
                                       internal_only=case != 'not-internal')
                with zipfile.ZipFile(path, 'a') as archive:
                    app = 'Payload/Aegis.app/'
                    for name, data in resources.items():
                        if (case, name) in (('missing-list', NODES), ('missing-anchor', NODE), ('missing-quota', QUOTA)):
                            continue
                        if name == NODES and case == 'changed-order':
                            data = json.dumps(LIST[::-1], ensure_ascii=False, separators=(',', ':')).encode()
                        if name == QUOTA and case == 'changed-quota':
                            data = data.replace(b'fixture-client', b'fixture-wrong!')
                        entry = app + name
                        if name == NODES and case == 'symlink':
                            entry = zipfile.ZipInfo(entry)
                            entry.create_system = 3
                            entry.external_attr = (stat.S_IFLNK | 0o600) << 16
                        archive.writestr(entry, data)
                    if case == 'copy-in-extension':
                        archive.writestr(app + 'PlugIns/Share.appex/' + NODES, resources[NODES])
                if case == 'correct':
                    self.assertTrue(test_verify_ipa.ipa.verify(path, '3.2', '70', 'alpha', private_alpha_directory=directory))
                else:
                    with self.assertRaises(ValueError):
                        test_verify_ipa.ipa.verify(path, '3.2', '70', 'alpha', private_alpha_directory=directory)

    def test_upload_refuses_unverified_or_mutated_package_before_transport(self):
        self.addCleanup(os.umask, os.umask(0o077))
        directory = self.stage()
        source = alpha.load('ios_action', RELEASE / 'ios-action.py')
        sha, tree = source.source()
        package = directory / 'package'
        package.mkdir(mode=0o700)
        path = package / 'app.ipa'
        test_verify_ipa.VerifyIPATests().fixture(path, channel='alpha', internal_only=True)
        with zipfile.ZipFile(path, 'a') as archive:
            for name in (NODES, NODE, QUOTA):
                archive.write(directory / 'inputs' / name, 'Payload/Aegis.app/' + name)
        raw = json.dumps(dict(schemaVersion=1, releaseReady=False, applicationId='com.gcsa.aegis',
                              repository=ROUTE['GITHUB_REPOSITORY'], platform='ios', format='ipa',
                              version='3.2', build='70', profile='alpha', source=dict(sha=sha, tree=tree, dirty=False),
                              details=dict(privateAlphaNodes=True, signingMode='manual', testFlightInternalTestingOnly=True),
                              assets=[dict(name=path.name, size=path.stat().st_size,
                                           sha256=hashlib.sha256(path.read_bytes()).hexdigest())])).encode()
        (package / 'ios-manifest.json').write_bytes(raw)
        digest = hashlib.sha256(raw).hexdigest()
        (package / 'ios-manifest.json.sha256').write_text(f'{digest}  ios-manifest.json\n')
        env = dict(self.env, PATH=os.defpath, GITHUB_SHA=sha, RELEASE_MANIFEST_SHA256=digest,
                   AEGIS_PRIVATE_ALPHA_DIRECTORY=str(directory), AEGIS_ASC_KEY_ID='synthetic',
                   AEGIS_ASC_ISSUER_ID='synthetic', AEGIS_ASC_KEY_BASE64='synthetic')
        # Only stub the two external operations. The workspace, manifest, IPA
        # verification and digest checks are the real implementations.
        load = alpha.load
        with (patch.dict(os.environ, env, clear=True), patch.object(sys, 'argv', [str(HELPER), 'upload']),
              patch.object(alpha, 'run_private') as upload,
              patch.object(source, 'check_new_upload_runs') as inventory,
              patch.object(alpha, 'load', side_effect=lambda name, path: source if name == 'ios_action' else load(name, path)),
              redirect_stderr(io.StringIO())):
            with self.assertRaises(SystemExit):
                alpha.main()
            upload.assert_not_called()
            inventory.assert_not_called()
            os.environ['AEGIS_PRIVATE_ALPHA_VERIFIED_MANIFEST_SHA256'] = digest
            with patch('sys.stdout', new=io.StringIO()):
                alpha.main()
            upload.assert_called_once_with('upload', directory)
            inventory.assert_called_once()
            upload.reset_mock()
            with path.open('ab') as output:
                output.write(b'changed after verification')
            with self.assertRaises(SystemExit):
                alpha.main()
            upload.assert_not_called()
