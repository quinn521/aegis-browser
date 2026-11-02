# frozen_string_literal: true
require_relative 'release_test'
require_relative '../../scripts/release/action'

class AndroidActionAdmissionTest < ReleaseFixture
  def with_actions_source(sha)
    previous = %w[GITHUB_ACTIONS GITHUB_SHA].to_h { |key| [key, ENV[key]] }
    ENV['GITHUB_ACTIONS'] = 'true'
    ENV['GITHUB_SHA'] = sha
    yield
  ensure
    previous.each { |key, value| value ? ENV[key] = value : ENV.delete(key) }
  end

  def test_long_build_must_recheck_actions_source_before_touching_the_upload_key
    sha = AegisRelease.source(@root).fetch('sha')
    flow = AegisRelease::Pipeline.new(request('android'), Object.new)
    calls = []
    with_actions_source(sha) do
      flow.stub(:run, lambda { |*args|
        calls << args
        raise AegisRelease::Error, 'fixture source moved during build' if args[1].end_with?('release-preflight.mjs')
      }) do
        error = assert_raises(AegisRelease::Error) { flow.sign_android_bundle('unused-source', File.join(@root, 'never-signed.aab')) }
        assert_equal 'fixture source moved during build', error.message
      end
    end
    assert_equal [['node', File.join(@root, 'scripts/ci/android-operator.mjs')],
      ['node', File.join(@root, 'scripts/ci/release-preflight.mjs'), '--sha', sha]], calls
    refute File.exist?(File.join(@root, 'never-signed.aab'))
  end

  def test_changed_checkout_cannot_be_signed_under_a_stale_actions_sha
    flow = AegisRelease::Pipeline.new(request('android'), Object.new)
    with_actions_source('a' * 40) do
      flow.stub(:run, ->(*) { flunk 'changed checkout must fail before API or signer' }) do
        error = assert_raises(AegisRelease::Error) { flow.sign_android_bundle('unused-source', 'unused-target') }
        assert_match(/source.*SHA/i, error.message)
      end
    end
  end

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
