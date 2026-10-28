# frozen_string_literal: true
require "optparse"
require "rubygems"
require_relative "../../fastlane/lib/release"

begin
  options = {}
  parser = OptionParser.new do |p|
    p.banner = "Usage: ./release <ios|android|macos> <plan|package|upload|release> [options]"
    p.on("--destination NAME", "artifacts (default), testflight, github-draft") { |v| options[:destination] = v }
    p.on("--version VERSION", "iOS marketing version; desktop must match committed version") { |v| options[:version] = v }
    p.on("--build NUMBER", "iOS build; Android must match committed versionCode") { |v| options[:build] = v }
    p.on("--run-id ID", "Unique local attempt directory; reuse only for upload") { |v| options[:run_id] = v }
    p.on("--tag TAG", "Existing candidate/<platform>/... tag for GitHub Draft") { |v| options[:tag] = v }
    p.on("--manifest-sha256 SHA", "Required for upload of an existing package") { |v| options[:manifest_sha256] = v }
    p.on("-h", "--help") { puts p; exit 0 }
  end
  parser.parse!
  raise ArgumentError, parser.to_s unless (1..2).cover?(ARGV.length)
  platform, stage = ARGV
  stage ||= "plan"
  raise ArgumentError, "Unknown stage: #{stage}" unless %w[plan package upload release].include?(stage)
  request = AegisRelease::Request.new(platform, options)
  if stage == "plan"
    puts JSON.pretty_generate(request.plan)
  else
    AegisRelease.require!(Gem::Version.new(RUBY_VERSION) >= Gem::Version.new('3.3'), 'Use Ruby 3.3 or newer on PATH, then run bundle install')
    raise ArgumentError, "upload requires --manifest-sha256" if stage == "upload" && !options[:manifest_sha256]
    raise ArgumentError, "release/upload requires an upload destination" if %w[release upload].include?(stage) && request.destination == "artifacts"
    Dir.chdir(AegisRelease::ROOT)
    ENV["FASTLANE_SKIP_UPDATE_CHECK"] = "1"
    ENV["FASTLANE_HIDE_GITHUB_ISSUES"] = "1"
    ENV["FASTLANE_OPT_OUT_USAGE"] = "1"
    ENV["BUNDLE_GEMFILE"] = File.join(AegisRelease::ROOT, "Gemfile")
    exec("bundle", "exec", "fastlane", platform == "macos" ? "mac" : platform, stage,
         *options.map { |key, value| "#{key}:#{value}" })
  end
rescue ArgumentError, AegisRelease::Error, OptionParser::ParseError => e
  warn e.message
  exit 1
end
