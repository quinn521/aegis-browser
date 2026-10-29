# frozen_string_literal: true
require_relative '../../fastlane/lib/release'

module AegisRelease
  class ActionRequest
    FIELDS = %w[profile destination version build run_id tag format signing_mode].freeze
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
      if env['AEGIS_RELEASE_REHEARSAL'] == 'true' && env['GITHUB_REPOSITORY'] != REPOSITORY && @options[:destination] == 'github-draft'
        @options[:repository] = env.fetch('GITHUB_REPOSITORY')
      end
      @request = Request.new(env.fetch('RELEASE_PLATFORM'), options, root: root)
      request.validate_stage!(operation)
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
