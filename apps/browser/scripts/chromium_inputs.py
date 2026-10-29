"""Resolve committed Chromium inputs for a native platform without fetching."""
import argparse
import json
from pathlib import Path
import re


def load_inputs(browser, platform='desktop'):
    browser = Path(browser).resolve(strict=True)
    if platform == 'android':
        pin_path = browser / 'ci-pins/android.json'
        if not pin_path.is_file():
            raise ValueError('Android requires its committed ci-pins/android.json')
        pin = json.loads(pin_path.read_text())
        if not isinstance(pin, dict):
            raise ValueError('Invalid Android Chromium pin')
    elif platform == 'desktop':
        pin = {'version': (browser / 'CHROMIUM_VERSION').read_text().strip(),
               'commit': (browser / 'CHROMIUM_COMMIT').read_text().strip(),
               'patchDirectory': 'patches'}
    else:
        raise ValueError('Unsupported Chromium platform')
    if not isinstance(pin.get('version'), str) or not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', pin['version']):
        raise ValueError('Invalid Chromium version')
    if not isinstance(pin.get('commit'), str) or not re.fullmatch(r'[0-9a-f]{40}', pin['commit']):
        raise ValueError('Invalid Chromium commit')
    name = pin.get('patchDirectory')
    if not isinstance(name, str) or not name or '\\' in name or Path(name).is_absolute() or any(
            part in {'', '.', '..'} for part in name.split('/')):
        raise ValueError('Unsafe Chromium patch directory')
    patches = (browser / name).resolve()
    if patches == browser or not patches.is_relative_to(browser):
        raise ValueError('Chromium patch directory escapes apps/browser')
    return {'platform': platform, 'version': pin['version'], 'commit': pin['commit'],
            'patchDirectory': name}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['desktop', 'android'], default='desktop')
    parser.add_argument('--field', choices=['version', 'commit', 'patchDirectory'])
    args = parser.parse_args()
    inputs = load_inputs(Path(__file__).resolve().parents[1], args.platform)
    print(inputs[args.field] if args.field else json.dumps(inputs))
