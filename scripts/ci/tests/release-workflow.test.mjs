import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import YAML from 'yaml';

const root = new URL('../../../', import.meta.url);
const text = readFileSync(new URL('.github/workflows/release.yml', root), 'utf8');
const workflow = YAML.parse(text);

test('release is manual, upstream-only, exact-source and opt-in', () => {
  assert.deepEqual(Object.keys(workflow.on), ['workflow_dispatch']);
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
});

test('uploads require package success and the trusted manifest digest output', () => {
  for (const name of ['testflight', 'github-draft']) {
    const job = workflow.jobs[name];
    assert.deepEqual(job.needs, ['preflight', 'package']);
    const upload = job.steps.find((step) => step.run === 'bash scripts/release/action.sh upload');
    assert.equal(upload.env.RELEASE_MANIFEST_SHA256, '${{ needs.package.outputs.manifest_sha256 }}');
    assert.equal(job.steps.find((s) => s.uses?.startsWith('actions/download-artifact@')).with['run-id'], undefined);
  }
  const artifact = workflow.jobs.package.steps.find((s) => s.uses?.startsWith('actions/upload-artifact@'));
  assert.equal(artifact.with['if-no-files-found'], 'error');
  assert.doesNotMatch(artifact.with.path, /\*\*|work|\.p12|\.p8|mobileprovision/u);
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

test('candidate branch validation uses no secrets or upload permissions', () => {
  const candidate = YAML.parse(readFileSync(new URL('.github/workflows/release-validation.yml', root), 'utf8'));
  assert.deepEqual(candidate.on.push.branches, ['codex/fastlane-release-*']);
  assert.equal(candidate.permissions.contents, 'read');
  assert.equal(candidate.jobs['ios-archive'].needs, 'contracts');
  assert.doesNotMatch(JSON.stringify(candidate), /secrets\.|self-hosted|contents.*write|aegis-testflight/u);
  for (const job of Object.values(candidate.jobs)) {
    assert.equal(job.environment, undefined);
    for (const step of job.steps) {
      if (step.uses) assert.match(step.uses, /@[a-f0-9]{40}$/u);
      if (step.uses?.startsWith('actions/checkout@')) assert.equal(step.with.ref, '${{ github.sha }}');
    }
  }
  const archive = candidate.jobs['ios-archive'].steps.find((step) => step.run?.includes('xcodebuild'));
  assert.match(archive.run, /CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO/u);
  assert.match(archive.run, /MARKETING_VERSION=0\.8\.0 CURRENT_PROJECT_VERSION=1/u);
});
