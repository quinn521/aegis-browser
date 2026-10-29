# frozen_string_literal: true
require 'openssl'

module AegisRelease
  class Pipeline
    def verify_android_identity(path)
      AegisRelease.file!(path)
      version_name = File.read(File.join(request.root, 'apps/browser/args/aegis-android.gn'))[/android_override_version_name = "([^"]+)"/, 1]
      if request.format == 'aab'
        tool = AegisRelease.file!(File.expand_path(required('AEGIS_BUNDLETOOL_JAR')))
        values = %w[package android:versionCode android:versionName].map do |attribute|
          AegisRelease.command('java', '-jar', tool, 'dump', 'manifest', "--bundle=#{path}", "--xpath=/manifest/@#{attribute}")
        end
        AegisRelease.require!(values == [APP_ID, request.build, version_name], 'AAB applicationId/versionCode/versionName differs from committed inputs')
      else
        badging = AegisRelease.command(ENV.fetch('AEGIS_AAPT2', 'aapt2'), 'dump', 'badging', path)
        AegisRelease.require!(badging.match?(/^package: name='#{Regexp.escape(APP_ID)}' versionCode='#{request.build}' /), 'APK identity mismatch')
        AegisRelease.require!(version_name && badging.lines.first.include?("versionName='#{version_name}'"), 'APK versionName differs from committed inputs')
        run(ENV.fetch('AEGIS_APKSIGNER', 'apksigner'), 'verify', path)
      end
    end

    def sign_android_bundle(source, target)
      keystore = AegisRelease.file!(File.expand_path(required('AEGIS_ANDROID_KEYSTORE')))
      AegisRelease.require!((File.stat(keystore).mode & 0o077).zero?, 'Upload keystore must be private (mode 600)')
      key_alias = required('AEGIS_ANDROID_KEY_ALIAS')
      %w[AEGIS_ANDROID_STORE_PASSWORD AEGIS_ANDROID_KEY_PASSWORD].each { |key| required(key) }
      fingerprint = required('AEGIS_ANDROID_CERT_SHA256').delete(':').downcase
      AegisRelease.require!(/\A[0-9a-f]{64}\z/.match?(fingerprint), 'Invalid upload certificate SHA-256')
      certificate = AegisRelease.command('keytool', '-exportcert', '-rfc', '-keystore', keystore, '-alias', key_alias,
                                         '-storepass:env', 'AEGIS_ANDROID_STORE_PASSWORD')
      actual = Digest::SHA256.hexdigest(OpenSSL::X509::Certificate.new(certificate).to_der)
      AegisRelease.require!(actual == fingerprint, 'Keystore certificate differs from expected Play upload certificate')
      # Strip only old JAR signatures from a new copy; never change build output.
      run('python3', File.join(request.root, 'scripts/release/unsigned-bundle.py'), source, target)
      run('jarsigner', '-keystore', keystore, '-storepass:env', 'AEGIS_ANDROID_STORE_PASSWORD',
          '-keypass:env', 'AEGIS_ANDROID_KEY_PASSWORD', '-digestalg', 'SHA-256', '-sigalg', 'SHA256withRSA', target, key_alias)
      run('java', File.join(request.root, 'scripts/release/VerifyAndroidBundle.java'), target, fingerprint)
      verify_android_identity(target)
    end
  end
end
