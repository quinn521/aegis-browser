# frozen_string_literal: true
require_relative '../../fastlane/lib/release'

module AegisRelease
  class CandidateTag
    def self.parse(tag, root: ROOT)
      match = /\Acandidate\/(ios|android|macos)\/(temporary-ad-hoc\/)?(\d+\.\d+\.\d+(?:\.\d+)?)-(alpha|beta|release)\.([1-9]\d*)\z/.match(tag)
      AegisRelease.require!(match, 'Use candidate/<platform>/<version>-<alpha|beta|release>.<build>')
      platform, temporary, version, profile, build = match.captures
      Request.new(platform, { destination: 'github-draft', tag: tag, version: version, profile: profile, build: build,
                             signing_mode: temporary ? 'temporary-ad-hoc' : 'native' }, root: root)
    end
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    tag = ARGV.fetch(0) { ENV.fetch('GITHUB_REF_NAME') }
    request = AegisRelease::CandidateTag.parse(tag)
    if ENV['GITHUB_ACTIONS'] == 'true'
      AegisRelease.require!(ENV['GITHUB_REF'] == "refs/tags/#{tag}", 'Tag event and tag request differ')
      AegisRelease.require!(AegisRelease.source.fetch('sha') == ENV.fetch('GITHUB_SHA'), 'Checkout differs from the tag event commit')
    end
    puts JSON.pretty_generate(request.plan)
    if ENV['GITHUB_OUTPUT']
      File.open(ENV.fetch('GITHUB_OUTPUT'), 'a') do |output|
        %i[platform profile version build tag signing_mode].each { |field| output.puts("#{field}=#{request.public_send(field)}") }
      end
    end
  rescue ArgumentError, KeyError, AegisRelease::Error => error
    warn error.message
    exit 1
  end
end
