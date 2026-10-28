# frozen_string_literal: true
require_relative 'release_test'
require 'ostruct'
require 'fastlane'
require 'supply'
require 'spaceship'

class StoresTest < ReleaseFixture
  def test_profiles_and_store_routes_are_independent
    %w[alpha beta release].each do |profile|
      assert_equal profile, request(profile: profile, destination: 'testflight-external').identity['profile']
      assert_equal profile, request('android', profile: profile, destination: 'play-open').profile
    end
    assert_equal 'beta', request(profile: 'Beta').profile
    assert_raises(AegisRelease::Error) { request(profile: 'beta', destination: 'app-store') }
    assert_raises(AegisRelease::Error) { request('android', profile: 'alpha', destination: 'play-production') }
    assert_raises(AegisRelease::Error) { request('android', destination: 'play-internal', format: 'apk') }
    assert_raises(AegisRelease::Error) { request('android', destination: 'play-closed', track: 'production') }
    assert_equal 'private-qa', request('android', destination: 'play-closed', track: 'private-qa').track
  end

  def test_profile_cannot_be_relabelled_after_packaging
    req, sha = package(profile: 'alpha', run_id: 'frozen')
    changed = request(profile: 'release', run_id: req.run_id)
    assert_raises(AegisRelease::Error) { AegisRelease::Receipt.verify(changed, sha) }
  end

  def test_journal_refuses_retry_after_unknown_network_outcome
    req, sha = package
    journal = AegisRelease::Journal.new(req, sha)
    count = 0
    assert_raises(IOError) { journal.perform('apple-upload') { count += 1; raise IOError, 'lost response' } }
    assert_raises(AegisRelease::Error) { journal.perform('apple-upload') { count += 1 } }
    assert_equal 1, count
    refute File.exist?(File.join(req.directory, 'apple-upload-result.json'))
  end

  def test_journal_success_is_idempotent_and_freezes_parameters
    req, sha = package
    journal = AegisRelease::Journal.new(req, sha)
    first = journal.perform('distribute', 'groups' => ['QA']) { { 'status' => 'assigned' } }
    assert_equal first, journal.perform('distribute', 'groups' => ['QA']) { flunk 'Repeated mutation' }
    assert_raises(AegisRelease::Error) { journal.perform('distribute', 'groups' => ['Public']) { flunk } }
  end

  def test_journal_rejects_concurrent_stage_and_corrupt_package
    req, sha, artifact = package
    lock = File.open(File.join(req.directory, '.store.lock'), 'w')
    lock.flock(File::LOCK_EX)
    assert_raises(AegisRelease::Error) { AegisRelease::Journal.new(req, sha).perform('upload') { flunk } }
    lock.close
    File.write(artifact, 'corrupt')
    assert_raises(AegisRelease::Error) { AegisRelease::Journal.new(req, sha).perform('upload') { flunk } }
  ensure
    lock&.close unless lock&.closed?
  end

  class FakeLane
    attr_reader :calls
    def initialize
      @calls = []
    end
    def upload_to_testflight(**options)
      @calls << options
    end
    def upload_to_app_store(**options)
      @calls << options
    end
  end

  def apple(destination, group_rows: [], **options)
    req = request(destination: destination, **options)
    lane = FakeLane.new
    client = AegisRelease::AppleStore.new(req, lane)
    app = Object.new
    app.define_singleton_method(:get_beta_groups) { group_rows }
    app.define_singleton_method(:id) { 'app-fixture' }
    client.instance_variable_set(:@app, app)
    client.instance_variable_set(:@api_key, {})
    [client, lane, app]
  end

  def test_apple_upload_only_never_distributes_or_submits
    client, lane = apple('testflight-external')
    client.stub(:build, nil) { assert_equal false, client.upload('fixture.ipa')['reviewSubmitted'] }
    assert_equal true, lane.calls.first[:skip_submission]
    assert_equal true, lane.calls.first[:skip_waiting_for_build_processing]
    assert_equal false, lane.calls.first[:distribute_external]
    client.stub(:build, Object.new) { assert_raises(AegisRelease::Error) { client.upload('fixture.ipa') } }
    assert_equal 1, lane.calls.size
  end

  def test_missing_or_wrong_type_testflight_groups_cannot_mutate
    internal = OpenStruct.new(name: 'Team', is_internal_group: true)
    client, lane = apple('testflight-external', groups: 'Team', group_rows: [internal])
    client.stub(:ready_build, OpenStruct.new(id: '1')) do
      assert_raises(AegisRelease::Error) { client.distribute }
    end
    assert_empty lane.calls
    client, lane = apple('testflight-internal', groups: 'Typo', group_rows: [internal])
    client.stub(:ready_build, OpenStruct.new(id: '1')) do
      assert_raises(AegisRelease::Error) { client.distribute }
    end
    assert_empty lane.calls
  end

  def test_testflight_uses_exact_version_build_and_confirms_group_assignment
    group = OpenStruct.new(name: 'Team', is_internal_group: true)
    group.define_singleton_method(:fetch_builds) { [OpenStruct.new(id: 'build1')] }
    client, lane = apple('testflight-internal', groups: 'Team', group_rows: [group], version: '0.8.0', build: '1')
    client.stub(:ready_build, OpenStruct.new(id: 'build1')) do
      client.stub(:status, { 'internalState' => 'IN_BETA_TESTING' }) do
        assert_equal 'testflight_groups_assigned', client.distribute['status']
      end
    end
    assert_equal '0.8.0', lane.calls.first[:app_version]
    assert_equal '1', lane.calls.first[:build_number]
    assert_equal false, lane.calls.first[:submit_beta_review]
    assert_equal true, lane.calls.first[:distribute_only]
  end

  def test_store_submission_is_a_separate_explicit_stage
    flow = AegisRelease::Pipeline.new(request(profile: 'alpha', destination: 'testflight'), FakeLane.new)
    assert_raises(AegisRelease::Error) { flow.submit }
    flow = AegisRelease::Pipeline.new(request('android', destination: 'play-production'), FakeLane.new)
    assert_raises(AegisRelease::Error) { flow.submit }
    assert_raises(AegisRelease::Error) { flow.distribute }
  end

  def test_distribute_cannot_skip_an_unknown_upload_outcome
    req, sha = package(profile: 'alpha', destination: 'testflight-internal', groups: 'Team')
    staged = request(profile: 'alpha', destination: 'testflight-internal', groups: 'Team',
                     run_id: req.run_id, manifest_sha256: sha)
    AegisRelease.write_new(File.join(req.directory, 'apple-upload-started.json'), 'outcome' => 'unknown')
    flow = AegisRelease::Pipeline.new(staged, FakeLane.new)
    assert_raises(AegisRelease::Error) { flow.distribute }
    refute File.exist?(File.join(req.directory, 'ios-distribute-started.json'))
  end

  def test_app_store_submission_sets_manual_release_even_with_metadata_skipped
    client, lane, app = apple('app-store')
    version = OpenStruct.new(version_string: '3.2', release_type: 'AFTER_APPROVAL')
    changes = []
    version.define_singleton_method(:update) { |**opts| changes << opts }
    app.define_singleton_method(:get_edit_app_store_version) { |**_| version }
    app.define_singleton_method(:get_in_progress_review_submission) { |**_| nil }
    client.stub(:ready_build, Object.new) do
      client.stub(:status, { 'appVersionState' => 'WAITING_FOR_REVIEW', 'releaseType' => 'MANUAL' }) do
        assert_equal 'app_review_submitted', client.submit['status']
      end
    end
    assert_equal [{ attributes: { releaseType: 'MANUAL' } }], changes
    assert_equal '69', lane.calls.first[:build_number]
    assert_equal true, lane.calls.first[:skip_binary_upload]
    assert_equal true, lane.calls.first[:submit_for_review]
    assert_equal false, lane.calls.first[:automatic_release]
  end

  class FakePlayClient
    attr_accessor :current_edit, :current_package_name, :rows, :codes, :commits, :writes, :upload_digest, :fail_commit
    def initialize
      @rows, @codes, @commits, @writes = [], [], [], []
    end
    def begin_edit(package_name:)
      raise 'wrong app' unless package_name == AegisRelease::APP_ID
      @current_edit = OpenStruct.new(id: 'edit1')
    end
    def abort_current_edit; @current_edit = nil; end
    def track_releases(_); @rows; end
    def aab_version_codes; @codes; end
    def apks_version_codes; []; end
    def upload_bundle(_); @codes << 2002000144; 2002000144; end
    def client; self; end
    def list_edit_bundles(*)
      OpenStruct.new(bundles: [OpenStruct.new(version_code: 2002000144, sha256: @upload_digest)])
    end
    def update_track(name, track)
      @writes << name
      @rows = track.releases
    end
    def validate_current_edit!; end
    def commit_edit(_app, _edit, **options)
      @commits << options
      raise AegisRelease::Error, 'Changes already in review' if fail_commit
    end
  end

  def play
    req = request('android', destination: 'play-internal')
    client = AegisRelease::PlayStore.new(req, FakeLane.new)
    fake = FakePlayClient.new
    client.instance_variable_set(:@client, fake)
    Supply.config = FastlaneCore::Configuration.create(Supply::Options.available_options, {})
    [client, fake]
  end

  def test_play_upload_then_distribute_without_reupload_or_review_fallback
    client, fake = play
    aab = write('.artifacts/fixture.aab', 'bundle')
    fake.upload_digest = AegisRelease.digest(aab)
    assert_equal 'play_draft_uploaded', client.upload(aab)['status']
    assert_equal [{ changes_not_sent_for_review: true, changes_in_review_behavior: 'ERROR_IF_IN_REVIEW' }], fake.commits
    assert_equal 'play_track_committed_review_requested', client.distribute['status']
    assert_equal [{ changes_not_sent_for_review: true, changes_in_review_behavior: 'ERROR_IF_IN_REVIEW' },
                  { changes_not_sent_for_review: false, changes_in_review_behavior: 'ERROR_IF_IN_REVIEW' }], fake.commits
    assert_equal [2002000144], fake.codes
    assert_raises(AegisRelease::Error) { client.upload(aab) }
  end

  def test_play_wrong_upload_hash_never_commits
    client, fake = play
    aab = write('.artifacts/fixture.aab', 'bundle')
    fake.upload_digest = 'wrong'
    assert_raises(AegisRelease::Error) { client.upload(aab) }
    assert_empty fake.commits
    assert_nil fake.current_edit
  end

  def test_play_does_not_retry_by_cancelling_an_existing_review
    client, fake = play
    fake.rows = [::AndroidPublisher::TrackRelease.new(status: 'draft', version_codes: ['2002000144'])]
    fake.fail_commit = true
    assert_raises(AegisRelease::Error) { client.distribute }
    assert_equal 1, fake.commits.length
    assert_equal 'ERROR_IF_IN_REVIEW', fake.commits.first[:changes_in_review_behavior]
    assert_nil fake.current_edit
  end

  def test_play_distribute_cannot_modify_an_unrelated_rollout
    client, fake = play
    fake.rows = [::AndroidPublisher::TrackRelease.new(status: 'draft', version_codes: ['2002000144']),
                 ::AndroidPublisher::TrackRelease.new(status: 'inProgress', version_codes: ['2002000143'])]
    assert_raises(AegisRelease::Error) { client.distribute }
    assert_empty fake.commits
  end
end
