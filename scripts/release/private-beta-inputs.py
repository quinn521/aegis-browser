"""Stage protected Beta inputs and require their exact presence in the exported IPA."""
import argparse
import base64
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import plistlib
# Runs only this checked-in helper with the current Python interpreter and fixed argv.
import subprocess  # nosec B404
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

QUOTA_TIMEOUT_SECONDS = 20
MAX_SECRET_LENGTH = 48 * 1024
IOS_SCRIPTS = Path(__file__).resolve().parents[2] / 'apps/ios/scripts'
sys.path.insert(0, str(IOS_SCRIPTS))
import private_beta_nodes as catalogue


def run_live_quota(configuration):
    # A separate process bounds DNS, TLS, response headers and slow-drip bodies together.
    # Values travel only over stdin; child output and exception details remain private.
    try:
        subprocess.run(  # nosec B603
            [sys.executable, str(Path(__file__).resolve()), 'quota'],
            input=json.dumps(configuration).encode(), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=QUOTA_TIMEOUT_SECONDS, check=True, env={})
    except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
        raise ValueError('Live quota verification failed') from None


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('Duplicate quota field')
        value[key] = item
    return value


def invalid_constant(_value):
    raise ValueError('Nonstandard JSON constant')


def module(name):
    path = IOS_SCRIPTS / (name + '.py')
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def stage(directory, environment):
    if directory.is_symlink() or not directory.is_dir() or directory.stat().st_mode & 0o077:
        raise ValueError('Private directory required')
    def decode(key, maximum):
        value = environment.get(key, '')
        if not 0 < len(value) <= maximum:
            raise ValueError('Missing or oversized private input')
        return base64.b64decode(value, validate=True)

    if environment.get('AEGIS_IOS_BETA_CATALOGUE_BASE64'):
        bundle = json.loads(decode('AEGIS_IOS_BETA_CATALOGUE_BASE64', MAX_SECRET_LENGTH),
                            object_pairs_hook=unique_object, parse_constant=invalid_constant)
        if (not isinstance(bundle, dict) or bundle.keys() != {'anchor', 'subscription', 'routes'}
                or not isinstance(bundle['anchor'], str) or not isinstance(bundle['subscription'], dict)):
            raise ValueError('Invalid private Beta catalogue input')
        node = bundle['anchor'].encode('utf-8')
        subscription = json.dumps(bundle['subscription'], ensure_ascii=False).encode('utf-8')
        routes = catalogue.canonicalize(json.dumps(bundle['routes'], ensure_ascii=False).encode('utf-8'), bundle['anchor'])
        catalogue_input = directory / 'catalogue.json'
        with os.fdopen(os.open(catalogue_input, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as stream:
            stream.write(routes)
    else:
        node = decode('AEGIS_IOS_TEST_NODE_BASE64', 11000)
        subscription = decode('AEGIS_IOS_TEST_SUBSCRIPTION_BASE64', 11000)
    for filename, raw in (('node.vless', node), ('subscription.json', subscription)):
        if not raw or len(raw) > 8192:
            raise ValueError('Invalid private input size')
        with os.fdopen(os.open(directory / filename, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as stream:
            stream.write(raw)
    # Use the exact product resource validators before invoking Xcode.
    module('private_test_node').install('beta', str(directory / 'node.vless'), directory / 'expected-node')
    module('private_test_subscription').install('beta', str(directory / 'subscription.json'),
                                              str(directory / 'node.vless'), directory / 'expected-subscription')
    for filename in ('expected-node', 'expected-subscription'):
        (directory / filename).chmod(0o600)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError('Quota redirects are not allowed')


def check_live_quota(configuration):
    url = configuration['subscriptionInfoURL']
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != 'https':
        raise ValueError('Quota requires HTTPS')
    # Default HTTPSHandler verifies certificates; never follow a redirect with a private URL.
    opener = urllib.request.build_opener(NoRedirect())
    # Match AccessSubscriptionClient's request headers, including the product user agent.
    request = urllib.request.Request(url, headers={
        'Accept': 'application/json', 'User-Agent': 'Aegis-Alpha/1.0'})
    with opener.open(request, timeout=20) as response:
        if (response.status != 200 or response.geturl() != url
                or response.headers.get_content_type() != 'application/json'):
            raise ValueError('Invalid quota response')
        # Require explicit framing: http.client accepts a truncated chunk trailer as EOF.
        lengths = response.headers.get_all('Content-Length', [])
        if len(lengths) != 1 or response.headers.get('Transfer-Encoding') is not None:
            raise ValueError('Quota requires one explicit response length')
        value = lengths[0].strip()
        if not value.isascii() or not value.isdecimal() or len(value) > 5:
            raise ValueError('Invalid quota response length')
        length = int(value)
        if length > 65536:
            raise ValueError('Invalid quota response framing')
        raw = response.read(65537)
    if len(raw) != length:
        raise ValueError('Incomplete quota response')
    usage = json.loads(raw, object_pairs_hook=unique_object, parse_constant=invalid_constant)
    if not isinstance(usage, dict):
        raise ValueError('Invalid quota payload')
    maximum = (1 << 64) - 1
    for key in ('uploadByte', 'downloadByte', 'totalByte', 'expire'):
        if type(usage[key]) is not int or not 0 <= usage[key] <= maximum:
            raise ValueError('Invalid quota number')
    used = usage['uploadByte'] + usage['downloadByte']
    if (usage['sId'] != urllib.parse.unquote(parts.path.rsplit('/', 1)[-1])
            or usage['emails'] != [configuration['allowedTestClient']]
            or usage['totalByte'] != configuration['expectedQuotaBytes']
            or usage['enabled'] is not True or used >= usage['totalByte']
            or usage['expire'] > 253_402_300_799
            or (usage['expire'] != 0 and usage['expire'] <= time.time())):
        raise ValueError('Quota identity, availability or allowance mismatch')


def verify(directory, ipa, live_quota=False):
    root = 'Payload/Aegis.app/'
    with zipfile.ZipFile(ipa) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError('Duplicate IPA members')
        info = plistlib.loads(archive.read(root + 'Info.plist'))
        if info.get('AegisAccessReleaseChannel') != 'beta' or info.get('AegisReleaseProfile') != 'beta':
            raise ValueError('Expected Beta IPA')
        if info.get('TFInternalTestingOnly') is not True:
            raise ValueError('Private Beta requires an internal-only TestFlight IPA')
        for member, expected in (('AegisPrivateTestNode.vless', 'expected-node'),
                                 ('AegisPrivateTestSubscription.json', 'expected-subscription')):
            if archive.getinfo(root + member).file_size > 8192:
                raise ValueError('Oversized private resource')
            if archive.read(root + member) != (directory / expected).read_bytes():
                raise ValueError('Private Beta resource mismatch')
        expected_catalogue = directory / 'catalogue.json'
        routes = read_catalogue(archive, root, required=expected_catalogue.exists())
        if routes is not None and routes != expected_catalogue.read_bytes():
            raise ValueError('Private Beta catalogue resource mismatch')
        if live_quota:
            run_live_quota(json.loads(archive.read(root + 'AegisPrivateTestSubscription.json')))


def read_catalogue(archive, root, required):
    member = root + catalogue.RESOURCE
    matches = [name for name in archive.namelist()
               if name.replace('\\', '/').rstrip('/').rsplit('/', 1)[-1].casefold() == catalogue.RESOURCE.casefold()]
    if matches != ([member] if required else []):
        raise ValueError('Unexpected private Beta catalogue placement')
    if not required:
        return None
    if archive.getinfo(member).file_size > catalogue.MAX_RESOURCE_BYTES:
        raise ValueError('Oversized private Beta catalogue')
    return archive.read(member)


def verify_live_ipa(ipa, requires_catalogue=False):
    root = 'Payload/Aegis.app/'
    with zipfile.ZipFile(ipa) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError('Duplicate IPA members')
        info = plistlib.loads(archive.read(root + 'Info.plist'))
        if (info.get('AegisAccessReleaseChannel') != 'beta' or info.get('AegisReleaseProfile') != 'beta'
                or info.get('TFInternalTestingOnly') is not True):
            raise ValueError('Expected internal Beta IPA')
        resources = []
        for name in ('AegisPrivateTestNode.vless', 'AegisPrivateTestSubscription.json'):
            if archive.getinfo(root + name).file_size > 8192:
                raise ValueError('Oversized private resource')
            resources.append(archive.read(root + name))
        configuration = json.loads(resources[1])
        if configuration['nodeSHA256'] != hashlib.sha256(resources[0]).hexdigest():
            raise ValueError('Quota binding differs from the packaged node')
        routes = read_catalogue(archive, root, required=requires_catalogue)
        if routes is not None and catalogue.canonicalize(routes, resources[0].decode('utf-8')) != routes:
            raise ValueError('Invalid packaged private Beta catalogue')
    run_live_quota(configuration)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('stage', 'verify', 'quota', 'verify-live'))
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--ipa', type=Path)
    parser.add_argument('--live-quota', action='store_true')
    parser.add_argument('--require-catalogue', action='store_true')
    args = parser.parse_args()
    try:
        if args.operation in ('stage', 'verify') and args.directory is None:
            raise ValueError('Directory required')
        if args.operation == 'quota':
            raw = sys.stdin.buffer.read(8193)
            if len(raw) > 8192:
                raise ValueError('Oversized quota configuration')
            check_live_quota(json.loads(raw))
        elif args.operation == 'stage':
            stage(args.directory, os.environ)
        else:
            if args.ipa is None:
                raise ValueError('IPA required')
            if args.operation == 'verify-live':
                verify_live_ipa(args.ipa, requires_catalogue=args.require_catalogue)
            else:
                verify(args.directory, args.ipa, live_quota=args.live_quota)
            if args.live_quota or args.operation == 'verify-live':
                print('Private Beta IPA resources and live quota verified.')
    except (OSError, ValueError, KeyError, TypeError, RecursionError, zipfile.BadZipFile,
            urllib.error.URLError, http.client.HTTPException):
        sys.exit('Private Beta input validation failed; no private values are logged.')
