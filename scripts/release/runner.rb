# frozen_string_literal: true
require_relative '../../fastlane/lib/release'

module AegisRelease
  module ReleaseRunner
    LOCAL_IOS_LABELS = %w[self-hosted macOS ARM64 aegis-ios-release-rehearsal].freeze

    def self.local_ios!(env)
      repository = env['GITHUB_REPOSITORY'].to_s
      id = env['GITHUB_REPOSITORY_ID'].to_s
      branch = env['GITHUB_REF'].to_s
      tag = env['RELEASE_TAG'].to_s
      candidate = env['RELEASE_DESTINATION'] == 'github-draft' &&
        env['RELEASE_OPERATION'] == 'upload' && branch == "refs/tags/#{tag}" &&
        /\Acandidate\/ios\/[A-Za-z0-9][A-Za-z0-9_.-]{0,95}\z/.match?(tag) && !tag.include?('..')
      package = env['RELEASE_DESTINATION'] == 'artifacts' && env['RELEASE_OPERATION'] == 'package' &&
        /\Arefs\/heads\/codex\/fastlane-release-[A-Za-z0-9_.-]+\z/.match?(branch)
      AegisRelease.require!(env['GITHUB_ACTIONS'] == 'true' && env['AEGIS_RELEASE_ENABLED'] == 'true' &&
        env['AEGIS_RELEASE_REHEARSAL'] == 'true' && env['AEGIS_IOS_LOCAL_REHEARSAL'] == 'true' &&
        env['RELEASE_PLATFORM'] == 'ios' && repository != REPOSITORY &&
        /\A[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+\z/.match?(repository) &&
        /\A[1-9][0-9]*\z/.match?(id) && id != '1348409663' &&
        env['AEGIS_RELEASE_TEST_REPOSITORY_ID'] == id && (package || candidate),
        'Local iOS signing is limited to explicitly enabled fork package/Draft rehearsals')
    end

    def self.select(env)
      case env['RELEASE_PLATFORM']
      when 'ios'
        return 'macos-26' unless env['AEGIS_IOS_LOCAL_REHEARSAL'] == 'true'
        local_ios!(env)
        LOCAL_IOS_LABELS
      when 'macos', 'android'
        ready = env["#{env['RELEASE_PLATFORM'].upcase}_RUNNERS_READY"]
        ready = env['CHROMIUM_RUNNERS_READY'] if ready.to_s.empty?
        AegisRelease.require!(ready == 'true', "#{env['RELEASE_PLATFORM']} Chromium release runners are not ready")
        return %w[self-hosted macOS ARM64 aegis-release] if env['RELEASE_PLATFORM'] == 'macos'
        architecture = env['AEGIS_ANDROID_RUNNER_ARCH'].to_s
        architecture = 'X64' if architecture.empty?
        AegisRelease.require!(%w[X64 ARM64].include?(architecture), 'Android runner architecture must be X64 or ARM64')
        AegisRelease.require!(architecture != 'ARM64' || env['AEGIS_ANDROID_EXPERIMENTAL_ARM64'] == 'true',
          'Android ARM64 runner requires explicit experimental host opt-in')
        ['self-hosted', 'Linux', architecture, 'aegis-release']
      else
        raise AegisRelease::Error, 'Unknown release platform'
      end
    end

    def self.owned_directory(env, key)
      path = env[key].to_s
      AegisRelease.require!(!path.empty? && !path.match?(/[\x00-\x1f]/), "Provide #{key}")
      AegisRelease.real_path!(path)
      directory = File.realpath(path)
      stat = File.stat(directory)
      AegisRelease.require!(stat.directory? && stat.uid == Process.uid && (stat.mode & 0o022).zero?,
        "#{key} must be an owned directory without group/world write access")
      directory
    end

    def self.validate_local_macos(env, os:, cpu:)
      AegisRelease.require!(env['RELEASE_PLATFORM'] == 'macos' && env['GITHUB_ACTIONS'] == 'true' &&
        env['RUNNER_ENVIRONMENT'] == 'self-hosted' && os.include?('darwin') && %w[arm64 aarch64].include?(cpu),
        'macOS release requires an Apple Silicon self-hosted runner')
      select(env)
      sensitive = %w[AEGIS_ASC_KEY_ID AEGIS_ASC_ISSUER_ID AEGIS_ASC_KEY_PATH AEGIS_ASC_KEY_BASE64
        AEGIS_PLAY_JSON_KEY AEGIS_PLAY_JSON_KEY_DATA MATCH_PASSWORD MATCH_GIT_BASIC_AUTHORIZATION
        AEGIS_ANDROID_STORE_PASSWORD AEGIS_ANDROID_KEY_PASSWORD]
      AegisRelease.require!(sensitive.none? { |name| !env[name].to_s.empty? },
        'macOS DMG packaging must not receive Apple/Play/Android store credentials')
      %w[AEGIS_LOCAL_MACOS_BUNDLE_PATH CHROMIUM_ROOT DEPOT_TOOLS_DIR CIPD_CACHE_DIR VPYTHON_VIRTUALENV_ROOT TMPDIR]
        .to_h { |key| [key, owned_directory(env, key)] }
    end

    def self.validate_local_ios(env, os:, cpu:)
      local_ios!(env)
      AegisRelease.require!(env['RUNNER_ENVIRONMENT'] == 'self-hosted' && os.include?('darwin') &&
        %w[arm64 aarch64].include?(cpu), 'Local iOS rehearsal requires an Apple Silicon self-hosted runner')
      sensitive = %w[AEGIS_ASC_KEY_ID AEGIS_ASC_ISSUER_ID AEGIS_ASC_KEY_PATH AEGIS_ASC_KEY_BASE64
                     AEGIS_PLAY_JSON_KEY AEGIS_PLAY_JSON_KEY_DATA MATCH_PASSWORD MATCH_GIT_BASIC_AUTHORIZATION]
      AegisRelease.require!(sensitive.none? { |name| !env[name].to_s.empty? }, 'Package rehearsal must not receive store or Match credentials')
      path = env['AEGIS_LOCAL_IOS_BUNDLE_PATH'].to_s
      AegisRelease.require!(!path.empty? && !path.match?(/[\x00-\x1f]/), 'Provide the task-owned local bundle cache')
      cache = File.realpath(path)
      stat = File.stat(cache)
      AegisRelease.require!(stat.directory? && stat.uid == Process.uid && (stat.mode & 0o022).zero?,
        'Local bundle cache must be an owned directory without group/world write access')
      cache
    end
  end
end

puts "runner=#{JSON.generate(AegisRelease::ReleaseRunner.select(ENV))}" if $PROGRAM_NAME == __FILE__
