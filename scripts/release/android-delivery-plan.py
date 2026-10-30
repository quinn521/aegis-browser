#!/usr/bin/env python3
"""Four-field Actions form; local release CLI remains unchanged."""
import json
import os
from pathlib import Path
import re

OPERATIONS = {'package': '仅打包 / Package', 'upload': '上传 Play 草稿 / Upload',
              'status': '查询状态 / Status', 'full': '完整模式 / Full'}
PRODUCTS = {'auto': '自动 / Auto', 'alpha': 'Alpha · 仅内测 / Internal only',
            'beta': 'Beta · 内外测 / Internal and external', 'release': 'Release · 通用 / Universal'}
VERSION = r'[0-9]{1,8}(?:\.[0-9]{1,8}){3}'
NUMBER = r'[1-9][0-9]{0,15}'
DIGEST = r'[0-9a-f]{64}'
RECEIPT_FIELDS = ['profile', 'version', 'build', 'package_run', 'package_attempt', 'state_run', 'state_attempt', 'manifest_sha256']


def require(ok, message):
    if not ok:
        raise ValueError(message)


def canonical(value, labels):
    for key, label in labels.items():
        if value in {key, label}:
            return key
    raise ValueError('Unknown operation or product')


def decode(value):
    require(isinstance(value, str) and len(value) < 400, 'Invalid delivery receipt')
    parts = value.split(':')
    require(len(parts) == 9 and parts[0] == 'aegis-android-v1', 'Copy the complete successful Android delivery receipt')
    pins = dict(zip(RECEIPT_FIELDS, parts[1:]))
    require(pins['profile'] in {'alpha', 'beta', 'release'} and re.fullmatch(VERSION, pins['version']) and
            re.fullmatch(r'[1-9][0-9]{0,9}', pins['build']) and
            all(re.fullmatch(NUMBER, pins[k]) for k in ['package_run', 'package_attempt', 'state_run', 'state_attempt']) and
            re.fullmatch(DIGEST, pins['manifest_sha256']), 'Invalid delivery receipt identity')
    return pins


def encode(pins):
    value = ':'.join(['aegis-android-v1']+[str(pins[k]) for k in RECEIPT_FIELDS])
    decode(value)
    return value


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate Full option')
        result[key] = value
    return result


def normalize(inputs, native):
    require(isinstance(inputs, dict) and not inputs.keys()-{'operation', 'product', 'target', 'advanced'} and
            all(isinstance(v, str) and not any(ord(c) < 32 or ord(c) == 127 for c in v) for k, v in inputs.items() if k != 'advanced'), 'Invalid delivery inputs')
    raw = inputs.get('advanced') or '{}'
    require(isinstance(raw, str) and len(raw.encode()) <= 64000, 'Full options exceed bound')
    try:
        full = json.loads(raw, object_pairs_hook=unique, parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    except (ValueError, RecursionError) as error:
        raise ValueError('Invalid Full options JSON') from error
    require(isinstance(full, dict) and not full.keys()-{'operation', 'destination', 'track', 'tag', 'play_release_policy'} and
            all(isinstance(v, str) and len(v) < 256 and not any(ord(c) < 32 or ord(c) == 127 for c in v) for v in full.values()), 'Unknown or invalid Full option')
    operation = canonical(inputs.get('operation') or 'package', OPERATIONS)
    if operation == 'full':
        operation = full.get('operation')
        require(operation in {'package', 'upload', 'status', 'distribute', 'submit'}, 'Select an explicit Full operation')
    else:
        require(not full, 'Select Full mode to use advanced options')
    profile = canonical(inputs.get('product') or 'auto', PRODUCTS)
    result = dict(operation=operation, profile=profile, version=native[0], build=native[1], format='aab',
                  destination=full.get('destination', 'artifacts' if operation in {'package'} else 'play-internal'),
                  track=full.get('track', ''), tag=full.get('tag', ''), play_release_policy=full.get('play_release_policy', ''))
    target = inputs.get('target', '')
    if operation in {'package'}:
        if target:
            match = re.fullmatch('('+VERSION+r')\(([1-9][0-9]{0,9})\)', target)
            require(match is not None and match.groups() == tuple(native), 'Version(versionCode) must match committed Android source')
        result['profile'] = 'alpha' if profile == 'auto' else profile
        require(not result['track'] and not result['play_release_policy'], 'Package accepts no Play stage settings')
        require(result['destination'] == 'artifacts', 'Package is artifacts only; use the existing candidate tag workflow for Tag to Draft')
    else:
        pins = decode(target)
        require(profile == 'auto' or profile == pins['profile'], 'Product differs from receipt')
        require((pins['version'], pins['build']) == tuple(native), 'Receipt differs from committed Android identity')
        result.update(pins)
    require(result['destination'] in {'artifacts', 'github-draft', 'play-internal', 'play-closed', 'play-open', 'play-production'}, 'Unknown Android destination')
    require(result['profile'] != 'alpha' or result['destination'] in {'artifacts', 'github-draft', 'play-internal'}, 'Alpha is internal testing only')
    require(result['profile'] == 'release' or result['destination'] != 'play-production', 'Production requires Release')
    if result['destination'] == 'github-draft':
        require(result['operation'] in {'package', 'upload'} and re.fullmatch(r'candidate/android/[A-Za-z0-9][A-Za-z0-9_.-]{0,95}', result['tag']) and '..' not in result['tag'], 'Draft requires an existing Android candidate tag')
    else:
        require(not result['tag'], 'Tag is only for Draft')
    if result['track']:
        require(result['destination'] == 'play-closed' and re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9_.-]{0,99}', result['track']) and
                result['track'] not in {'internal', 'beta', 'production'}, 'Track override requires a closed testing track')
    require(result['operation'] != 'distribute' or result['destination'] in {'play-internal', 'play-closed', 'play-open'}, 'Distribution requires a testing destination')
    require(result['operation'] != 'submit' or result['destination'] == 'play-production' and result['play_release_policy'] in {'managed', 'automatic'}, 'Submit requires explicit production publishing policy')
    require(not result['play_release_policy'] or result['operation'] == 'submit', 'Publishing policy is only for submit')
    require(result['operation'] in {'package'} or result['destination'] not in {'artifacts'}, 'Existing-state operation requires a destination')
    require(result['operation'] != 'status' or result['destination'].startswith('play-'), 'Status requires Play')
    return result


def native_identity(root):
    version = re.search(r'kProductVersion\[\] = "([0-9.]+)"', (root/'apps/browser/overlay/chrome/browser/ui/webui/help/aegis_github_update.h').read_text()).group(1)
    build = re.search(r'android_override_version_code = "([0-9]+)"', (root/'apps/browser/args/aegis-android.gn').read_text()).group(1)
    return version, build


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[2]
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    require(os.environ['GITHUB_EVENT_NAME'] == 'workflow_dispatch', 'Delivery is a manual entrypoint; tags use the existing candidate workflow')
    values = normalize(event.get('inputs') or {}, native_identity(root))
    with open(os.environ['GITHUB_OUTPUT'], 'a') as output:
        output.write('plan='+json.dumps(values, separators=(',', ':'))+'\n')
