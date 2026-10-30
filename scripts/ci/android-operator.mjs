import {pathToFileURL} from 'node:url';
import {githubApi} from './promotion-github.mjs';

function requireValue(ok, message) { if (!ok) throw new Error(message); }
function user(value) {
  return Number.isSafeInteger(value?.id) && value.id > 0 && /^[A-Za-z0-9[\]-]{1,100}$/u.test(value?.login || '');
}
function same(a, b) { return user(a) && user(b) && a.id === b.id && a.login.toLowerCase() === b.login.toLowerCase(); }
export async function requireAndroidAdministrator({environment: e = process.env, api = githubApi} = {}) {
  const repository = e.GITHUB_REPOSITORY;
  requireValue(/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u.test(repository || '') && !repository.includes('..'), 'Invalid repository');
  const token = e.GH_TOKEN || e.GITHUB_TOKEN;
  requireValue(token && e.GITHUB_ACTIONS === 'true', 'Actions Admin verification requires a token');
  const metadata = await api(repository, '', {token});
  requireValue(metadata.full_name === repository && String(metadata.id) === e.GITHUB_REPOSITORY_ID, 'Repository identity changed');
  requireValue(/^[1-9][0-9]{0,15}$/u.test(e.GITHUB_RUN_ID || '') && /^[1-9][0-9]{0,15}$/u.test(e.GITHUB_RUN_ATTEMPT || '') && /^[a-f0-9]{40}$/u.test(e.GITHUB_SHA || ''), 'Invalid runtime identity');
  const run = await api(repository, `/actions/runs/${e.GITHUB_RUN_ID}`, {token});
  requireValue(String(run.id) === e.GITHUB_RUN_ID && String(run.run_attempt) === e.GITHUB_RUN_ATTEMPT && run.head_sha === e.GITHUB_SHA &&
    run.repository?.id === metadata.id && run.repository?.full_name === repository && run.head_repository?.full_name === repository, 'Run/source/attempt changed');
  const delivery = run.path === '.github/workflows/android-delivery.yml';
  const candidate = run.path === '.github/workflows/release-candidate-tag.yml';
  const legacy = ['.github/workflows/release.yml', '.github/workflows/release-validation.yml'].includes(run.path);
  requireValue((delivery || legacy) && run.event === 'workflow_dispatch' || (candidate || legacy) && run.event === 'push', 'Unexpected Android workflow/event');
  requireValue(e.GITHUB_EVENT_NAME === run.event && e.GITHUB_REF === (run.event === 'push' && candidate ? 'refs/tags/' :
    e.GITHUB_REF?.startsWith('refs/tags/candidate/android/') ? 'refs/tags/' : 'refs/heads/')+run.head_branch &&
    (!candidate || e.GITHUB_REF.startsWith('refs/tags/candidate/android/')), 'Run ref/event changed');
  requireValue(user(run.actor) && user(run.triggering_actor) && run.actor.login === e.GITHUB_ACTOR &&
    String(run.actor.id) === e.GITHUB_ACTOR_ID && run.triggering_actor.login === e.GITHUB_TRIGGERING_ACTOR, 'Original actor/rerunner identity changed');
  // Read current Admin inventory, including inherited repository grants.
  const admins = [];
  let complete = false;
  for (let page = 1; page <= 20; page++) {
    const rows = await api(repository, `/collaborators?affiliation=all&permission=admin&per_page=100&page=${page}`, {token});
    requireValue(Array.isArray(rows) && rows.length <= 100, 'Incomplete Admin inventory');
    for (const row of rows) {
      requireValue(user(row) && row.permissions?.admin === true && !admins.some(a => a.id === row.id), 'Invalid Admin inventory');
      admins.push(row);
    }
    if (rows.length < 100) { complete = true; break; }
  }
  requireValue(complete && [run.actor, run.triggering_actor].every(principal => admins.some(a => same(a, principal))),
    'Original operator and rerunner require current repository Admin');
  return {authorized: true, repository, runId: run.id, attempt: run.run_attempt};
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try { console.log(JSON.stringify(await requireAndroidAdministrator())); }
  catch { console.error('Android repository Admin verification failed'); process.exitCode = 1; }
}
