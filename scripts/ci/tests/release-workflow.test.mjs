import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import YAML from 'yaml';

const root = new URL('../../../', import.meta.url);
const text = readFileSync(new URL('.github/workflows/release.yml', root), 'utf8');
const workflow = YAML.parse(text);

test('release is manual or reusable, exact-source and explicitly opt-in', () => {
  assert.deepEqual(Object.keys(workflow.on), ['workflow_dispatch', 'workflow_call']);
  assert.match(workflow.jobs.preflight.if, /github\.repository == 'gcsagroup\/aegis-browser'/u);
  assert.match(workflow.jobs.preflight.if, /github\.repository_id == '1348409663'/u);
  assert.match(workflow.jobs.preflight.if, /github\.ref == 'refs\/heads\/main'/u);
  assert.match(workflow.jobs.preflight.if, /AEGIS_RELEASE_ENABLED == 'true'/u);
  assert.equal(workflow.concurrency['cancel-in-progress'], false);
  for (const job of Object.values(workflow.jobs)) {
    for (const step of job.steps) {
      if (step.uses?.startsWith('actions/checkout@')) {
        assert.equal(step.with.ref, '${{ github.sha }}');
        assert.equal(step.with['persist-credentials'], false);
      }
      if (step.uses) assert.match(step.uses, /@[a-f0-9]{40}$/u);
    }
  }
});

test('credential and write permissions stay behind protected upload/build jobs', () => {
  assert.equal(workflow.permissions.contents, 'read');
  assert.equal(workflow.jobs.preflight.environment, undefined);
  assert.doesNotMatch(JSON.stringify(workflow.jobs.preflight), /secrets\./u);
  assert.equal(workflow.jobs.package.permissions.contents, 'read');
  assert.doesNotMatch(JSON.stringify(workflow.jobs.package), /AEGIS_ASC_KEY|contents.*write/u);
  assert.equal(workflow.jobs.testflight.environment, 'aegis-testflight');
  assert.equal(workflow.jobs.testflight.permissions.contents, 'read');
  assert.equal(workflow.jobs['github-draft'].environment, 'aegis-github-draft');
  assert.equal(workflow.jobs['github-draft'].permissions.contents, 'write');
  assert.equal(workflow.jobs.play.environment, 'aegis-play');
  assert.equal(workflow.jobs.play.permissions.contents, 'read');
  assert.equal(workflow.jobs['store-finish'].permissions.contents, 'read');
  assert.equal(workflow.jobs['store-finish'].environment, 'aegis-${{ inputs.platform }}-${{ inputs.operation }}');
});

test('uploads require package success and the trusted manifest digest output', () => {
  for (const name of ['testflight', 'play', 'github-draft']) {
    const job = workflow.jobs[name];
    assert.deepEqual(job.needs, ['preflight', 'package']);
    const upload = job.steps.find((step) => ['bash scripts/release/action.sh upload', 'ruby scripts/release/with-play-key.rb upload'].includes(step.run));
    assert.equal(upload.env.RELEASE_MANIFEST_SHA256, '${{ needs.package.outputs.manifest_sha256 }}');
    assert.equal(job.steps.find((s) => s.uses?.startsWith('actions/download-artifact@')).with['run-id'], undefined);
    assert.equal(job.env.RELEASE_RUN_ID, '${{ needs.package.outputs.run_id }}');
  }
  const artifact = workflow.jobs.package.steps.find((s) => s.uses?.startsWith('actions/upload-artifact@'));
  assert.equal(artifact.with['if-no-files-found'], 'error');
  assert.doesNotMatch(artifact.with.path, /\*\*|work|\.p12|\.p8|mobileprovision/u);
  const draftRecord = workflow.jobs['github-draft'].steps.find(s=>s.uses?.startsWith('actions/upload-artifact@'));
  assert.equal(draftRecord.if, '${{ always() }}');
  assert.match(draftRecord.with.path, /github-draft-create-\*\.json/u);
});

test('application IDs match the requested release identity', () => {
  const read = (path) => readFileSync(new URL(path, root), 'utf8');
  const project = read('apps/ios/project.yml');
  const generated = read('apps/ios/Aegis.xcodeproj/project.pbxproj');
  for (const id of ['com.gcsa.aegis', 'com.gcsa.aegis.safari', 'com.gcsa.aegis.share']) {
    assert.ok(project.includes(`PRODUCT_BUNDLE_IDENTIFIER: ${id}\n`));
    assert.ok(generated.includes(`PRODUCT_BUNDLE_IDENTIFIER = ${id};`));
  }
  assert.match(read('apps/browser/args/aegis-android.gn'), /chrome_public_manifest_package = "com\.gcsa\.aegis"/u);
  assert.match(read('apps/browser/overlay/chrome/app/theme/gcsa_aegis/BRANDING'), /^MAC_BUNDLE_ID=com\.gcsa\.aegis$/mu);
  assert.match(read('apps/browser/scripts/verify-android-agent-target.mjs'), /PACKAGE = 'com\.gcsa\.aegis'/u);
});

test('candidate branch checks stay uncredentialed before an explicitly opted-in fork rehearsal', () => {
  const candidate = YAML.parse(readFileSync(new URL('.github/workflows/release-validation.yml', root), 'utf8'));
  assert.deepEqual(candidate.on.push.branches, ['codex/fastlane-release-*']);
  assert.equal(candidate.permissions.contents, 'read');
  assert.equal(candidate.jobs['ios-archive'].needs, 'contracts');
  for (const name of ['contracts', 'ios-archive']) {
    const job = candidate.jobs[name];
    assert.doesNotMatch(JSON.stringify(job), /secrets\.|self-hosted|contents.*write|aegis-testflight/u);
    assert.equal(job.environment, undefined);
    for (const step of job.steps) {
      if (step.uses) assert.match(step.uses, /@[a-f0-9]{40}$/u);
      if (step.uses?.startsWith('actions/checkout@')) assert.equal(step.with.ref, '${{ github.sha }}');
    }
  }
  const archive = candidate.jobs['ios-archive'].steps.find((step) => step.run?.includes('xcodebuild'));
  assert.match(archive.run, /CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO/u);
  assert.match(archive.run, /MARKETING_VERSION=0\.8\.0 CURRENT_PROJECT_VERSION=1/u);
  const rehearsal = candidate.jobs['release-rehearsal'];
  assert.deepEqual(rehearsal.needs, ['contracts', 'ios-archive']);
  assert.match(rehearsal.if, /AEGIS_RELEASE_REHEARSAL == 'true'/u);
  assert.match(rehearsal.if, /AEGIS_RELEASE_TEST_REPOSITORY_ID/u);
  assert.equal(rehearsal.with.operation, 'package');
  assert.equal(rehearsal.with.destination, 'artifacts');
  assert.equal(rehearsal.uses, './.github/workflows/release.yml');
});

test('store review is a separate protected step after a completed upload record', () => {
  const finish = workflow.jobs['store-finish'];
  assert.deepEqual(finish.needs, ['preflight', 'package', 'testflight', 'play']);
  assert.match(finish.if, /inputs.operation == 'distribute' \|\| inputs.operation == 'submit'/u);
  assert.match(finish.if, /needs.testflight.result == 'success' \|\| needs.play.result == 'success'/u);
  const records = finish.steps.filter(s=>s.uses?.startsWith('actions/download-artifact@'));
  assert.equal(records.length, 2);
  assert.ok(records.some(s=>s.with.name.includes('store-upload-record')));
  assert.equal(workflow.on.workflow_dispatch.inputs.operation.default, 'package');
});

test('fork local signing is opt-in and remains downstream of the uncredentialed preflight', () => {
  assert.equal(workflow.jobs.package.needs, 'preflight');
  assert.equal(workflow.jobs.package.environment, 'aegis-release-${{ inputs.platform }}');
  assert.equal(workflow.env.AEGIS_IOS_LOCAL_REHEARSAL, '${{ vars.AEGIS_IOS_LOCAL_REHEARSAL }}');
  const route = workflow.jobs.preflight.steps.find(s=>s.id === 'route');
  assert.match(route.run, /ruby scripts\/release\/runner\.rb/u);
  const local = workflow.jobs.package.steps.find(s=>s.run === 'ruby scripts/release/local-ios-rehearsal.rb');
  assert.match(local.if, /runner.environment == 'self-hosted'/u);
  assert.doesNotMatch(JSON.stringify(local), /secrets\./u);
  const match = workflow.jobs.package.steps.find(s=>s.run === 'bundle exec fastlane ios ci_signing');
  assert.match(match.if, /runner.environment == 'github-hosted'/u);
});

test('candidate tags reuse the reviewed workflow and cannot trigger public releases', () => {
  const tag = YAML.parse(readFileSync(new URL('.github/workflows/release-candidate-tag.yml', root), 'utf8'));
  assert.deepEqual(tag.on.push.tags, ['candidate/ios/*', 'candidate/android/*', 'candidate/macos/*']);
  assert.equal(tag.permissions.contents, 'read');
  assert.doesNotMatch(JSON.stringify(tag.jobs.plan), /secrets/u);
  assert.equal(tag.jobs.release.uses, './.github/workflows/release.yml');
  assert.equal(tag.jobs.release.with.destination, 'github-draft');
  assert.equal(tag.jobs.release.with.operation, 'upload');
  assert.deepEqual(Object.keys(tag.jobs), ['plan', 'release']);
});

test('macOS has independent readiness and a protected native runtime without Android secrets', () => {
  const route = workflow.jobs.preflight.steps.find(s=>s.id === 'route');
  assert.equal(route.env.MACOS_RUNNERS_READY, '${{ vars.AEGIS_MACOS_RELEASE_RUNNERS_READY }}');
  assert.equal(route.env.ANDROID_RUNNERS_READY, '${{ vars.AEGIS_ANDROID_RELEASE_RUNNERS_READY }}');
  const local = workflow.jobs.package.steps.find(s=>s.run === 'ruby scripts/release/local-macos-release.rb');
  assert.match(local.if, /inputs.platform == 'macos'.*runner.environment == 'self-hosted'/u);
  assert.doesNotMatch(JSON.stringify(local), /secrets\./u);
  for (const step of workflow.jobs.package.steps.filter(s=>s.uses?.match(/setup-(node|ruby)@/u))) {
    assert.equal(step.if, "${{ inputs.platform == 'android' || runner.environment == 'github-hosted' }}");
  }
  const build = workflow.jobs.package.steps.find(s=>s.run === 'bash scripts/release/action.sh package');
  for (const name of ['AEGIS_ANDROID_STORE_PASSWORD', 'AEGIS_ANDROID_KEY_PASSWORD']) {
    assert.match(build.env[name], /inputs.platform == 'android' && secrets\./u);
  }
  const regression = workflow.jobs.package.steps.find(s=>s.run?.includes("-p 'test_verify_macos*.py'"));
  assert.equal(regression.if, "${{ inputs.platform == 'macos' }}");
  assert.equal(regression.env.AEGIS_NATIVE_SIGNING_REGRESSION, 'true');
  assert.match(readFileSync(new URL('scripts/release/local-macos-release.rb', root), 'utf8'), /verify-macos-toolchain\.py/u);
  assert.match(readFileSync(new URL('scripts/release/local-macos-release.rb', root), 'utf8'), /verify-macos-signing\.py/u);
  const candidate = YAML.parse(readFileSync(new URL('.github/workflows/release-validation.yml', root), 'utf8'));
  const mac = candidate.jobs['macos-release-rehearsal'];
  assert.deepEqual(mac.needs, ['contracts', 'ios-archive']);
  assert.equal(mac.with.platform, 'macos');
  assert.equal(mac.with.operation, 'package');
  assert.match(mac.if, /AEGIS_MACOS_BRANCH_REHEARSAL == 'true'/u);
  assert.match(candidate.jobs['release-rehearsal'].if, /AEGIS_IOS_BRANCH_REHEARSAL == 'true'/u);
});

test('normal PR quality installs locked release runtimes and keeps real signing in the protected job', () => {
  const quality = YAML.parse(readFileSync(new URL('.github/workflows/quality.yml', root), 'utf8')).jobs.quality;
  const ruby = quality.steps.find(s=>s.uses?.startsWith('ruby/setup-ruby@'));
  const java = quality.steps.find(s=>s.uses?.startsWith('actions/setup-java@'));
  assert.equal(ruby.with['ruby-version'], '3.3.6');
  assert.equal(ruby.with.bundler, '2.5.22');
  assert.equal(ruby.with['bundler-cache'], true);
  assert.equal(java.with['java-version'], '17');
  assert.equal(java.with.distribution, 'temurin');
  assert.ok(quality.steps.some(s=>s.run === 'bundle exec fastlane lanes'));
  assert.doesNotMatch(JSON.stringify(quality), /AEGIS_NATIVE_SIGNING_REGRESSION/u);
  assert.doesNotMatch(JSON.stringify([ruby, java]), /secrets\./u);
});
