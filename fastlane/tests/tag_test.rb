# frozen_string_literal: true
require_relative 'actions_test'
require_relative '../../scripts/release/tag'

class CandidateTagTest < ReleaseFixture
  def parse(tag)
    AegisRelease::CandidateTag.parse(tag, root: @root)
  end

  def test_candidate_tags_bind_profile_platform_version_and_build
    ios = parse('candidate/ios/0.8.0-alpha.1')
    assert_equal ['ios', 'alpha', '0.8.0', '1', 'github-draft'],
                 [ios.platform, ios.profile, ios.version, ios.build, ios.destination]
    android = parse('candidate/android/2.2.0.144-beta.2002000144')
    assert_equal 'aab', android.format
    assert_equal 'macos', parse('candidate/macos/2.2.0.144-release.144').platform
  end

  def test_formal_ambiguous_and_injected_tags_do_not_enter_candidate_packaging
    %w[v0.8.0 candidate/ios/0.8.0 candidate/ios/0.8.0-alpha.0 candidate/ios/0.8.0-rc.1
       candidate/ios/0.8.0-alpha.1/extra candidate/android/0.8.0-alpha.1].each do |tag|
      assert_raises(AegisRelease::Error, tag) { parse(tag) }
    end
    assert_raises(AegisRelease::Error) { parse("candidate/ios/0.8.0-alpha.1\nmalicious") }
    refute File.directory?(File.join(@root, '.artifacts'))
  end

  def test_temporary_macos_tags_cannot_be_confused_with_native_or_other_platforms
    temporary = parse('candidate/macos/temporary-ad-hoc/2.2.0.144-beta.144')
    assert_equal 'temporary-ad-hoc', temporary.signing_mode
    assert_equal 'temporary-ad-hoc', temporary.identity['signingMode']
    assert_equal 'native', parse('candidate/macos/2.2.0.144-beta.144').signing_mode
    %w[ios android].each do |platform|
      assert_raises(AegisRelease::Error) { parse("candidate/#{platform}/temporary-ad-hoc/2.2.0.144-beta.144") }
    end
    assert_raises(AegisRelease::Error) do
      request('macos', signing_mode: 'temporary-ad-hoc', destination: 'github-draft', tag: 'candidate/macos/2.2.0.144-beta.144')
    end
    assert_raises(AegisRelease::Error) do
      request('macos', destination: 'github-draft', tag: 'candidate/macos/temporary-ad-hoc/2.2.0.144-beta.144')
    end
  end
end
