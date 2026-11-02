# frozen_string_literal: true
require 'json'
require 'stringio'
require 'fileutils'
require_relative '../../fastlane/lib/android_status'

module AegisRelease
  class AndroidStatusCredential
    def self.token_provider(data)
      require 'googleauth'
      value = JSON.parse(data)
      # Accept the explicitly supplied service account only; never fall back to
      # ambient credentials or another app's default identity.
      raise AndroidBuildStatus::QueryError, 'Expected explicit Play service-account credentials' unless value.is_a?(Hash) && value['type'] == 'service_account'
      raise AndroidBuildStatus::QueryError, 'Unexpected Play token endpoint' unless value['token_uri'] == 'https://oauth2.googleapis.com/token'
      credentials = Google::Auth::ServiceAccountCredentials.make_creds(json_key_io: StringIO.new(data), scope: ['https://www.googleapis.com/auth/androidpublisher'])
      -> { credentials.apply({}).fetch(:authorization).delete_prefix('Bearer ') }
    rescue StandardError
      raise AndroidBuildStatus::QueryError, 'Play credential initialization failed', cause: nil
    end
  end
end

if $PROGRAM_NAME == __FILE__
  query = nil
  begin
    plan = JSON.parse(ENV.fetch('ANDROID_DELIVERY_PLAN'))
    mode = plan.fetch('operation')
    raise AegisRelease::AndroidBuildStatus::QueryError, 'Expected a read-only status plan' unless %w[status-query status-wait].include?(mode) && plan['destination'] == 'play-internal' && plan['track'] == ''
    api = AegisRelease::AndroidStatusApi.new(token: AegisRelease::AndroidStatusCredential.token_provider(ENV.fetch('AEGIS_PLAY_JSON_KEY_DATA')))
    query = AegisRelease::AndroidBuildStatus.new(version: plan.fetch('version'), build: plan.fetch('build'), track: 'internal', api: api)
    result = mode == 'status-wait' ? query.wait : query.call
    puts JSON.generate(result)
  rescue StandardError
    warn 'Android status query failed; inspect retained observation and Play. No edit or upload was attempted.'
    exit 1
  ensure
    if query&.last_result
      directory = File.expand_path('../../.artifacts/android-status', __dir__)
      FileUtils.mkdir_p(directory, mode: 0o700)
      File.write(File.join(directory, 'status.json'), JSON.pretty_generate(query.last_result)+"\n", mode: 'w', perm: 0o600)
    end
  end
end
