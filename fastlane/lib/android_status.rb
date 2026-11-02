# frozen_string_literal: true
require 'json'
require 'net/http'
require 'time'
require 'uri'

module AegisRelease
  # Query implementation source is never provenance for a remote binary.
  # https://developers.google.com/android-publisher/api-ref/rest/v3/applications.tracks.releases/list
  class AndroidBuildStatus
    class QueryError < StandardError; end
    attr_reader :last_result
    STATES = %w[RELEASE_LIFECYCLE_STATE_UNSPECIFIED RELEASE_LIFECYCLE_STATE_DRAFT
      RELEASE_LIFECYCLE_STATE_NOT_SENT_FOR_REVIEW RELEASE_LIFECYCLE_STATE_IN_REVIEW
      RELEASE_LIFECYCLE_STATE_APPROVED_NOT_PUBLISHED RELEASE_LIFECYCLE_STATE_NOT_APPROVED
      RELEASE_LIFECYCLE_STATE_PUBLISHED].freeze

    def initialize(version:, build:, track:, api:)
      check(version.is_a?(String) && version.match?(/\A[0-9]{1,8}(?:\.[0-9]{1,8}){3}\z/), 'Invalid Android version')
      check(build.is_a?(String) && build.match?(/\A[1-9][0-9]{0,9}\z/) && build.to_i <= 2_100_000_000, 'Invalid Android versionCode')
      check(track.is_a?(String) && track.match?(/\A[A-Za-z0-9][A-Za-z0-9_.-]{0,99}\z/) && !track.include?('..'), 'Invalid Play track')
      @version, @build, @track, @api = version, build, track, api
    end

    def call
      path = "/androidpublisher/v3/applications/com.gcsa.aegis/tracks/#{@track}/releases"
      response = @api.get(path)
      check(response.is_a?(Hash) && (response.keys - ['releases']).empty?, 'Unexpected Play release response')
      rows = response.fetch('releases', [])
      check(rows.is_a?(Array) && rows.size <= 20, 'Invalid Play release list')
      matches = rows.select do |row|
        check(row.is_a?(Hash) && row['track'] == @track && STATES.include?(row['releaseLifecycleState']), 'Invalid Play release summary')
        check(!row.key?('releaseName') || row['releaseName'].is_a?(String), 'Invalid Play release name')
        artifacts = row.fetch('activeArtifacts', [])
        check(artifacts.is_a?(Array) && artifacts.all? { |a| a.is_a?(Hash) && a['versionCode'].is_a?(Integer) && a['versionCode'].positive? && a['versionCode'] <= 2_100_000_000 }, 'Invalid Play artifact summary')
        check(artifacts.map { |a| a['versionCode'] }.uniq.size == artifacts.size, 'Duplicate Play artifact summary')
        artifacts.any? { |a| a['versionCode'].to_s == @build }
      end
      check(matches.size <= 1, 'Ambiguous Play release for requested versionCode')
      item = matches.first
      @last_result = { 'applicationId' => 'com.gcsa.aegis', 'requestedVersion' => @version, 'versionCode' => @build,
        'track' => @track, 'remote' => { 'lifecycleState' => item ? item.fetch('releaseLifecycleState') : 'NOT_OBSERVED',
          'releaseName' => item && item['releaseName'], 'versionNameVerified' => false,
          'lookupScope' => 'non_obsolete_releases_in_selected_track_at_most_20',
          'absenceProvesUnusedVersionCode' => false, 'binaryProvenance' => 'not_verified',
          'testerAvailability' => 'not_verified', 'deviceAcceptance' => 'not_verified',
          'observedAt' => Time.now.utc.iso8601 }, 'transport' => 'GET_only_no_edit' }
    end

    def wait(timeout: 600, interval: 30, clock: -> { Process.clock_gettime(Process::CLOCK_MONOTONIC) }, sleeper: ->(seconds) { sleep(seconds) })
      check(timeout.is_a?(Integer) && timeout.between?(1, 1800) && interval.is_a?(Integer) && interval.between?(1, 60), 'Invalid bounded wait')
      deadline = clock.call + timeout
      loop do
        result = call
        state = result.fetch('remote').fetch('lifecycleState')
        check(state != 'RELEASE_LIFECYCLE_STATE_NOT_APPROVED', 'Play release was not approved; inspect Play')
        unless %w[NOT_OBSERVED RELEASE_LIFECYCLE_STATE_UNSPECIFIED RELEASE_LIFECYCLE_STATE_DRAFT RELEASE_LIFECYCLE_STATE_IN_REVIEW].include?(state)
          result['waitOutcome'] = state == 'RELEASE_LIFECYCLE_STATE_PUBLISHED' ? 'observed_published_on_track' : 'developer_action_required'
          return result
        end
        remaining = deadline - clock.call
        check(remaining.positive?, 'Play status wait timed out; no mutation or retry performed')
        sleeper.call([interval, remaining].min)
      end
    end

    private

    def check(condition, message)
      raise QueryError, message unless condition
    end
  end

  class AndroidStatusApi
    def initialize(token:, http_factory: ->(uri) { Net::HTTP.new(uri.host, uri.port, nil) })
      @token, @http_factory = token, http_factory
    end

    def get(path)
      unless path.match?(%r{\A/androidpublisher/v3/applications/com\.gcsa\.aegis/tracks/[A-Za-z0-9][A-Za-z0-9_.-]{0,99}/releases\z}) && !path.include?('..')
        raise AndroidBuildStatus::QueryError, 'Unexpected Play query path'
      end
      uri = URI('https://androidpublisher.googleapis.com' + path)
      request = Net::HTTP::Get.new(uri)
      token = @token.call
      raise AndroidBuildStatus::QueryError, 'Invalid Play authorization' unless token.is_a?(String) && !token.empty? && !token.match?(/[\r\n]/)
      request['Authorization'] = 'Bearer ' + token
      request['Accept'] = 'application/json'
      http = @http_factory.call(uri)
      http.use_ssl = true
      http.open_timeout, http.read_timeout, http.write_timeout = 20, 60, 20
      http.max_retries = 0
      body = +''
      http.start do |connection|
        connection.request(request) do |response|
          raise AndroidBuildStatus::QueryError, "Play status HTTP #{response.code}; no edit fallback" unless response.code == '200'
          response.read_body do |chunk|
            raise AndroidBuildStatus::QueryError, 'Play status response exceeds bound' if body.bytesize + chunk.bytesize > 2 * 1024 * 1024
            body << chunk
          end
        end
      end
      JSON.parse(body)
    rescue JSON::ParserError
      raise AndroidBuildStatus::QueryError, 'Play status response is invalid JSON', cause: nil
    rescue AndroidBuildStatus::QueryError
      raise
    rescue StandardError
      raise AndroidBuildStatus::QueryError, 'Play status transport failed; no edit fallback', cause: nil
    end
  end
end
