# frozen_string_literal: true
require "digest"
require "fileutils"
require "json"
require "open3"
require "pathname"
require "rbconfig"
require "time"
require "uri"

module AegisRelease
  ROOT = File.expand_path("../..", __dir__)
  REPOSITORY = "gcsagroup/aegis-browser"
  APP_ID = "com.gcsa.aegis"
  IOS_TARGETS = { "Aegis" => APP_ID, "SafariWebExtension" => "#{APP_ID}.safari", "ShareExtension" => "#{APP_ID}.share" }.freeze
  DESTINATIONS = {
    'ios' => %w[artifacts testflight testflight-internal testflight-external asc app-store github-draft],
    'android' => %w[artifacts play-internal play-closed play-open play-production github-draft],
    'macos' => %w[artifacts github-draft]
  }.freeze
  ALPHA_DESTINATIONS = {
    'ios' => %w[artifacts testflight testflight-internal github-draft],
    'android' => %w[artifacts play-internal github-draft],
    'macos' => %w[artifacts github-draft]
  }.freeze
  class Error < StandardError; end

  def self.destinations(platform, profile)
    (profile == 'alpha' ? ALPHA_DESTINATIONS : DESTINATIONS).fetch(platform)
  end

  def self.require!(condition, message)
    raise Error, message unless condition
  end

  # Capture failures without echoing credential-bearing command output or arguments.
  def self.command(*argv, cwd: ROOT, env: {})
    stdout, _stderr, status = Open3.capture3({ 'PYTHONDONTWRITEBYTECODE' => '1' }.merge(env), *argv, chdir: cwd)
    require!(status.success?, "#{File.basename(argv.first)} failed (exit #{status.exitstatus}); no success receipt written")
    stdout.strip
  end

  def self.source(root = ROOT, clean: true)
    status = command("git", "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none", cwd: root)
    require!(!clean || status.empty?, "Release requires a clean committed source; commit changes first")
    { "sha" => command("git", "rev-parse", "HEAD", cwd: root),
      "tree" => command("git", "rev-parse", "HEAD^{tree}", cwd: root), "dirty" => !status.empty? }
  end

  def self.real_path!(path)
    expanded = File.expand_path(path)
    cursor = Pathname.new(expanded)
    loop do
      require!(!cursor.symlink?, "Symlink is not allowed in release paths: #{cursor}")
      break if cursor.root?
      cursor = cursor.parent
    end
    expanded
  end

  def self.file!(path)
    real_path!(path)
    require!(File.file?(path) && File.size(path).positive?, "Missing or empty release file: #{path}")
    path
  end

  def self.digest(path)
    Digest::SHA256.file(file!(path)).hexdigest
  end

  def self.write_new(path, object)
    real_path!(path)
    File.open(path, File::WRONLY | File::CREAT | File::EXCL, 0o600) { |f| f.write(JSON.pretty_generate(object) + "\n") }
  end

  class Request
    attr_reader :platform, :destination, :version, :build, :run_id, :tag, :root, :expected_digest,
                :profile, :groups, :track, :format, :repository, :play_release_policy

    def initialize(platform, options = {}, root: ROOT)
      @root = root
      @platform = platform
      AegisRelease.require!(%w[ios android macos].include?(platform), "Platform must be ios, android or macos")
      unknown = options.keys.map(&:to_sym) - %i[destination version build run_id tag manifest_sha256 profile groups track format repository play_release_policy]
      AegisRelease.require!(unknown.empty?, "Unknown release options: #{unknown.join(', ')}")
      options = options.transform_keys(&:to_sym)
      @destination = options.fetch(:destination, "artifacts").to_s
      AegisRelease.require!(DESTINATIONS.fetch(platform).include?(destination), "Unsupported destination for #{platform}: #{destination}")
      @profile = options.fetch(:profile, 'release').to_s.downcase
      AegisRelease.require!(%w[alpha beta release].include?(profile), 'Profile must be alpha, beta or release')
      AegisRelease.require!(AegisRelease.destinations(platform, profile).include?(destination), 'Alpha packages support internal testing only; use a new beta or release package for external distribution')
      AegisRelease.require!(!%w[asc app-store play-production].include?(destination) || profile == 'release', 'Store production destinations require --profile release')
      @groups = options.fetch(:groups, '').to_s.split(',').map(&:strip)
      AegisRelease.require!(groups.uniq == groups && groups.all? { |g| !g.empty? && g.size <= 100 && !g.match?(/[\x00-\x1f]/) }, 'Invalid or duplicate TestFlight groups')
      AegisRelease.require!(groups.empty? || %w[testflight-internal testflight-external].include?(destination), '--groups requires a TestFlight distribution destination')
      @track = options[:track]&.to_s || { 'play-internal' => 'internal', 'play-closed' => 'alpha', 'play-open' => 'beta', 'play-production' => 'production' }[destination]
      AegisRelease.require!(!options[:track] || destination == 'play-closed', '--track overrides only the closed testing track')
      AegisRelease.require!(!track || /\A[a-zA-Z0-9][a-zA-Z0-9_.-]{0,99}\z/.match?(track), 'Invalid Play track')
      AegisRelease.require!(destination != 'play-closed' || !%w[internal beta production].include?(track), 'Closed testing cannot select an internal/open/production track')
      @format = options.fetch(:format, platform == 'android' ? 'aab' : { 'ios' => 'ipa', 'macos' => 'dmg' }[platform]).to_s
      AegisRelease.require!((platform == 'android' ? %w[apk aab] : [platform == 'ios' ? 'ipa' : 'dmg']).include?(format), 'Invalid package format')
      AegisRelease.require!(!destination.start_with?('play-') || format == 'aab', 'Play requires an upload-key signed AAB')
      @repository = options.fetch(:repository, REPOSITORY).to_s
      AegisRelease.require!(/\A[A-Za-z0-9][A-Za-z0-9_.-]*\/[A-Za-z0-9][A-Za-z0-9_.-]*\z/.match?(repository), 'Invalid GitHub repository')
      AegisRelease.require!(!options[:repository] || destination == 'github-draft', '--repository is only for GitHub Draft')
      @play_release_policy = options[:play_release_policy]&.to_s
      AegisRelease.require!(!play_release_policy || (destination == 'play-production' && %w[managed automatic].include?(play_release_policy)), 'Production Play policy must be managed or automatic')
      native_version, native_build = native_identity
      @version = options.fetch(:version, native_version).to_s
      @build = options.fetch(:build, native_build).to_s
      pattern = platform == "ios" ? /\A\d+\.\d+(?:\.\d+)?\z/ : /\A\d+\.\d+\.\d+\.\d+\z/
      AegisRelease.require!(pattern.match?(version) && version.size < 40, "Invalid product version")
      AegisRelease.require!(/\A[1-9]\d{0,9}\z/.match?(build), "Build must be a positive integer")
      if platform != "ios"
        AegisRelease.require!([version, build] == [native_version, native_build], "Chromium version/build must match committed native inputs; update source first")
      end
      @run_id = options.fetch(:run_id, "#{profile}-#{version}-#{build}-#{AegisRelease.source(root, clean: false)['sha'][0, 12]}").to_s
      AegisRelease.require!(/\A[a-zA-Z0-9][a-zA-Z0-9_.-]{0,95}\z/.match?(run_id) && !run_id.include?(".."), "Unsafe run ID")
      @tag = options[:tag]&.to_s
      if destination == "github-draft"
        AegisRelease.require!(tag && /\Acandidate\/#{platform}\/[A-Za-z0-9][A-Za-z0-9_.-]{0,95}\z/.match?(tag) && !tag.include?(".."), "GitHub Draft requires --tag candidate/#{platform}/<version-build>")
      else
        AegisRelease.require!(tag.nil?, "--tag is only supported for github-draft")
      end
      @expected_digest = options[:manifest_sha256]&.to_s
      AegisRelease.require!(!expected_digest || /\A[0-9a-f]{64}\z/.match?(expected_digest), "Invalid manifest SHA-256")
    end

    def native_identity
      if platform == "ios"
        text = File.read(File.join(root, "apps/ios/project.yml"))
        [text[/MARKETING_VERSION: ([0-9.]+)/, 1], text[/CURRENT_PROJECT_VERSION: (\d+)/, 1]]
      else
        text = File.read(File.join(root, "apps/browser/overlay/chrome/browser/ui/webui/help/aegis_github_update.h"))
        version = text[/kProductVersion\[\] = "([0-9.]+)"/, 1]
        build = if platform == "android"
                  File.read(File.join(root, "apps/browser/args/aegis-android.gn"))[/android_override_version_code = "(\d+)"/, 1]
                else
                  version&.split(".")&.last
                end
        [version, build]
      end
    end

    def directory
      File.join(root, ".artifacts/release", platform, run_id)
    end

    def manifest_path
      File.join(directory, "#{platform}-manifest.json")
    end

    def identity
      { "platform" => platform, "applicationId" => APP_ID, "version" => version, "build" => build, "profile" => profile, "format" => format }
    end

    def plan
      identity.merge("destination" => destination, "tag" => tag, "runId" => run_id,
                     "source" => AegisRelease.source(root, clean: false), "output" => directory,
                     "host" => platform == "android" ? "Linux x86_64" : "macOS",
                     "groups" => groups, "track" => track, "repository" => repository,
                     "stages" => %w[package upload distribute submit status],
                     "uploadSubmitsReview" => false, "releaseReady" => false)
    end

    def validate_stage!(stage)
      AegisRelease.require!(%w[plan package upload release distribute submit status].include?(stage), 'Unknown release stage')
      if %w[upload release].include?(stage)
        AegisRelease.require!(destination != 'artifacts', 'Upload requires an external destination')
      elsif stage == 'distribute'
        AegisRelease.require!(%w[testflight-internal testflight-external play-internal play-closed play-open].include?(destination), 'Distribution requires a testing destination')
        AegisRelease.require!(platform != 'ios' || !groups.empty?, 'TestFlight distribution requires explicit groups')
      elsif stage == 'submit'
        AegisRelease.require!(profile == 'release' && %w[app-store play-production].include?(destination), 'Review submission requires a release profile and production destination')
        AegisRelease.require!(platform != 'android' || play_release_policy, 'Play production submission requires an explicit publishing policy')
      elsif stage == 'status'
        AegisRelease.require!(platform != 'macos' && !%w[artifacts github-draft].include?(destination), 'Status requires an Apple or Play destination')
      end
    end
  end

  class Receipt
    def self.create(request, paths, source, details = {})
      AegisRelease.require!(AegisRelease.source(request.root) == source, "Source changed during packaging")
      assets = paths.map do |path|
        AegisRelease.file!(path)
        AegisRelease.require!(File.dirname(File.expand_path(path)) == request.directory, "Artifact must be directly inside the release directory")
        { "name" => File.basename(path), "size" => File.size(path), "sha256" => AegisRelease.digest(path) }
      end
      AegisRelease.require!(assets.any? && assets.map { |a| a['name'] }.uniq.length == assets.length, "Missing or duplicate artifacts")
      value = request.identity.merge("schemaVersion" => 1, "repository" => REPOSITORY, "source" => source,
                                     "assets" => assets, "details" => details, "releaseReady" => false,
                                     "createdAt" => Time.now.utc.iso8601)
      AegisRelease.write_new(request.manifest_path, value)
      sha = AegisRelease.digest(request.manifest_path)
      File.write(request.manifest_path + ".sha256", "#{sha}  #{File.basename(request.manifest_path)}\n", mode: "wx")
      sha
    end

    def self.verify(request, expected_digest)
      AegisRelease.require!(expected_digest && AegisRelease.digest(request.manifest_path) == expected_digest, "Release manifest digest mismatch or missing --manifest-sha256")
      checksum = request.manifest_path + '.sha256'
      AegisRelease.file!(checksum)
      AegisRelease.require!(File.read(checksum) == "#{expected_digest}  #{File.basename(request.manifest_path)}\n", 'Manifest checksum sidecar mismatch')
      value = JSON.parse(File.read(request.manifest_path))
      AegisRelease.require!(value['schemaVersion'] == 1 && value['repository'] == REPOSITORY && value['releaseReady'] == false, "Unsupported release manifest")
      AegisRelease.require!(request.identity.all? { |k, v| value[k] == v }, "Release identity differs from requested platform/version/build")
      AegisRelease.require!(value['source'] == AegisRelease.source(request.root), "Package source differs from current clean checkout")
      assets = value['assets']
      AegisRelease.require!(assets.is_a?(Array) && !assets.empty?, "Missing artifacts")
      names = assets.map { |a| a.fetch('name') }
      AegisRelease.require!(names.uniq == names, "Duplicate artifact names")
      assets.each do |asset|
        name = asset['name']
        AegisRelease.require!(name.is_a?(String) && /\A[A-Za-z0-9][A-Za-z0-9_.-]*\z/.match?(name) && !name.include?(".."), "Unsafe artifact name")
        path = File.join(request.directory, name)
        AegisRelease.require!(AegisRelease.digest(path) == asset['sha256'] && File.size(path) == asset['size'], "Artifact changed: #{name}")
      end
      suffix = ".#{request.format}"
      AegisRelease.require!(names.count { |n| n.end_with?(suffix) } == 1, "Expected exactly one #{suffix} artifact")
      value
    end
  end

  class Pipeline
    attr_reader :request

    def initialize(request, fastlane)
      @request = request
      @fastlane = fastlane
    end

    def required(name)
      value = ENV[name]
      AegisRelease.require!(value && !value.empty?, "Missing #{name}")
      value
    end

    def run(*argv, env: {})
      # Build output is streamed, but commands and environment values are never printed here.
      ok = system({ 'PYTHONDONTWRITEBYTECODE' => '1' }.merge(env), *argv, chdir: request.root)
      AegisRelease.require!(ok, "#{File.basename(argv.first)} failed; retain the attempt directory for diagnosis")
    end

    def package
      host = RbConfig::CONFIG['host_os']
      AegisRelease.require!(request.platform == "android" ? host.include?("linux") && RbConfig::CONFIG['host_cpu'] == 'x86_64' : host.include?("darwin"), "Wrong build host for #{request.platform}")
      source = AegisRelease.source(request.root)
      AegisRelease.real_path!(request.directory)
      AegisRelease.require!(!File.exist?(request.directory), "Attempt already exists; use a new --run-id or upload the verified package")
      if request.platform == 'ios'
        required('AEGIS_APPLE_TEAM_ID')
        ios_profiles # Validate before creating output or touching signing state.
        run('python3', File.join(request.root, 'scripts/release/safari_manifest.py'),
            File.join(request.root, 'apps/ios/SharedWebExtension'))
      else
        chromium_root
        if request.platform == 'macos'
          run('python3', File.join(request.root, 'scripts/release/verify-macos-toolchain.py'))
          run('python3', File.join(request.root, 'scripts/release/verify-macos-signing.py'))
        end
        if request.platform == 'android' && request.format == 'aab'
          %w[AEGIS_BUNDLETOOL_JAR AEGIS_ANDROID_KEYSTORE AEGIS_ANDROID_KEY_ALIAS AEGIS_ANDROID_STORE_PASSWORD AEGIS_ANDROID_KEY_PASSWORD AEGIS_ANDROID_CERT_SHA256].each { |name| required(name) }
        end
      end
      FileUtils.mkdir_p(File.dirname(request.directory))
      Dir.mkdir(request.directory, 0o700)
      paths, details = request.platform == 'ios' ? package_ios : package_chromium
      sha = Receipt.create(request, paths, source, details)
      puts "Manifest SHA-256: #{sha}"
      if ENV['GITHUB_OUTPUT']
        File.open(ENV.fetch('GITHUB_OUTPUT'), 'a') { |f| f.puts("manifest_sha256=#{sha}\nrun_id=#{request.run_id}") }
      end
      sha
    end

    def ios_profiles
      profiles = JSON.parse(required('AEGIS_IOS_PROFILES'))
      AegisRelease.require!(profiles.is_a?(Hash) && profiles.keys.sort == IOS_TARGETS.values.sort && profiles.values.all? { |v| v.is_a?(String) && !v.empty? }, "AEGIS_IOS_PROFILES must name App Store profiles for the app, Safari and Share extensions")
      profiles
    end

    def package_ios
      profiles = ios_profiles
      work = File.join(request.directory, 'work')
      # Copy only committed inputs; ignored local files must not enter the archive.
      tracked = AegisRelease.command('git', 'ls-files', '-z', 'apps/ios',
                                     'apps/browser/overlay/chrome/browser/resources/aegis/enhancements_shared',
                                     'packages/core/src/agent/contracts/v1/agent-contract-v1.vectors.json', cwd: request.root).split("\0")
      tracked.each do |relative|
        original = File.join(request.root, relative)
        AegisRelease.real_path!(original)
        AegisRelease.require!(File.file?(original), 'Missing tracked iOS input')
        target = File.join(work, relative)
        FileUtils.mkdir_p(File.dirname(target))
        FileUtils.cp(original, target)
      end
      project = File.join(work, 'apps/ios/Aegis.xcodeproj')
      team = required('AEGIS_APPLE_TEAM_ID')
      @fastlane.update_code_signing_settings(path: project, use_automatic_signing: false,
                                            team_id: team, code_sign_identity: 'Apple Distribution', build_configurations: ['Release'])
      IOS_TARGETS.each do |target, app_id|
        @fastlane.update_code_signing_settings(path: project, use_automatic_signing: false, team_id: team,
                                              targets: [target], build_configurations: ['Release'], profile_name: profiles.fetch(app_id))
      end
      %w[AegisApp SafariWebExtension ShareExtension].each do |target|
        run('/usr/libexec/PlistBuddy', '-c', "Add :AegisReleaseProfile string #{request.profile}", File.join(work, 'apps/ios', target, 'Info.plist'))
      end
      name = "Aegis-#{request.profile}-#{request.version}-#{request.build}-ios.ipa"
      @fastlane.build_app(project: project, scheme: 'Aegis-Release', configuration: 'Release', clean: true,
                         destination: 'generic/platform=iOS', output_directory: request.directory, output_name: name,
                         archive_path: File.join(work, 'Aegis.xcarchive'), derived_data_path: File.join(work, 'DerivedData'),
                         buildlog_path: File.join(work, 'logs'), export_method: 'app-store',
                         skip_profile_detection: true, xcargs: "MARKETING_VERSION=#{request.version} CURRENT_PROJECT_VERSION=#{request.build}",
                         export_options: { method: 'app-store-connect', teamID: team, signingStyle: 'manual',
                                           provisioningProfiles: profiles, manageAppVersionAndBuildNumber: false,
                                           testFlightInternalTestingOnly: request.profile == 'alpha' })
      ipa = File.join(request.directory, name)
      run('python3', File.join(request.root, 'scripts/release/verify-ipa.py'), ipa, request.version, request.build, request.profile)
      paths = [ipa] + Dir.glob(File.join(request.directory, '*.dSYM.zip'))
      [paths, { 'signing' => 'app-store-export', 'testflight' => 'not_uploaded', 'deviceAcceptance' => 'not_verified',
                'testFlightInternalTestingOnly' => request.profile == 'alpha' }]
    end

    def chromium_root
      root = AegisRelease.real_path!(File.expand_path(required('CHROMIUM_ROOT')))
      AegisRelease.require!(File.directory?(File.join(root, 'src')), "CHROMIUM_ROOT must be a prepared dedicated Chromium workspace")
      root
    end

    def package_chromium
      root = chromium_root
      lock = File.join(root, '.aegis-fastlane-release.lock')
      AegisRelease.real_path!(lock)
      Dir.mkdir(lock) # Never break an existing worker's lock.
      begin
        scripts = File.join(request.root, 'apps/browser/scripts')
        source = File.join(root, 'src')
        verification_args = ['python3', File.join(scripts, 'verify-build-source.py'), '--source', source,
                             '--platform', request.platform == 'android' ? 'android' : 'desktop']
        verification = AegisRelease.command(*verification_args)
        out = File.join(source, 'out', request.platform == 'macos' ? 'AegisRelease' : 'AegisAndroid')
        env = { 'CHROMIUM_ROOT' => root, 'CHROMIUM_SRC' => nil, 'OUT_DIR' => out,
                'AEGIS_ALLOW_DIRTY_IDENTITY' => nil, 'ALLOW_COMPONENT_PACKAGE' => nil,
                'AEGIS_REPLACE_PACKAGE_IDENTITY' => nil, 'AEGIS_BUILD_MANIFEST' => nil,
                'AEGIS_PACKAGE_VERSION' => request.version, 'DIST_DIR' => File.join(request.directory, 'native'),
                'PACKAGE_FORMATS' => 'dmg' }
        if request.platform == 'android'
          env['AEGIS_ANDROID_FORMAT'] = request.format
          artifact = File.join(out, "apks/ChromePublic.#{request.format}")
          AegisRelease.real_path!(artifact)
          if File.exist?(artifact)
            history = File.join(out, '.aegis-fastlane-history')
            AegisRelease.real_path!(history)
            FileUtils.mkdir_p(history)
            saved = File.join(history, "#{request.run_id}.#{request.format}")
            AegisRelease.require!(!File.exist?(saved), 'Android package history already exists')
            File.rename(artifact, saved)
          end
          run('bash', File.join(scripts, 'build-android.sh'), env: env)
          verify_android_identity(artifact)
          run('bash', File.join(scripts, 'package-android.sh'), env: env)
          pattern = "*.#{request.format}"
          signing = request.format == 'aab' ? 'android-upload-key' : 'existing-chromium-signature-not-distribution-qualified'
        else
          AegisRelease.require!(RbConfig::CONFIG['host_cpu'] =~ /arm64|aarch64/, 'macOS release GN currently requires Apple Silicon')
          run('bash', File.join(scripts, 'build-release.sh'), env: env)
          manifest = File.join(out, '.aegis/build-manifest.json')
          env['AEGIS_BUILD_MANIFEST_SHA256'] = AegisRelease.digest(manifest)
          app = File.join(out, 'GCSA Aegis.app/Contents/Info.plist')
          actual_id = AegisRelease.command('/usr/libexec/PlistBuddy', '-c', 'Print :CFBundleIdentifier', app)
          AegisRelease.require!(actual_id == APP_ID, 'macOS CFBundleIdentifier mismatch')
          run('bash', File.join(scripts, 'package.sh'), env: env)
          pattern = '*.dmg'
        end
        after = AegisRelease.command(*verification_args)
        AegisRelease.require!(verification == after, 'Chromium source changed during packaging')
        native = env.fetch('DIST_DIR')
        files = Dir.glob(File.join(native, pattern)).reject { |p| File.basename(p) == "GCSA-aegis.#{request.format}" }
        AegisRelease.require!(files.size == 1, 'Expected one native installable artifact')
        dmg_verification = if request.platform == 'macos'
          JSON.parse(AegisRelease.command('python3', File.join(request.root, 'scripts/release/verify-macos-dmg.py'),
                                         files.first, request.version))
        end
        signing = dmg_verification.fetch('signing') if dmg_verification
        files += Dir.glob(File.join(native, '*.build-identity.json'))
        paths = files.map do |file|
          AegisRelease.file!(file)
          target = File.join(request.directory, File.basename(file))
          if request.platform == 'android' && file.end_with?('.aab')
            sign_android_bundle(file, target)
          else
            FileUtils.cp(file, target)
          end
          if request.platform == 'macos' && file.end_with?('.dmg')
            AegisRelease.require!(AegisRelease.digest(target) == dmg_verification.fetch('sha256'),
                                  'Verified DMG changed before the release manifest was written')
          end
          target
        end
        [paths, { 'signing' => signing, 'chromium' => JSON.parse(verification), 'dmgVerification' => dmg_verification,
                  'nativeSnapshot' => 'build-identity describes the pre-signing native snapshot; release manifest hashes the final package',
                  'certificateSha256' => request.format == 'aab' ? required('AEGIS_ANDROID_CERT_SHA256').delete(':').downcase : nil,
                  'deviceAcceptance' => 'not_verified' }]
      ensure
        Dir.rmdir(lock)
      end
    end

    def upload(expected_digest = request.expected_digest)
      receipt = Receipt.verify(request, expected_digest)
      preflight_upload
      if request.destination == 'github-draft'
        result = Draft.new(request, receipt, expected_digest).upload
        path = File.join(request.directory, "github-upload-#{Time.now.utc.strftime('%Y%m%dT%H%M%S')}.json")
        AegisRelease.write_new(path, result)
      else
        client = store
        client.authenticate
        asset = receipt.fetch('assets').find { |a| a['name'].end_with?(".#{request.format}") }
        path = File.join(request.directory, asset.fetch('name'))
        if request.platform == 'ios'
          run('python3', File.join(request.root, 'scripts/release/verify-ipa.py'), path, request.version, request.build, request.profile)
        else
          AegisRelease.require!(receipt.fetch('details')['signing'] == 'android-upload-key', 'Play requires a verified upload-key signed AAB')
        end
        operation = request.platform == 'ios' ? 'apple-upload' : 'play-upload'
        result = Journal.new(request, expected_digest).perform(operation, 'track' => request.track) { client.upload(path) }
      end
      puts JSON.pretty_generate(result)
      result
    end

    def preflight_upload
      AegisRelease.require!(request.destination != 'artifacts', 'Choose an upload destination')
      AegisRelease.source(request.root)
      # Local manual Fastlane accepts the operator's clean branch. Actions owns CI
      # admission. Upstream GitHub drafts retain exact-main quality admission.
      if ENV['GITHUB_ACTIONS'] == 'true' || (request.destination == 'github-draft' && request.repository == REPOSITORY)
        sha = AegisRelease.source(request.root).fetch('sha')
        run('node', File.join(request.root, 'scripts/ci/release-preflight.mjs'), '--sha', sha,
            env: { 'RELEASE_PLATFORM' => request.platform, 'RELEASE_DESTINATION' => request.destination, 'RELEASE_TAG' => request.tag })
      end
    end

    def store
      AegisRelease.require!(request.platform != 'macos' && !%w[artifacts github-draft].include?(request.destination), 'Choose an Apple or Play store destination')
      request.platform == 'ios' ? AppleStore.new(request, @fastlane) : PlayStore.new(request, @fastlane)
    end

    def status
      client = store
      client.authenticate
      result = request.identity.merge('destination' => request.destination, 'remote' => client.status)
      if File.directory?(request.directory)
        result['localStages'] = Dir.glob(File.join(request.directory, '*-{started,result}.json')).map do |path|
          { 'file' => File.basename(path), 'record' => JSON.parse(File.read(AegisRelease.file!(path))) }
        end
      end
      puts JSON.pretty_generate(result)
      result
    end

    def distribute
      AegisRelease.require!(%w[testflight-internal testflight-external play-internal play-closed play-open].include?(request.destination), 'distribute requires a testing destination')
      store_stage('distribute') { |client| client.distribute }
    end

    def submit
      AegisRelease.require!(request.profile == 'release' && %w[app-store play-production].include?(request.destination), 'submit requires a release profile and production destination')
      if request.platform == 'android'
        AegisRelease.require!(request.play_release_policy, 'Select --play-release-policy managed after checking Play managed publishing, or automatic to allow publishing after approval')
      end
      store_stage('submit') { |client| request.platform == 'ios' ? client.submit : client.distribute }
    end

    def store_stage(stage)
      Receipt.verify(request, request.expected_digest)
      operation = request.platform == 'ios' ? 'apple-upload' : 'play-upload'
      uploaded = JSON.parse(File.read(AegisRelease.file!(File.join(request.directory, "#{operation}-result.json"))))
      intent = uploaded.fetch('intent')
      AegisRelease.require!(intent['manifestSha256'] == request.expected_digest &&
                            request.identity.all? { |k, v| intent[k] == v } &&
                            intent.fetch('parameters')['track'] == request.track,
                            'Distribution/review requires the completed upload record for this package and track')
      preflight_upload
      client = store
      client.authenticate
      # Validate before creating the mutation intent; a missing group or unfinished
      # build is a safe preflight failure that can be fixed without a new upload.
      if request.platform == 'ios'
        client.wait_for_processing if ENV['GITHUB_ACTIONS'] == 'true'
        client.ready_build
        client.groups if stage == 'distribute'
      end
      parameters = { 'destination' => request.destination, 'groups' => request.groups,
                     'track' => request.track, 'playReleasePolicy' => request.play_release_policy }
      result = Journal.new(request, request.expected_digest).perform("#{request.platform}-#{stage}", parameters) { yield client }
      puts JSON.pretty_generate(result)
      result
    end
  end

  class Draft
    def initialize(request, receipt, expected_digest, environment: ENV)
      @request, @receipt, @expected_digest, @environment = request, receipt, expected_digest, environment
    end

    def gh(*args)
      AegisRelease.command('gh', *args)
    end

    def api(path)
      JSON.parse(gh('api', "repos/#{@request.repository}/#{path}", '--header', 'Cache-Control: no-cache'))
    end

    def releases
      JSON.parse(gh('api', "repos/#{@request.repository}/releases?per_page=100", '--paginate', '--slurp')).flatten(1)
    end

    def selected
      matches = releases.select { |r| r['tag_name'] == @request.tag }
      AegisRelease.require!(matches.size <= 1, 'Multiple releases for candidate tag')
      matches.first && validate_release(matches.first)
    end

    def validate_release(value)
      AegisRelease.require!(value.is_a?(Hash) && value['id'].is_a?(Integer) && value['id'].positive? &&
                            value['tag_name'] == @request.tag, 'Release response differs from the candidate tag')
      AegisRelease.require!(value['draft'] == true && value['prerelease'] == true, 'Existing release must remain a candidate draft')
      AegisRelease.require!(value['target_commitish'] == @receipt['source']['sha'], 'Existing draft targets a different source')
      value
    end

    def refresh(release)
      current = validate_release(api("releases/#{release.fetch('id')}"))
      AegisRelease.require!(current['id'] == release['id'], 'Release ID changed')
      current
    end

    def expected_assets
      @receipt.fetch('assets') + [@request.manifest_path, @request.manifest_path + '.sha256'].map do |path|
        { 'name' => File.basename(path), 'sha256' => AegisRelease.digest(path), 'size' => File.size(path) }
      end
    end

    def check_assets(release, complete: false)
      remote = api("releases/#{Integer(release.fetch('id'))}/assets?per_page=100")
      expected = expected_assets.to_h { |a| [a.fetch('name'), a] }
      names = remote.map { |a| a.fetch('name') }
      AegisRelease.require!(names.uniq == names && (names - expected.keys).empty?, 'Unexpected or duplicate draft assets')
      remote.each do |asset|
        local = expected.fetch(asset.fetch('name'))
        AegisRelease.require!(asset['state'] == 'uploaded' && asset['size'] == local['size'] && asset['digest'] == "sha256:#{local['sha256']}", "Existing draft asset differs: #{asset['name']}; never overwrite")
      end
      AegisRelease.require!(!complete || names.sort == expected.keys.sort, 'Draft is missing assets')
      names
    end

    def upload
      Receipt.verify(@request, @expected_digest)
      sha = @receipt.fetch('source').fetch('sha')
      tag = URI.encode_www_form_component(@request.tag)
      AegisRelease.require!(api("commits/#{tag}")['sha'] == sha, 'Existing upstream candidate tag must resolve to the exact package commit')
      parameters = { 'repository' => @request.repository, 'tag' => @request.tag }
      journal = Journal.new(@request, @expected_digest)
      created = File.join(@request.directory, 'github-draft-create-result.json')
      release = if File.exist?(created)
                  journal.perform('github-draft-create', parameters) { raise Error, 'Missing completed draft creation record' }
                else
                  selected
                end
      unless release
        AegisRelease.require!(@environment['GITHUB_ACTIONS'] != 'true' || @environment['GITHUB_RUN_ATTEMPT'] == '1',
          'Actions rerun cannot create another draft; reconcile the earlier release or retained creation record')
        notes = File.join(@request.directory, 'draft-notes.txt')
        text = "Candidate package: #{@request.platform} #{@request.profile} #{@request.version} (#{@request.build})\n\nSource: #{sha}\nSigning: #{@receipt.fetch('details').fetch('signing')}\nDevice acceptance: not verified. Distribution qualification: not complete.\n"
        AegisRelease.real_path!(notes)
        if File.exist?(notes)
          AegisRelease.require!(File.read(notes) == text, 'Existing draft notes differ; retain the previous attempt')
        else
          File.write(notes, text, mode: 'wx')
        end
        Receipt.verify(@request, @expected_digest)
        payload = { 'tag_name' => @request.tag, 'target_commitish' => sha, 'draft' => true, 'prerelease' => true,
                    'name' => "Aegis #{@request.platform} #{@request.profile} #{@request.version} (#{@request.build}) candidate", 'body' => text }
        input = File.join(@request.directory, 'draft-request.json')
        AegisRelease.real_path!(input)
        if File.exist?(input)
          AegisRelease.require!(JSON.parse(File.read(AegisRelease.file!(input))) == payload, 'Existing draft request differs')
        else
          AegisRelease.write_new(input, payload)
        end
        release = journal.perform('github-draft-create', parameters) do
          AegisRelease.require!(api("commits/#{tag}")['sha'] == sha, 'Candidate tag changed before draft creation')
          validate_release(JSON.parse(gh('api', "repos/#{@request.repository}/releases", '--method', 'POST', '--input', input)))
        end
      end
      release = refresh(validate_release(release))
      present = check_assets(release)
      expected_assets.each do |asset|
        next if present.include?(asset['name'])
        AegisRelease.require!(asset['size'] < 2 * 1024**3, 'GitHub asset must be smaller than 2 GiB')
        Receipt.verify(@request, @expected_digest)
        refresh(release) # Check the known ID before sending the next asset.
        gh('release', 'upload', @request.tag, File.join(@request.directory, asset['name']), '--repo', @request.repository)
      end
      release = refresh(release)
      AegisRelease.require!(api("commits/#{tag}")['sha'] == sha, 'Candidate tag changed during upload')
      Receipt.verify(@request, @expected_digest)
      check_assets(release, complete: true)
      { 'status' => 'draft_assets_verified', 'url' => release.fetch('html_url'), 'source' => @receipt['source'],
        'tag' => @request.tag, 'draft' => true, 'releaseReady' => false }
    end
  end
end

require_relative "stores"
require_relative "android"
