import {pathToFileURL} from 'node:url';
import {githubApi} from './promotion-github.mjs';
import {requireAndroidAdministrator} from './android-operator.mjs';

const REPOSITORY='quinn521/aegis-browser',ID=1363949860,BRANCH='codex/android-release-acceptance';
const WORKFLOW='.github/workflows/android-release-validation.yml';
function requireValue(ok,message) {if(!ok)throw new Error(message);}

// Unsigned capability check. It never admits a package or mutates GitHub state.
export async function qualifyAndroidReadToken({environment:e=process.env,api=githubApi}={}) {
  const token=e.GH_TOKEN||e.GITHUB_TOKEN,sha=e.GITHUB_SHA;
  requireValue(e.GITHUB_ACTIONS==='true' && token && e.GITHUB_REPOSITORY===REPOSITORY && e.GITHUB_REPOSITORY_ID===String(ID) &&
    e.GITHUB_JOB==='read-token-admission' && e.GITHUB_EVENT_NAME==='push' && e.GITHUB_REF===`refs/heads/${BRANCH}` &&
    /^[a-f0-9]{40}$/u.test(sha||'') && e.AEGIS_ANDROID_REHEARSAL_SOURCE_SHA===sha,'Invalid Android read-token runtime');
  const read=path=>api(REPOSITORY,path,{token,method:'GET'});
  const metadata=await read('');
  requireValue(metadata?.id===ID && metadata.full_name===REPOSITORY && metadata.private===false && metadata.fork===true &&
    metadata.parent?.id===1348409663 && metadata.parent?.full_name==='gcsagroup/aegis-browser','Read-token public fork identity changed');
  const admin=await requireAndroidAdministrator({environment:e,api});
  const matches=run=>run?.id===admin.runId && run.run_attempt===admin.attempt && run.head_sha===sha &&
    run.head_branch===BRANCH && run.event==='push' && run.path===WORKFLOW &&
    run.repository?.id===ID && run.repository?.full_name===REPOSITORY &&
    run.head_repository?.id===ID && run.head_repository?.full_name===REPOSITORY;
  const query=new URLSearchParams({event:'push',branch:BRANCH,head_sha:sha,per_page:'100'});
  const listing=await read(`/actions/workflows/android-release-validation.yml/runs?${query}`);
  requireValue(Number.isInteger(listing?.total_count) && listing.total_count>0 && listing.total_count<=100 &&
    Array.isArray(listing.workflow_runs) && listing.workflow_runs.length===listing.total_count &&
    listing.workflow_runs.some(matches),'Read-token workflow/source inventory unavailable');
  const jobs=await read(`/actions/runs/${admin.runId}/attempts/${admin.attempt}/jobs?per_page=100`);
  requireValue(Number.isInteger(jobs?.total_count) && jobs.total_count>0 && jobs.total_count<=100 &&
    Array.isArray(jobs.jobs) && jobs.jobs.length===jobs.total_count &&
    jobs.jobs.every(j=>j.run_id===admin.runId && j.run_attempt===admin.attempt && j.head_sha===sha) &&
    jobs.jobs.filter(j=>j.name==='read-token-admission').length===1,'Read-token job inventory unavailable');
  const assertRef=async()=>requireValue((await read(`/git/ref/heads/${BRANCH}`))?.object?.sha===sha,'Read-token source branch changed');
  await assertRef();
  requireValue((await read(`/commits/${sha}`))?.sha===sha,'Read-token source commit changed');
  requireValue(matches(await read(`/actions/runs/${admin.runId}`)),'Read-token run/attempt changed');
  await assertRef();
  return {status:'unsigned_read_token_endpoints_passed',repository:REPOSITORY,sourceSha:sha,runId:admin.runId,
    attempt:admin.attempt,releaseReady:false,productBuilt:false,productSigned:false,playUploadPerformed:false};
}

if(process.argv[1] && import.meta.url===pathToFileURL(process.argv[1]).href) {
  try {console.log(JSON.stringify(await qualifyAndroidReadToken()));}
  catch {console.error('Android read-token capability qualification failed');process.exitCode=1;}
}
