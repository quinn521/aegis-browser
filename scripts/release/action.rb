# frozen_string_literal: true
require_relative '../../fastlane/lib/release'

module AegisRelease
  class ActionRequest
    FIELDS = %w[profile destination version build run_id tag format].freeze
    STORE_FIELDS = %w[groups track play_release_policy].freeze
    attr_reader :request, :options, :operation

    def initialize(env, root: ROOT)
      @environment = env
      @operation = env.fetch('RELEASE_OPERATION', 'package')
      AegisRelease.require!(%w[package upload distribute submit].include?(operation), 'Invalid workflow operation')
      @options = FIELDS.to_h { |key| [key.to_sym, env["RELEASE_#{key.upcase}"]] }.reject { |_, v| v.nil? || v.empty? }
      @options.delete(:format) if @options[:format] == 'auto'
      settings = JSON.parse(env.fetch('RELEASE_STORE_OPTIONS', '{}'))
      AegisRelease.require!(settings.is_a?(Hash) && (settings.keys - STORE_FIELDS).empty? && settings.values.all? { |v| v.is_a?(String) }, 'store_options accepts only string groups, track and play_release_policy')
      @options.merge!(settings.transform_keys(&:to_sym))
      @options[:manifest_sha256] = env['RELEASE_MANIFEST_SHA256'] unless env['RELEASE_MANIFEST_SHA256'].to_s.empty?
      rehearsal = env['RELEASE_PLATFORM'] == 'android' ? android_rehearsal?(env) : env['AEGIS_RELEASE_REHEARSAL'] == 'true'
      if rehearsal && env['GITHUB_REPOSITORY'] != REPOSITORY && @options[:destination] == 'github-draft'
        @options[:repository] = env.fetch('GITHUB_REPOSITORY')
      end
      @request = Request.new(env.fetch('RELEASE_PLATFORM'), options, root: root)
      request.validate_stage!(operation)
    end

    def android_rehearsal?(env)
      repository = env['GITHUB_REPOSITORY'].to_s
      id = env['GITHUB_REPOSITORY_ID'].to_s
      sha = env['GITHUB_SHA'].to_s
      tag = env['RELEASE_TAG'].to_s
      branch = %w[push workflow_dispatch].include?(env['GITHUB_EVENT_NAME']) && env['GITHUB_REF'] == 'refs/heads/codex/android-release-acceptance'
      candidate = env['GITHUB_EVENT_NAME'] == 'push' && env['GITHUB_REF'] == "refs/tags/#{tag}" &&
        /\Acandidate\/android\/[A-Za-z0-9][A-Za-z0-9_.-]{0,95}\z/.match?(tag) && !tag.include?('..')
      env['GITHUB_ACTIONS'] == 'true' && env['AEGIS_ANDROID_RELEASE_ENABLED'] == 'true' &&
        env['AEGIS_ANDROID_REHEARSAL_ENABLED'] == 'true' && repository != REPOSITORY && !repository.include?('..') &&
        /\A[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+\z/.match?(repository) &&
        /\A[1-9][0-9]{0,15}\z/.match?(id) && id != '1348409663' && env['AEGIS_RELEASE_TEST_REPOSITORY_ID'] == id &&
        /\A[a-f0-9]{40}\z/.match?(sha) && env['AEGIS_ANDROID_REHEARSAL_SOURCE_SHA'] == sha && (branch || candidate)
    end

    def argv(stage)
      stage = operation if stage == 'finish'
      request.validate_stage!(stage)
      if @environment['GITHUB_ACTIONS'] == 'true' && @environment['GITHUB_RUN_ATTEMPT'] != '1' &&
         %w[upload distribute submit].include?(stage) && request.destination != 'github-draft'
        raise Error, 'Store mutations cannot be replayed by rerunning Actions. Inspect retained stage records and store state; use a new build/run after reconciliation'
      end
      [request.platform, stage] + options.flat_map { |key, value| ["--#{key.to_s.tr('_', '-')}", value] }
    end
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    action = AegisRelease::ActionRequest.new(ENV)
    stage = ARGV.fetch(0)
    if stage == 'plan'
      puts JSON.pretty_generate(action.request.plan.merge('through' => action.operation))
    else
      exec(File.join(AegisRelease::ROOT, 'release'), *action.argv(stage))
    end
  rescue ArgumentError, KeyError, JSON::ParserError, AegisRelease::Error => error
    warn error.message
    exit 1
  end
end
