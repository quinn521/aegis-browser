# frozen_string_literal: true

module AegisRelease
  # A process lock plus an immutable intent survives a lost network response. Never
  # retry a possibly completed mutation just because the client did not see success.
  class Journal
    def initialize(request, digest)
      @request, @digest = request, digest
    end

    def perform(operation, parameters = {})
      root = @request.directory
      lock_path = AegisRelease.real_path!(File.join(root, '.store.lock'))
      File.open(lock_path, File::RDWR | File::CREAT, 0o600) do |lock|
        AegisRelease.require!(lock.flock(File::LOCK_EX | File::LOCK_NB), 'Another store stage is running for this package')
        Receipt.verify(@request, @digest)
        intent = @request.identity.merge('manifestSha256' => @digest, 'operation' => operation, 'parameters' => parameters)
        started = File.join(root, "#{operation}-started.json")
        finished = File.join(root, "#{operation}-result.json")
        if File.exist?(finished)
          result = JSON.parse(File.read(AegisRelease.file!(finished)))
          AegisRelease.require!(result['intent'] == intent, 'Completed stage has different parameters; inspect status before changing destinations')
          return result.fetch('result')
        end
        AegisRelease.require!(!File.exist?(started), 'Earlier store stage has an unknown outcome. Run status and reconcile with the store; automatic retries are disabled')
        AegisRelease.write_new(started, intent.merge('startedAt' => Time.now.utc.iso8601, 'outcome' => 'unknown'))
        result = yield
        AegisRelease.write_new(finished, 'intent' => intent, 'result' => result, 'completedAt' => Time.now.utc.iso8601)
        result
      end
    end
  end

  class AppleStore
    def initialize(request, fastlane)
      @request, @fastlane = request, fastlane
    end

    def authenticate
      require 'spaceship'
      key = { key_id: ENV.fetch('AEGIS_ASC_KEY_ID'), issuer_id: ENV.fetch('AEGIS_ASC_ISSUER_ID') }
      if ENV['AEGIS_ASC_KEY_PATH'].to_s != ''
        path = AegisRelease.file!(File.realpath(ENV.fetch('AEGIS_ASC_KEY_PATH')))
        AegisRelease.require!((File.stat(path).mode & 0o077).zero?, 'ASC key file must be private (mode 600)')
        key[:key_filepath] = path
      else
        key.merge!(key_content: ENV.fetch('AEGIS_ASC_KEY_BASE64'), is_key_content_base64: true)
      end
      @api_key = @fastlane.app_store_connect_api_key(**key)
      @app = Spaceship::ConnectAPI::App.find(APP_ID)
      AegisRelease.require!(@app && @app.bundle_id == APP_ID, 'ASC application not found for the exact bundle ID')
    end

    def build
      rows = Spaceship::ConnectAPI::Build.all(app_id: @app.id, version: @request.version,
                                             build_number: @request.build, platform: 'IOS')
      AegisRelease.require!(rows.length <= 1, 'Ambiguous App Store Connect build')
      rows.first
    end

    def ready_build
      item = build
      AegisRelease.require!(item && item.processing_state == 'VALID' && !item.expired, 'Exact build must finish processing and remain unexpired; run status later')
      item
    end

    def wait_for_processing
      require 'fastlane_core/build_watcher'
      FastlaneCore::BuildWatcher.wait_for_build_processing_to_be_complete(
        app_id: @app.id, platform: 'IOS', app_version: @request.version, build_version: @request.build,
        poll_interval: 15, timeout_duration: 1800, select_latest: false,
        return_spaceship_testflight_build: false, wait_for_build_beta_detail_processing: true)
    end

    def groups
      AegisRelease.require!(!@request.groups.empty?, 'Distribution requires --groups with existing TestFlight group names')
      all = @app.get_beta_groups
      @request.groups.map do |name|
        matches = all.select { |group| group.name == name }
        AegisRelease.require!(matches.size == 1, "TestFlight group missing or ambiguous: #{name}")
        group = matches.first
        internal = @request.destination == 'testflight-internal'
        AegisRelease.require!(group.is_internal_group == internal, "TestFlight group type differs from destination: #{name}")
        group
      end
    end

    def status
      item = build
      version = @app.get_app_store_versions(filter: { platform: 'IOS', versionString: @request.version }).first
      { 'appId' => @app.id, 'buildId' => item&.id, 'processingState' => item&.processing_state || 'NOT_FOUND',
        'expired' => item&.expired, 'internalState' => item&.build_beta_detail&.internal_build_state,
        'externalState' => item&.build_beta_detail&.external_build_state,
        'appStoreState' => version&.app_store_state, 'appVersionState' => version&.app_version_state,
        'releaseType' => version&.release_type, 'observedAt' => Time.now.utc.iso8601,
        'deviceAcceptance' => 'not_verified' }
    end

    def upload(ipa)
      AegisRelease.require!(build.nil?, 'ASC already contains this version/build; inspect status instead of uploading again')
      @fastlane.upload_to_testflight(api_key: @api_key, app_identifier: APP_ID, ipa: ipa,
                                    skip_submission: true, skip_waiting_for_build_processing: true,
                                    distribute_external: false)
      { 'status' => 'uploaded_processing_unverified', 'appId' => @app.id, 'reviewSubmitted' => false }
    end

    def distribute
      item = ready_build
      selected = groups # pilot silently ignores unknown group names, so validate first.
      external = @request.destination == 'testflight-external'
      @fastlane.upload_to_testflight(api_key: @api_key, app_identifier: APP_ID,
                                    distribute_only: true, app_version: @request.version, build_number: @request.build,
                                    groups: selected.map(&:name), distribute_external: external,
                                    submit_beta_review: external, notify_external_testers: false)
      assigned = selected.all? { |group| group.fetch_builds.any? { |b| b.id == item.id } }
      AegisRelease.require!(assigned, 'TestFlight group assignment not confirmed; inspect status before retrying')
      status.merge('status' => 'testflight_groups_assigned', 'groups' => selected.map(&:name),
                   'betaReviewRequested' => external, 'testerAvailability' => 'check_internal_or_external_state')
    end

    def submit
      ready_build
      # Store listing, privacy, review contact and compliance are operator-owned.
      # Do not fabricate answers, change version trains or overwrite listing data.
      version = @app.get_edit_app_store_version(platform: 'IOS')
      AegisRelease.require!(version && version.version_string == @request.version, 'Prepare this exact App Store version and its metadata in ASC before submitting')
      AegisRelease.require!(!@app.get_in_progress_review_submission(platform: 'IOS'), 'Another App Review is already in progress')
      # deliver skips release-type changes when skip_metadata is true.
      version.update(attributes: { releaseType: 'MANUAL' }) unless version.release_type == 'MANUAL'
      @fastlane.upload_to_app_store(api_key: @api_key, app_identifier: APP_ID,
                                   app_version: @request.version, build_number: @request.build,
                                   skip_binary_upload: true, skip_metadata: true, skip_screenshots: true,
                                   submit_for_review: true, automatic_release: false, force: true,
                                   run_precheck_before_submit: true, precheck_include_in_app_purchases: false)
      result = status
      AegisRelease.require!(%w[WAITING_FOR_REVIEW IN_REVIEW].include?(result['appVersionState']) ||
                            %w[WAITING_FOR_REVIEW IN_REVIEW].include?(result['appStoreState']),
                            'App Review submission has not been confirmed; inspect ASC')
      AegisRelease.require!(result['releaseType'] == 'MANUAL', 'App Store manual release policy was not confirmed')
      result.merge('status' => 'app_review_submitted')
    end
  end

  class PlayStore
    def initialize(request, _fastlane)
      @request = request
    end

    def authenticate
      require 'supply'
      path = AegisRelease.file!(File.realpath(ENV.fetch('AEGIS_PLAY_JSON_KEY')))
      AegisRelease.require!((File.stat(path).mode & 0o077).zero?, 'Play credentials file must be private (mode 600)')
      Supply.config = FastlaneCore::Configuration.create(Supply::Options.available_options,
        json_key: path, package_name: APP_ID, rescue_changes_not_sent_for_review: false)
      @client = Supply::Client.make_from_config(params: Supply.config)
    end

    def edit
      @client.begin_edit(package_name: APP_ID)
      yield
    ensure
      @client.abort_current_edit if @client.current_edit
    end

    def releases
      @client.track_releases(@request.track)
    end

    def exact_release
      rows = releases.select { |r| (r.version_codes || []).map(&:to_s).include?(@request.build) }
      AegisRelease.require!(rows.size == 1, 'Expected exactly one Play release containing the requested versionCode')
      AegisRelease.require!(rows.first.version_codes.map(&:to_s) == [@request.build], 'Refusing to change a release containing other versionCodes')
      rows.first
    end

    def status
      edit do
        rows = releases.select { |r| (r.version_codes || []).map(&:to_s).include?(@request.build) }
        { 'track' => @request.track, 'releases' => rows.map { |r| { 'versionCodes' => r.version_codes.map(&:to_s), 'status' => r.status, 'name' => r.name } },
          'reviewState' => 'not_exposed_by_edits_api', 'observedAt' => Time.now.utc.iso8601,
          'testerAvailability' => 'not_verified', 'deviceAcceptance' => 'not_verified' }
      end
    end

    def upload(aab)
      edit do
        used = @client.aab_version_codes + @client.apks_version_codes
        AegisRelease.require!(!used.map(&:to_s).include?(@request.build), 'Play already has this versionCode; inspect status instead of re-uploading')
        existing = releases
        AegisRelease.require!(existing.none? { |r| r.status == 'draft' }, 'Track already has a draft; resolve it in Play before creating another')
        uploaded = @client.upload_bundle(aab)
        AegisRelease.require!(uploaded.to_s == @request.build, 'Uploaded Play versionCode differs from the manifest')
        bundles = @client.client.list_edit_bundles(APP_ID, @client.current_edit.id).bundles || []
        remote = bundles.find { |b| b.version_code.to_s == @request.build }
        AegisRelease.require!(remote&.sha256&.downcase == AegisRelease.digest(aab), 'Play AAB SHA-256 readback mismatch')
        release = ::AndroidPublisher::TrackRelease.new(name: "#{@request.profile} #{@request.version} (#{@request.build})",
                                                             status: 'draft', version_codes: [@request.build])
        commit(existing + [release], review: false)
      end
      result = status
      AegisRelease.require!(result['releases'].any? { |r| r['status'] == 'draft' }, 'Play draft readback missing; inspect Play before retrying')
      result.merge('status' => 'play_draft_uploaded', 'reviewSubmitted' => false)
    end

    def commit(rows, review:)
      track = ::AndroidPublisher::Track.new(track: @request.track, releases: rows)
      @client.update_track(@request.track, track)
      @client.validate_current_edit!
      # The API now defaults to cancelling an existing review. The Fastlane
      # wrapper does not expose this option, so use its authenticated SDK client.
      @client.client.commit_edit(APP_ID, @client.current_edit.id,
                                 changes_not_sent_for_review: !review,
                                 changes_in_review_behavior: 'ERROR_IF_IN_REVIEW')
      @client.current_edit = nil
      @client.current_package_name = nil
    end

    def distribute
      edit do
        item = exact_release
        AegisRelease.require!(item.status == 'draft', 'Expected a draft Play release before distribution/submission')
        # Completed replaces the previous completed release on this same track.
        # Never alter another active rollout or another draft implicitly.
        others = releases.reject { |r| (r.version_codes || []).map(&:to_s).include?(@request.build) }
        AegisRelease.require!(others.all? { |r| r.status == 'completed' }, 'Track has another active rollout/draft; resolve it before this stage')
        item.status = 'completed'
        commit([item], review: true)
      end
      result = status
      AegisRelease.require!(result['releases'].any? { |r| r['status'] == 'completed' }, 'Play track transition not confirmed')
      result.merge('status' => 'play_track_committed_review_requested',
                   'publishingPolicy' => @request.play_release_policy, 'reviewApproval' => 'not_verified')
    end
  end
end
