# frozen_string_literal: true
require 'tmpdir'
require_relative '../../fastlane/lib/release'

module AegisRelease
  def self.with_play_key(environment)
    data = environment.fetch('AEGIS_PLAY_JSON_KEY_DATA')
    begin
      parsed = JSON.parse(data)
    rescue JSON::ParserError
      # Parser messages can include fragments of a malformed private key.
      raise Error, 'Play credential JSON is invalid', cause: nil
    end
    require!(parsed.is_a?(Hash) && %w[service_account external_account].include?(parsed['type']), 'Expected Play service-account or workload identity JSON')
    Dir.mktmpdir('aegis-play-', environment['RUNNER_TEMP']) do |directory|
      path = File.join(directory, 'credential.json')
      File.open(path, File::WRONLY | File::CREAT | File::EXCL, 0o600) { |f| f.write(data) }
      child = environment.merge('AEGIS_PLAY_JSON_KEY' => path, 'AEGIS_PLAY_JSON_KEY_DATA' => nil)
      yield child
    end
  end
end

if $PROGRAM_NAME == __FILE__
  success = AegisRelease.with_play_key(ENV.to_h) do |child|
    system(child, 'bash', File.join(AegisRelease::ROOT, 'scripts/release/action.sh'), ARGV.fetch(0))
  end
  exit(success ? 0 : 1)
end
