# frozen_string_literal: true
require_relative 'release_test'
require_relative '../../scripts/release/action'

class AndroidActionAdmissionTest < ReleaseFixture
  def test_android_candidate_draft_keeps_its_fork_without_enabling_shared_platform_flags
    sha = 'a' * 40
    env = { 'RELEASE_PROFILE' => 'alpha', 'RELEASE_VERSION' => '2.2.0.144', 'RELEASE_BUILD' => '2002000144',
      'RELEASE_RUN_ID' => 'android-fork-request', 'RELEASE_PLATFORM' => 'android', 'RELEASE_DESTINATION' => 'github-draft',
      'RELEASE_OPERATION' => 'upload', 'RELEASE_STORE_OPTIONS' => '{}', 'RELEASE_FORMAT' => 'aab',
      'RELEASE_TAG' => 'candidate/android/2.2.0.144-alpha.2002000144', 'GITHUB_ACTIONS' => 'true',
      'GITHUB_EVENT_NAME' => 'push',
      'GITHUB_REPOSITORY' => 'fixture/aegis-browser', 'GITHUB_REPOSITORY_ID' => '42',
      'AEGIS_RELEASE_TEST_REPOSITORY_ID' => '42', 'GITHUB_SHA' => sha,
      'GITHUB_REF' => 'refs/tags/candidate/android/2.2.0.144-alpha.2002000144',
      'AEGIS_ANDROID_REHEARSAL_SOURCE_SHA' => sha, 'AEGIS_ANDROID_RELEASE_ENABLED' => 'true',
      'AEGIS_ANDROID_REHEARSAL_ENABLED' => 'true', 'AEGIS_RELEASE_ENABLED' => 'false', 'AEGIS_RELEASE_REHEARSAL' => 'false' }
    value = AegisRelease::ActionRequest.new(env, root: @root)
    assert_equal 'fixture/aegis-browser', value.options[:repository]
    assert_includes value.argv('upload'), 'fixture/aegis-browser'
    %w[push workflow_dispatch].each do |event|
      branch = env.merge('GITHUB_EVENT_NAME' => event, 'GITHUB_REF' => 'refs/heads/codex/android-release-acceptance')
      assert_equal 'fixture/aegis-browser', AegisRelease::ActionRequest.new(branch, root: @root).request.repository
    end
    [{ 'AEGIS_ANDROID_RELEASE_ENABLED' => 'false' }, { 'AEGIS_ANDROID_REHEARSAL_SOURCE_SHA' => 'b' * 40 },
     { 'GITHUB_REPOSITORY_ID' => '99' }, { 'GITHUB_REF' => 'refs/heads/main' },
     { 'GITHUB_ACTIONS' => 'false' }, { 'GITHUB_EVENT_NAME' => 'pull_request' },
     { 'AEGIS_ANDROID_REHEARSAL_ENABLED' => 'false' },
     { 'AEGIS_ANDROID_RELEASE_ENABLED' => 'false', 'AEGIS_RELEASE_ENABLED' => 'true', 'AEGIS_RELEASE_REHEARSAL' => 'true' }].each do |change|
      refute_equal 'fixture/aegis-browser', AegisRelease::ActionRequest.new(env.merge(change), root: @root).options[:repository]
    end
  end

end
