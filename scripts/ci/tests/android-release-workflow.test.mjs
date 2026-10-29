import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import YAML from 'yaml';

const root = new URL('../../../', import.meta.url);
const candidate = YAML.parse(readFileSync(new URL('.github/workflows/release-validation.yml', root), 'utf8'));

test('Android fork rehearsal needs exact-source uncredentialed validation before real AAB packaging', () => {
  const android = candidate.jobs['android-release-rehearsal'];
  assert.deepEqual(android.needs, ['contracts', 'ios-archive']);
  assert.match(android.if, /github\.repository != 'gcsagroup\/aegis-browser'/u);
  for (const flag of ['AEGIS_RELEASE_ENABLED', 'AEGIS_RELEASE_REHEARSAL', 'AEGIS_ANDROID_BRANCH_REHEARSAL']) {
    assert.ok(android.if.includes(`vars.${flag} == 'true'`));
  }
  assert.match(android.if, /github\.repository_id == vars\.AEGIS_RELEASE_TEST_REPOSITORY_ID/u);
  assert.equal(android.uses, './.github/workflows/release.yml');
  assert.deepEqual(android.with, {
    platform: 'android', profile: 'alpha', destination: 'artifacts',
    operation: 'package', format: 'aab', store_options: '{}',
  });
  assert.equal(android.secrets, 'inherit');
  for (const name of android.needs) {
    assert.doesNotMatch(JSON.stringify(candidate.jobs[name]), /secrets\.|self-hosted|contents.*write/u);
  }
});

test('Android branch rehearsal cannot dispatch another platform or consume its runner', () => {
  const prefix = "startsWith(github.ref, 'refs/heads/codex/fastlane-release-android-')";
  assert.ok(candidate.jobs['android-release-rehearsal'].if.includes(`&&\n    ${prefix}`));
  for (const name of ['release-rehearsal', 'macos-release-rehearsal']) {
    assert.ok(candidate.jobs[name].if.includes(`!${prefix}`));
  }
});
