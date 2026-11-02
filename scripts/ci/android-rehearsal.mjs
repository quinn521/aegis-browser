import {githubApi} from './promotion-github.mjs';

export const ANDROID_REHEARSAL_BRANCH = 'codex/android-release-acceptance';
const WORKFLOW = '.github/workflows/android-release-validation.yml';
const UPSTREAM = 'gcsagroup/aegis-browser';
function requireValue(ok, message) { if (!ok) throw new Error(message); }

// Fork-only build admission; never changes the upstream main/quality rule.
export async function requireAndroidRehearsal(sha, {environment:e=process.env, api=githubApi}={}) {
  const repository=e.GITHUB_REPOSITORY, token=e.GH_TOKEN || e.GITHUB_TOKEN;
  const tag=e.RELEASE_TAG || '';
  const candidate=e.GITHUB_EVENT_NAME==='push' && e.RELEASE_DESTINATION==='github-draft' &&
    /^candidate\/android\/[A-Za-z0-9][A-Za-z0-9_.-]{0,95}$/u.test(tag) && !tag.includes('..') && e.GITHUB_REF===`refs/tags/${tag}`;
  requireValue(e.GITHUB_ACTIONS==='true' && token && e.RELEASE_PLATFORM==='android' &&
    /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/u.test(repository || '') && !repository.includes('..') && repository!==UPSTREAM &&
    /^[1-9][0-9]{0,15}$/u.test(e.GITHUB_REPOSITORY_ID || '') && e.GITHUB_REPOSITORY_ID!=='1348409663' &&
    e.AEGIS_RELEASE_ENABLED==='true' && e.AEGIS_RELEASE_REHEARSAL==='true' &&
    e.AEGIS_RELEASE_TEST_REPOSITORY_ID===e.GITHUB_REPOSITORY_ID && e.AEGIS_ANDROID_REHEARSAL_ENABLED==='true' &&
    /^[a-f0-9]{40}$/u.test(sha) && e.GITHUB_SHA===sha && e.AEGIS_ANDROID_REHEARSAL_SOURCE_SHA===sha &&
    ['artifacts','play-internal','github-draft'].includes(e.RELEASE_DESTINATION) &&
    (candidate || ['push','workflow_dispatch'].includes(e.GITHUB_EVENT_NAME) && e.GITHUB_REF===`refs/heads/${ANDROID_REHEARSAL_BRANCH}`),
    'Android fork rehearsal requires explicit repository, source pin, fixed ref and testing-only destination');
  const metadata=await api(repository,'',{token});
  requireValue(metadata?.full_name===repository && String(metadata.id)===e.GITHUB_REPOSITORY_ID && metadata.fork===true &&
    metadata.parent?.full_name===UPSTREAM && metadata.parent?.id===1348409663,'Android rehearsal fork identity changed');
  const assertRefs=async()=>{
    const branch=await api(repository,`/git/ref/heads/${ANDROID_REHEARSAL_BRANCH}`,{token});
    requireValue(branch?.object?.sha===sha,'Android rehearsal source branch changed');
    if (candidate) {
      const value=await api(repository,`/commits/${encodeURIComponent(tag)}`,{token});
      requireValue(value?.sha===sha,'Android rehearsal candidate tag changed');
    }
  };
  await assertRefs();
  const query=new URLSearchParams({event:'push',branch:ANDROID_REHEARSAL_BRANCH,head_sha:sha,per_page:'100'});
  const latest=async()=>{
    const listing=await api(repository,`/actions/workflows/android-release-validation.yml/runs?${query}`,{token});
    requireValue(Number.isInteger(listing?.total_count) && listing.total_count>0 && listing.total_count<=100 &&
      Array.isArray(listing.workflow_runs) && listing.workflow_runs.length===listing.total_count,'Incomplete Android validation inventory');
    return [...listing.workflow_runs].sort((a,b)=>Number(b.id)-Number(a.id))[0];
  };
  const selected=await latest();
  const matches=run=>Number.isSafeInteger(run?.id) && run.id>0 && Number.isSafeInteger(run.run_attempt) && run.run_attempt>0 &&
    run.path===WORKFLOW && run.event==='push' && run.head_branch===ANDROID_REHEARSAL_BRANCH && run.head_sha===sha &&
    run.repository?.id===metadata.id && run.repository?.full_name===repository &&
    run.head_repository?.id===metadata.id && run.head_repository?.full_name===repository;
  requireValue(matches(selected),'Android validation source differs');
  const run=await api(repository,`/actions/runs/${selected.id}`,{token});
  requireValue(matches(run) && run.id===selected.id && run.run_attempt===selected.run_attempt,'Android validation attempt changed');
  const jobs=await api(repository,`/actions/runs/${run.id}/attempts/${run.run_attempt}/jobs?per_page=100`,{token});
  requireValue(Number.isInteger(jobs?.total_count) && jobs.total_count<=100 && Array.isArray(jobs.jobs) &&
    jobs.jobs.length===jobs.total_count,'Incomplete Android validation jobs');
  for (const name of ['contracts','native-admission']) {
    const rows=jobs.jobs.filter(j=>j.name===name);
    requireValue(rows.length===1 && rows[0].run_id===run.id && rows[0].head_sha===sha && rows[0].run_attempt===run.run_attempt &&
      rows[0].status==='completed' && rows[0].conclusion==='success',`Android validation has not passed: ${name}`);
  }
  const final=await api(repository,`/actions/runs/${run.id}`,{token});
  requireValue(matches(final) && final.id===run.id && final.run_attempt===run.run_attempt,'Android validation attempt changed during admission');
  await assertRefs();
  const last=await latest();
  requireValue(matches(last) && last.id===run.id && last.run_attempt===run.run_attempt,'Android latest validation changed during admission');
  return {repository,sha,admission:'fork_android_rehearsal_only',sourceValidationRun:run.id,sourceValidationAttempt:run.run_attempt,releaseReady:false};
}
