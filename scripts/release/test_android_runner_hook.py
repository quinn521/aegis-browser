import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('android_runner_hook', Path(__file__).with_name('android-runner-hook.py'))


class RunnerAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(Path(spec.origin).exists(), 'A host-pinned pre-checkout Android runner hook is required')
        self.hook = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.hook)
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        p = Path(self.tmp.name)/'event.json'
        p.write_text(json.dumps(dict(repository=dict(id=1363949860,full_name='quinn521/aegis-browser'))))
        self.config = dict(repository='quinn521/aegis-browser',repositoryId='1363949860',sourceSha='a'*40,
                           runId='12',attempt='1',actorId='77',actor='operator',job='native-admission',
                           workflow='android-release-validation.yml',ref='refs/heads/codex/android-release-acceptance',
                           ownerThreadId='01a19ada-b833-7bf0-9309-d860785b13fe')
        self.env = dict(GITHUB_REPOSITORY=self.config['repository'],GITHUB_REPOSITORY_ID=self.config['repositoryId'],
                        GITHUB_SHA=self.config['sourceSha'],GITHUB_RUN_ID='12',GITHUB_RUN_ATTEMPT='1',GITHUB_ACTOR_ID='77',
                        GITHUB_ACTOR='operator',GITHUB_TRIGGERING_ACTOR='operator',GITHUB_JOB='native-admission',
                        GITHUB_EVENT_NAME='push',GITHUB_EVENT_PATH=str(p),GITHUB_REF=self.config['ref'],
                        GITHUB_WORKFLOW_REF='quinn521/aegis-browser/.github/workflows/android-release-validation.yml@'+self.config['ref'])

    def test_exact_task_pinned_unsigned_native_job_is_admitted(self):
        self.hook.validate(self.env,self.config)

    def test_wrong_run_attempt_source_actor_workflow_ref_and_external_event_are_rejected(self):
        for k,v in dict(GITHUB_RUN_ID='13',GITHUB_RUN_ATTEMPT='2',GITHUB_SHA='b'*40,GITHUB_ACTOR_ID='88',
                        GITHUB_TRIGGERING_ACTOR='external',GITHUB_JOB='package',GITHUB_REPOSITORY='external/fork',
                        GITHUB_EVENT_NAME='pull_request',GITHUB_REF='refs/heads/main',GITHUB_WORKFLOW_REF='other/file@ref').items():
            with self.subTest(field=k), self.assertRaises(ValueError):
                self.hook.validate(dict(self.env,**{k:v}),self.config)

    def test_missing_config_or_event_repository_is_rejected(self):
        for key in self.config:
            config=dict(self.config); config.pop(key)
            with self.subTest(field=key), self.assertRaises(ValueError):
                self.hook.validate(self.env,config)
        Path(self.env['GITHUB_EVENT_PATH']).write_text(json.dumps(dict(repository=dict(id=888,full_name='external/fork'))))
        with self.assertRaises(ValueError): self.hook.validate(self.env,self.config)


if __name__ == '__main__':
    unittest.main()
