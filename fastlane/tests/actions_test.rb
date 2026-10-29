# frozen_string_literal: true
require_relative 'stores_test'
require_relative '../../scripts/release/action'
require_relative '../../scripts/release/with-play-key'
require_relative '../../scripts/release/runner'

class ActionsTest < ReleaseFixture
  def local_environment
    { 'GITHUB_ACTIONS' => 'true', 'GITHUB_REPOSITORY' => 'fixture/aegis-browser',
      'GITHUB_REPOSITORY_ID' => '42', 'AEGIS_RELEASE_TEST_REPOSITORY_ID' => '42',
      'GITHUB_REF' => 'refs/heads/codex/fastlane-release-test', 'AEGIS_RELEASE_ENABLED' => 'true',
      'AEGIS_RELEASE_REHEARSAL' => 'true', 'AEGIS_IOS_LOCAL_REHEARSAL' => 'true',
      'RELEASE_PLATFORM' => 'ios', 'RELEASE_DESTINATION' => 'artifacts', 'RELEASE_OPERATION' => 'package',
      'RUNNER_ENVIRONMENT' => 'self-hosted', 'AEGIS_LOCAL_IOS_BUNDLE_PATH' => @root }
  end

  def test_local_ios_route_is_fork_only_and_does_not_change_hosted_default
    assert_equal 'macos-26', AegisRelease::ReleaseRunner.select('RELEASE_PLATFORM' => 'ios')
    assert_equal AegisRelease::ReleaseRunner::LOCAL_IOS_LABELS, AegisRelease::ReleaseRunner.select(local_environment)
    [{ 'GITHUB_REPOSITORY' => AegisRelease::REPOSITORY }, { 'GITHUB_REPOSITORY_ID' => '1348409663' },
     { 'AEGIS_RELEASE_TEST_REPOSITORY_ID' => '99' }, { 'AEGIS_RELEASE_REHEARSAL' => 'false' },
     { 'GITHUB_REF' => 'refs/heads/unreviewed' }, { 'GITHUB_ACTIONS' => 'false' },
     { 'RELEASE_DESTINATION' => 'app-store' }, { 'RELEASE_DESTINATION' => 'testflight' }].each do |change|
      assert_raises(AegisRelease::Error) { AegisRelease::ReleaseRunner.select(local_environment.merge(change)) }
    end
  end

  def test_local_ios_candidate_route_requires_matching_tag_and_draft_operation
    env = local_environment.merge('GITHUB_REF' => 'refs/tags/candidate/ios/0.8.0-alpha.1',
      'RELEASE_TAG' => 'candidate/ios/0.8.0-alpha.1', 'RELEASE_DESTINATION' => 'github-draft', 'RELEASE_OPERATION' => 'upload')
    assert_equal AegisRelease::ReleaseRunner::LOCAL_IOS_LABELS, AegisRelease::ReleaseRunner.select(env)
    [{ 'RELEASE_TAG' => 'candidate/ios/other' }, { 'RELEASE_TAG' => 'v0.8.0' },
     { 'RELEASE_OPERATION' => 'submit' }].each do |change|
      assert_raises(AegisRelease::Error) { AegisRelease::ReleaseRunner.select(env.merge(change)) }
    end
  end

  def test_local_signing_rejects_store_credentials_and_unsafe_cache_before_building
    validate = ->(env, os = 'darwin', cpu = 'arm64') {
      AegisRelease::ReleaseRunner.validate_local_ios(env, os: os, cpu: cpu)
    }
    assert_equal @root, validate.call(local_environment)
    %w[AEGIS_ASC_KEY_PATH AEGIS_PLAY_JSON_KEY_DATA MATCH_PASSWORD].each do |key|
      error = assert_raises(AegisRelease::Error) { validate.call(local_environment.merge(key => 'fixture-secret')) }
      refute_includes error.message, 'fixture-secret'
    end
    assert_raises(AegisRelease::Error) { validate.call(local_environment, 'linux') }
    assert_raises(AegisRelease::Error) { validate.call(local_environment, 'darwin', 'x86_64') }
    File.chmod(0o777, @root)
    assert_raises(AegisRelease::Error) { validate.call(local_environment) }
  ensure
    File.chmod(0o700, @root)
  end

  def test_native_release_routing_still_requires_ready_dedicated_runners
    assert_raises(AegisRelease::Error) { AegisRelease::ReleaseRunner.select('RELEASE_PLATFORM' => 'android') }
    assert_equal %w[self-hosted Linux X64 aegis-release],
      AegisRelease::ReleaseRunner.select('RELEASE_PLATFORM' => 'android', 'CHROMIUM_RUNNERS_READY' => 'true')
  end

  def test_android_arm64_route_requires_explicit_opt_in_and_ready_runner
    env = { 'RELEASE_PLATFORM' => 'android', 'ANDROID_RUNNERS_READY' => 'true',
            'AEGIS_ANDROID_RUNNER_ARCH' => 'ARM64', 'AEGIS_ANDROID_EXPERIMENTAL_ARM64' => 'true' }
    assert_equal %w[self-hosted Linux ARM64 aegis-release], AegisRelease::ReleaseRunner.select(env)
    [{ 'AEGIS_ANDROID_EXPERIMENTAL_ARM64' => '' }, { 'AEGIS_ANDROID_EXPERIMENTAL_ARM64' => '1' },
     { 'ANDROID_RUNNERS_READY' => 'false' }, { 'AEGIS_ANDROID_RUNNER_ARCH' => 'arbitrary-label' }].each do |change|
      assert_raises(AegisRelease::Error) { AegisRelease::ReleaseRunner.select(env.merge(change)) }
    end
  end

  def test_android_package_host_policy_rejects_non_linux_and_unapproved_arm64
    assert AegisRelease.android_host?(os: 'linux-gnu', cpu: 'x86_64', env: {})
    %w[aarch64 arm64].each do |cpu|
      refute AegisRelease.android_host?(os: 'linux-gnu', cpu: cpu, env: {})
      assert AegisRelease.android_host?(os: 'linux-gnu', cpu: cpu,
        env: { 'AEGIS_ANDROID_EXPERIMENTAL_ARM64' => 'true' })
    end
    %w[darwin mswin].each do |os|
      refute AegisRelease.android_host?(os: os, cpu: 'arm64',
        env: { 'AEGIS_ANDROID_EXPERIMENTAL_ARM64' => 'true' })
    end
    refute AegisRelease.android_host?(os: 'linux-gnu', cpu: 'riscv64',
      env: { 'AEGIS_ANDROID_EXPERIMENTAL_ARM64' => 'true' })
  end

  def test_macos_readiness_cannot_enable_the_deferred_android_runner
    assert_equal %w[self-hosted macOS ARM64 aegis-release],
      AegisRelease::ReleaseRunner.select('RELEASE_PLATFORM' => 'macos', 'MACOS_RUNNERS_READY' => 'true')
    assert_raises(AegisRelease::Error) do
      AegisRelease::ReleaseRunner.select('RELEASE_PLATFORM' => 'android', 'MACOS_RUNNERS_READY' => 'true')
    end
    %w[macos android].each do |platform|
      assert_raises(AegisRelease::Error) do
        AegisRelease::ReleaseRunner.select('RELEASE_PLATFORM' => platform, 'CHROMIUM_RUNNERS_READY' => 'true',
          "#{platform.upcase}_RUNNERS_READY" => 'false')
      end
    end
  end

  def test_macos_runtime_rejects_store_credentials_wrong_host_and_shared_writable_paths
    env = { 'GITHUB_ACTIONS' => 'true', 'RELEASE_PLATFORM' => 'macos', 'MACOS_RUNNERS_READY' => 'true',
            'RUNNER_ENVIRONMENT' => 'self-hosted' }
    keys = %w[AEGIS_LOCAL_MACOS_BUNDLE_PATH CHROMIUM_ROOT DEPOT_TOOLS_DIR CIPD_CACHE_DIR VPYTHON_VIRTUALENV_ROOT TMPDIR]
    keys.each { |key| env[key] = @root }
    validate = ->(value, os = 'darwin', cpu = 'arm64') {
      AegisRelease::ReleaseRunner.validate_local_macos(value, os: os, cpu: cpu)
    }
    assert_equal keys.to_h { |key| [key, @root] }, validate.call(env)
    %w[AEGIS_ASC_KEY_PATH AEGIS_PLAY_JSON_KEY_DATA MATCH_PASSWORD AEGIS_ANDROID_STORE_PASSWORD].each do |key|
      error = assert_raises(AegisRelease::Error) { validate.call(env.merge(key => 'private-fixture')) }
      refute_includes error.message, 'private-fixture'
    end
    assert_raises(AegisRelease::Error) { validate.call(env, 'linux') }
    assert_raises(AegisRelease::Error) { validate.call(env, 'darwin', 'x86_64') }
    assert_raises(AegisRelease::Error) { validate.call(env.merge('TMPDIR' => "#{@root}\ninjected=true")) }
    File.chmod(0o777, @root)
    assert_raises(AegisRelease::Error) { validate.call(env) }
  ensure
    File.chmod(0o700, @root)
  end

  def environment
    { 'RELEASE_PLATFORM' => 'ios', 'RELEASE_PROFILE' => 'alpha', 'RELEASE_DESTINATION' => 'testflight-internal',
      'RELEASE_OPERATION' => 'distribute', 'RELEASE_VERSION' => '0.8.0', 'RELEASE_BUILD' => '1',
      'RELEASE_RUN_ID' => 'job-package-1', 'RELEASE_STORE_OPTIONS' => '{"groups":"Team"}' }
  end

  def action(env = environment)
    AegisRelease::ActionRequest.new(env, root: @root)
  end

  def test_invalid_stage_is_rejected_before_signing_or_store_access
    assert_raises(AegisRelease::Error) { action(environment.merge('RELEASE_OPERATION' => 'submit')) }
    assert_raises(AegisRelease::Error) { action(environment.merge('RELEASE_STORE_OPTIONS' => '{}')) }
    assert_raises(AegisRelease::Error) { action(environment.merge('RELEASE_STORE_OPTIONS' => '{"repository":"elsewhere/repo"}')) }
    assert_raises(AegisRelease::Error) { action(environment.merge('RELEASE_STORE_OPTIONS' => '[]')) }
  end

  def test_actions_and_cli_use_the_same_immutable_identity
    value = action
    assert_equal request(profile: 'alpha', version: '0.8.0', build: '1').identity, value.request.identity
    assert_equal ['ios', 'distribute'], value.argv('finish').first(2)
    assert_includes value.argv('upload'), '--profile'
    assert_includes value.argv('upload'), 'alpha'
  end

  def test_actions_do_not_interpolate_group_values_into_shell_code
    group = 'QA;$(touch should-not-exist)'
    value = action(environment.merge('RELEASE_STORE_OPTIONS' => JSON.generate('groups' => group)))
    argv = value.argv('finish')
    assert_equal group, argv[argv.index('--groups') + 1]
    refute File.exist?(File.join(@root, 'should-not-exist'))
  end

  def test_actions_rerun_cannot_repeat_unknown_store_mutation
    value = action(environment.merge('GITHUB_ACTIONS' => 'true', 'GITHUB_RUN_ATTEMPT' => '2'))
    assert_raises(AegisRelease::Error) { value.argv('upload') }
    assert_raises(AegisRelease::Error) { value.argv('finish') }
    assert_equal ['ios', 'package'], value.argv('package').first(2)
  end

  def test_package_survives_a_real_cross_checkout_artifact_handoff
    req, digest = package(profile: 'beta', run_id: 'handoff')
    Dir.mktmpdir('aegis-receiver-') do |temporary|
      checkout = File.join(File.realpath(temporary), 'checkout')
      AegisRelease.command('git', 'clone', '-q', '--no-hardlinks', @root, checkout)
      received = AegisRelease::Request.new('ios', { profile: 'beta', run_id: 'handoff' }, root: checkout)
      FileUtils.mkdir_p(received.directory)
      Dir.glob(File.join(req.directory, '*')).each { |path| FileUtils.cp(path, received.directory) }
      assert_equal req.identity['profile'], AegisRelease::Receipt.verify(received, digest)['profile']
      File.write(File.join(received.directory, 'Aegis.ipa'), 'tampered handoff')
      assert_raises(AegisRelease::Error) { AegisRelease::Receipt.verify(received, digest) }
    end
  end

  def test_temporary_play_credential_is_private_and_removed_even_on_failure
    observed = nil
    data = JSON.generate(type: 'service_account', private_key: 'test-fixture-not-a-key')
    assert_raises(IOError) do
      AegisRelease.with_play_key('AEGIS_PLAY_JSON_KEY_DATA' => data, 'RUNNER_TEMP' => @root) do |child|
        observed = child.fetch('AEGIS_PLAY_JSON_KEY')
        assert_nil child['AEGIS_PLAY_JSON_KEY_DATA']
        assert_equal 0o600, File.stat(observed).mode & 0o777
        assert_equal data, File.read(observed)
        raise IOError, 'simulated child failure'
      end
    end
    refute File.exist?(observed)
    refute File.directory?(File.dirname(observed))
  end

  def test_invalid_play_credential_never_echoes_secret_fragments
    error = assert_raises(AegisRelease::Error) do
      AegisRelease.with_play_key('AEGIS_PLAY_JSON_KEY_DATA' => '{"private_key":SECRET-FRAGMENT') { flunk }
    end
    refute_includes error.full_message, 'SECRET-FRAGMENT'
    assert_nil error.cause
  end
end
