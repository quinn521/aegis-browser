# frozen_string_literal: true
require_relative 'with-play-key'
success = AegisRelease.with_play_key(ENV.to_h) do |child|
  system(child, 'python3', File.join(AegisRelease::ROOT, 'scripts/release/android-delivery.py'), 'stage')
end
exit(success ? 0 : 1)
