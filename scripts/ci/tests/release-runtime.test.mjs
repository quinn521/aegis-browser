import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

const cwd = fileURLToPath(new URL('../../../', import.meta.url));
const options = {cwd, encoding: 'utf8', timeout: 180000,
  env: {...process.env, BUNDLE_FROZEN: 'true', AEGIS_NATIVE_SIGNING_REGRESSION: 'false', PYTHONDONTWRITEBYTECODE: '1'}};

test('local release request, artifact and draft-upload behavior', () => {
  execFileSync('bundle', ['exec', 'ruby', 'fastlane/tests/tag_test.rb'], options);
});

test('package identity and signature checks reject stale or unsafe fixtures', () => {
  execFileSync('python3', ['-m', 'unittest', 'discover', '-s', 'scripts/release', '-p', 'test_*.py'], options);
});
