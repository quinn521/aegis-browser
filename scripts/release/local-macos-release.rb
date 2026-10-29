# frozen_string_literal: true
require_relative 'runner'

paths = AegisRelease::ReleaseRunner.validate_local_macos(ENV,
  os: RbConfig::CONFIG['host_os'], cpu: RbConfig::CONFIG['host_cpu'])
AegisRelease.require!(RUBY_VERSION == '3.3.6', 'macOS release requires Ruby 3.3.6')
AegisRelease.require!(AegisRelease.command('node', '--version') == 'v22.23.1', 'macOS release requires Node 22.23.1')
AegisRelease.require!(AegisRelease.command('bundle', '--version') == 'Bundler version 2.5.22', 'macOS release requires Bundler 2.5.22')
source = AegisRelease.source(AegisRelease::ROOT)
AegisRelease.require!(source['sha'] == ENV['GITHUB_SHA'], 'Runner checkout differs from the admitted source')
toolchain = JSON.parse(AegisRelease.command('python3', File.join(AegisRelease::ROOT, 'scripts/release/verify-macos-toolchain.py')))
signing = JSON.parse(AegisRelease.command('python3', File.join(AegisRelease::ROOT, 'scripts/release/verify-macos-signing.py')))
File.open(ENV.fetch('GITHUB_ENV'), 'a') do |file|
  file.puts("BUNDLE_PATH=#{paths.fetch('AEGIS_LOCAL_MACOS_BUNDLE_PATH')}")
  %w[CHROMIUM_ROOT DEPOT_TOOLS_DIR CIPD_CACHE_DIR VPYTHON_VIRTUALENV_ROOT TMPDIR].each do |key|
    file.puts("#{key}=#{paths.fetch(key)}")
  end
  file.puts('DEPOT_TOOLS_UPDATE=0')
end
puts JSON.generate('status' => 'macos_release_inputs_verified', 'source' => source,
                   'toolchain' => toolchain, 'signing' => signing, 'privateKeyExported' => false)
