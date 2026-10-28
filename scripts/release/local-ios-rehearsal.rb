# frozen_string_literal: true
require_relative 'runner'

cache = AegisRelease::ReleaseRunner.validate_local_ios(ENV,
  os: RbConfig::CONFIG['host_os'], cpu: RbConfig::CONFIG['host_cpu'])
AegisRelease.require!(RUBY_VERSION == '3.3.6', 'Local rehearsal requires Ruby 3.3.6')
AegisRelease.require!(AegisRelease.command('node', '--version') == 'v22.23.1', 'Local rehearsal requires Node 22.23.1')
AegisRelease.require!(AegisRelease.command('bundle', '--version') == 'Bundler version 2.5.22', 'Local rehearsal requires Bundler 2.5.22')
source = AegisRelease.source(AegisRelease::ROOT)
AegisRelease.require!(source['sha'] == ENV['GITHUB_SHA'], 'Local runner checkout differs from the admitted source')
File.open(ENV.fetch('GITHUB_ENV'), 'a') { |file| file.puts("BUNDLE_PATH=#{cache}") }
puts JSON.generate('status' => 'local_rehearsal_inputs_verified', 'source' => source,
                   'signing' => 'preinstalled_profiles_and_identity', 'privateKeyExported' => false)
