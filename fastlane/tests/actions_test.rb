# frozen_string_literal: true
require_relative 'stores_test'
require_relative '../../scripts/release/action'
require_relative '../../scripts/release/with-play-key'

class ActionsTest < ReleaseFixture
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
