#!/usr/bin/env python3
"""Read-only Android Draft acceptance: download every asset and hash its bytes."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import time
from urllib.parse import quote

spec = importlib.util.spec_from_file_location('android_delivery', Path(__file__).with_name('android-delivery.py'))
delivery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(delivery)
require = delivery.require


class GitHubReadOnly:
    def get(self, path, *, pages=False):
        args = ['gh', 'api', path, '--header', 'Cache-Control: no-cache']
        if pages:
            args += ['--paginate', '--slurp']
        result = subprocess.run(args, capture_output=True, text=True, timeout=120)
        require(result.returncode == 0, 'Draft GET failed; retain upload receipt and retry only verification')
        value = json.loads(result.stdout)
        if pages:
            require(isinstance(value, list) and len(value) <= 100 and all(isinstance(page, list) and len(page) <= 100 for page in value), 'Incomplete Draft inventory')
            return [row for page in value for row in page]
        return value

    def download(self, path, expected_size):
        child = subprocess.Popen(['gh', 'api', path, '--header', 'Accept: application/octet-stream'],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        sha, size = hashlib.sha256(), 0
        deadline = time.monotonic()+900
        try:
            while True:
                remaining = deadline-time.monotonic()
                require(remaining > 0, 'Draft download timed out; do not re-upload')
                if not select.select([child.stdout], [], [], min(1, remaining))[0]:
                    continue
                chunk = os.read(child.stdout.fileno(), 1024 * 1024)
                if not chunk:
                    break
                size += len(chunk)
                require(size <= expected_size, 'Downloaded Draft asset exceeds its manifest size')
                sha.update(chunk)
            require(child.wait(timeout=30) == 0, 'Draft download failed; do not re-upload')
            return size, sha.hexdigest()
        finally:
            child.stdout.close()
            if child.poll() is None:
                child.kill()
                child.wait()


def verify_downloads(folder, expected_digest, repository, tag, exact_source, client):
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository) and '..' not in repository, 'Invalid Draft repository')
    manifest_path = folder/'android-manifest.json'
    require(not manifest_path.is_symlink() and not (folder/'android-manifest.json.sha256').is_symlink(), 'Invalid manifest path')
    manifest = json.loads(manifest_path.read_text())
    pins = {key: manifest.get(key) for key in ['profile', 'version', 'build']}
    require(pins['profile'] in {'alpha', 'beta', 'release'} and
            isinstance(pins['version'], str) and re.fullmatch(delivery.codec.VERSION, pins['version']) and
            isinstance(pins['build'], str) and re.fullmatch(r'[1-9][0-9]{0,9}', pins['build']) and
            int(pins['build']) <= 2_100_000_000, 'Invalid Android Draft identity')
    require(tag == f'candidate/android/{pins["version"]}-{pins["profile"]}.{pins["build"]}', 'Draft tag differs from package identity')
    delivery.verify_package(folder, pins, expected_digest, exact_source)
    expected = list(manifest['assets']) + [dict(name=p.name, size=p.stat().st_size, sha256=delivery.digest(p))
                                         for p in [manifest_path, folder/'android-manifest.json.sha256']]
    require(all(0 < asset['size'] < 2 * 1024**3 for asset in expected), 'Invalid Draft asset size')
    prefix = 'repos/'+repository+'/'
    tag_path = prefix+'commits/'+quote(tag, safe='')
    require(client.get(tag_path).get('sha') == exact_source['sha'], 'Candidate tag changed before download')
    releases = client.get(prefix+'releases?per_page=100', pages=True)
    matches = [row for row in releases if row.get('tag_name') == tag]
    require(len(matches) == 1, 'Expected exactly one candidate Draft')
    release = matches[0]
    require(isinstance(release.get('id'), int) and release['id'] > 0, 'Invalid Draft ID')
    release_path = prefix+f'releases/{release["id"]}'

    def refresh():
        value = client.get(release_path)
        require(value.get('id') == release['id'] and value.get('tag_name') == tag and
                value.get('draft') is True and value.get('prerelease') is True and
                value.get('target_commitish') == exact_source['sha'], 'Draft/source/privacy changed')

    def inventory():
        rows = client.get(release_path+'/assets?per_page=100')
        require(isinstance(rows, list) and len(rows) == len(expected), 'Draft asset inventory differs')
        names = [row.get('name') for row in rows]
        require(len(set(names)) == len(names) and set(names) == {a['name'] for a in expected}, 'Unexpected Draft asset names')
        for asset in expected:
            row = next(row for row in rows if row['name'] == asset['name'])
            require(isinstance(row.get('id'), int) and row['id'] > 0 and row.get('state') == 'uploaded' and
                    row.get('size') == asset['size'] and row.get('digest') == 'sha256:'+asset['sha256'], 'Draft metadata differs from signed package')
        require(len({row['id'] for row in rows}) == len(rows), 'Duplicate Draft asset ID')
        return {row['name']: row for row in rows}

    refresh()
    before = inventory()
    downloads = []
    for asset in expected:
        refresh()
        row = before[asset['name']]
        size, digest = client.download(prefix+f'releases/assets/{row["id"]}', asset['size'])
        require((size, digest) == (asset['size'], asset['sha256']), 'Downloaded Draft bytes differ from local package')
        downloads.append(dict(name=asset['name'], assetId=row['id'], size=size, sha256=digest))
    refresh()
    require(inventory() == before, 'Draft assets changed during download')
    require(client.get(tag_path).get('sha') == exact_source['sha'], 'Candidate tag changed during download')
    return dict(status='draft_downloads_verified', repository=repository, releaseId=release['id'], tag=tag,
                source=exact_source, manifestSha256=expected_digest, downloads=downloads,
                draft=True, prerelease=True, releaseReady=False, playUploadPerformed=False)


def main():
    delivery.run('node', delivery.ROOT/'scripts/ci/android-operator.mjs')
    require(delivery.source()['sha'] == os.environ['GITHUB_SHA'], 'Draft verifier source differs')
    if sys.argv[1:] == ['--delivery']:
        p = delivery.plan()
        require(p['operation'] == 'upload' and p['destination'] == 'github-draft', 'Expected Android Draft delivery plan')
        folder, digest, tag = delivery.directory(p), p['manifest_sha256'], p['tag']
    else:
        require(not sys.argv[1:] and os.environ['RELEASE_PLATFORM'] == 'android', 'Expected Android Tag verifier')
        run_id = os.environ['RELEASE_RUN_ID']
        require(re.fullmatch(r'gh-[1-9][0-9]*-[1-9][0-9]*', run_id), 'Invalid original package run')
        folder = delivery.ROOT/'.artifacts/release/android'/run_id
        digest, tag = os.environ['RELEASE_MANIFEST_SHA256'], os.environ['RELEASE_TAG']
    result = verify_downloads(folder, digest, os.environ['GITHUB_REPOSITORY'], tag, delivery.source(), GitHubReadOnly())
    receipt = folder/'android-draft-download-result.json'
    require(not receipt.exists(), 'Download verification receipt already exists')
    receipt.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    try:
        main()
    except Exception:
        print('Android Draft download verification failed; preserve the existing Draft and upload receipt. Retry GET verification only.', file=sys.stderr)
        sys.exit(1)
