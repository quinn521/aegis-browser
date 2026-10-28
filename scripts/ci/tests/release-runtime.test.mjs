import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

const cwd = fileURLToPath(new URL('../../../', import.meta.url));
const options = {cwd, encoding: 'utf8', timeout: 60000,
  env: {...process.env, PYTHONDONTWRITEBYTECODE: '1'}};

test('local release request, artifact and draft-upload behavior', () => {
  execFileSync('ruby', ['fastlane/tests/release_test.rb'], options);
});

test('IPA app and extension identity checks reject stale or unsigned fixtures', () => {
  execFileSync('python3', ['-m', 'unittest', 'discover', '-s', 'scripts/release', '-p', 'test_*.py'], options);
});
