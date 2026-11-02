# frozen_string_literal: true
require 'minitest/autorun'
require_relative '../lib/android_status'

class AndroidStatusTest < Minitest::Test
  class Api
    attr_reader :paths
    def initialize(*responses)
      @responses, @paths = responses, []
    end
    def get(path)
      @paths << path
      @responses.size > 1 ? @responses.shift : @responses.first
    end
  end
  def row(code = 266, state = 'PUBLISHED')
    { 'releases' => [{ 'track' => 'internal', 'releaseName' => 'arbitrary display name',
      'activeArtifacts' => [{ 'versionCode' => code }], 'releaseLifecycleState' => 'RELEASE_LIFECYCLE_STATE_' + state }] }
  end
  def query(api, **values)
    AegisRelease::AndroidBuildStatus.new(**{ version: '6.1.0.266', build: '266', track: 'internal', api: api }.merge(values))
  end
  def test_exact_code_get_without_version_or_package_provenance_inference
    api = Api.new(row)
    result = query(api).call
    assert_equal ['/androidpublisher/v3/applications/com.gcsa.aegis/tracks/internal/releases'], api.paths
    assert_equal 'RELEASE_LIFECYCLE_STATE_PUBLISHED', result['remote']['lifecycleState']
    refute result['remote']['versionNameVerified']
    assert_equal 'not_verified', result['remote']['binaryProvenance']
    refute result.key?('source')
    refute result.key?('manifestSha256')
  end
  def test_missing_or_obsolete_not_equated_with_unused_version_code
    [row(265), {}].each do |response|
      result = query(Api.new(response)).call
      assert_equal 'NOT_OBSERVED', result['remote']['lifecycleState']
      refute result['remote']['absenceProvesUnusedVersionCode']
    end
  end
  def test_invalid_identity_and_track_cannot_reach_api
    api = Api.new(row)
    [{build: '0266'}, {build: '2100000001'}, {version: '6.1'}, {track: '../production'}, {track: "internal\n"}].each do |values|
      assert_raises(AegisRelease::AndroidBuildStatus::QueryError) { query(api, **values) }
    end
    assert_empty api.paths
  end
  def test_malformed_wrong_track_unknown_state_and_duplicate_rows_fail_closed
    wrong = row; wrong['releases'][0]['track'] = 'production'
    duplicate = row; duplicate['releases'] *= 2
    [nil, [], {'error' => 'denied'}, {'releases' => nil}, wrong, duplicate, row(266, 'FUTURE_STATE')].each do |value|
      assert_raises(AegisRelease::AndroidBuildStatus::QueryError) { query(Api.new(value)).call }
    end
  end
  def test_wait_observes_exact_release_and_never_mutates
    api = Api.new(row(265), row(266, 'IN_REVIEW'), row)
    now = 0
    result = query(api).wait(timeout: 60, interval: 1, clock: -> { now }, sleeper: ->(seconds) { now += seconds })
    assert_equal 3, api.paths.size
    assert_equal 'RELEASE_LIFECYCLE_STATE_PUBLISHED', result['remote']['lifecycleState']
  end
  def test_wait_is_bounded_and_rejection_is_not_success
    now = 0
    assert_raises(AegisRelease::AndroidBuildStatus::QueryError) do
      query(Api.new({})).wait(timeout: 2, interval: 1, clock: -> { now }, sleeper: ->(seconds) { now += seconds })
    end
    assert_equal 2, now
    assert_raises(AegisRelease::AndroidBuildStatus::QueryError) { query(Api.new(row(266, 'NOT_APPROVED'))).wait }
  end
  def test_transport_refuses_edit_other_app_or_injected_path_before_token
    api = AegisRelease::AndroidStatusApi.new(token: -> { flunk 'Token must not be requested' })
    ['/androidpublisher/v3/applications/com.gcsa.aegis/edits', '/androidpublisher/v3/applications/other/tracks/internal/releases', '/androidpublisher/v3/applications/com.gcsa.aegis/tracks/internal/releases?x=1'].each do |path|
      assert_raises(AegisRelease::AndroidBuildStatus::QueryError) { api.get(path) }
    end
  end

  class Http
    attr_accessor :use_ssl, :open_timeout, :read_timeout, :write_timeout, :max_retries
    attr_reader :requests
    def initialize(code: '200', chunks: ['{}'], error: nil)
      @code, @chunks, @error, @requests = code, chunks, error, []
    end
    def start
      raise @error if @error
      yield self
    end
    def request(request)
      @requests << request
      yield self
    end
    def code = @code
    def read_body
      @chunks.each { |chunk| yield chunk }
    end
  end

  def transport(http)
    AegisRelease::AndroidStatusApi.new(token: -> { 'PRIVATE FIXTURE' }, http_factory: ->(uri) {
      assert_equal 'https://androidpublisher.googleapis.com/androidpublisher/v3/applications/com.gcsa.aegis/tracks/internal/releases', uri.to_s
      http
    })
  end

  def test_real_get_request_tls_auth_timeouts_and_bounded_stream
    http = Http.new(chunks: ['{"releases":', '[]}'])
    assert_equal({'releases' => []}, transport(http).get('/androidpublisher/v3/applications/com.gcsa.aegis/tracks/internal/releases'))
    assert_equal 1, http.requests.size
    request = http.requests.first
    assert_equal 'GET', request.method
    assert_nil request.body
    assert_equal 'Bearer PRIVATE FIXTURE', request['Authorization']
    assert_equal 'application/json', request['Accept']
    assert http.use_ssl
    assert_equal [20, 60, 20, 0], [http.open_timeout, http.read_timeout, http.write_timeout, http.max_retries]
  end

  def test_redirect_auth_failure_bad_json_overflow_and_network_errors_never_fall_back
    [Http.new(code: '302'), Http.new(code: '401'), Http.new(code: '403'), Http.new(chunks: ['PRIVATE INVALID JSON']),
     Http.new(chunks: ['x'*(2*1024*1024), 'x']), Http.new(error: IOError.new('PRIVATE ERROR'))].each do |http|
      error = assert_raises(AegisRelease::AndroidBuildStatus::QueryError) do
        transport(http).get('/androidpublisher/v3/applications/com.gcsa.aegis/tracks/internal/releases')
      end
      refute_includes error.message, 'PRIVATE'
      assert_operator http.requests.size, :<=, 1
    end
  end

  def test_wait_preserves_observation_and_distinguishes_developer_action
    result = query(Api.new(row(266, 'NOT_SENT_FOR_REVIEW'))).wait
    assert_equal 'developer_action_required', result['waitOutcome']
    q = query(Api.new(row(266, 'IN_REVIEW')))
    now = 0
    assert_raises(AegisRelease::AndroidBuildStatus::QueryError) do
      q.wait(timeout: 1, interval: 1, clock: -> { now }, sleeper: ->(seconds) { now += seconds })
    end
    assert_equal 'RELEASE_LIFECYCLE_STATE_IN_REVIEW', q.last_result['remote']['lifecycleState']
  end
end
