import io
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path
import tempfile
import unittest
import warnings
import zipfile

import third_party_notices as notices


class NoticePackageTests(unittest.TestCase):
    def archive(self, *, missing=None, replaced=None, duplicate=None):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            for name, data in notices.ipa_members().items():
                if name != missing:
                    archive.writestr(name, (replaced or {}).get(name, data))
            if duplicate:
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    archive.writestr(duplicate, b"ambiguous alternate notice")
        return zipfile.ZipFile(io.BytesIO(buffer.getvalue()))

    def test_generated_files_match_current_licenses_and_swift_package_pins(self):
        notices.check_outputs()

    @unittest.skipUnless(shutil.which("swift"), "Swift is required to exercise the iOS notice parser")
    def test_ios_component_parser_preserves_every_section_and_byte(self):
        # Compile the actual Foundation-only model from the view file, avoiding
        # an Xcode project change or a second implementation of its parser.
        view = (notices.ROOT / "apps/ios/AegisApp/ThirdPartyNoticesView.swift").read_text()
        model = view.split("struct ThirdPartyNoticesDocument {", 1)[1].split(
            "\nstruct ThirdPartyNoticesView:", 1)[0]
        generated = notices.render("ios")
        expected_sources = re.findall(rb"^Source file: (.+)$", generated, re.MULTILINE)
        self.assertGreater(len(expected_sources), 0)
        program = "import Foundation\nstruct ThirdPartyNoticesDocument {" + model + r'''
let data = try Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1]))
let text = String(decoding: data, as: UTF8.self)
let document = ThirdPartyNoticesDocument(text: text)
precondition(Data(document.fullText.utf8) == data, "Full notice bytes changed")
precondition(document.notices.count == Int(CommandLine.arguments[2])!, "Missing generated sections")
precondition(document.introduction.contains("Third-party works retain their own licenses."))
for (index, notice) in document.notices.enumerated() {
    precondition(notice.id == index && !notice.title.isEmpty)
    precondition(notice.body.contains("Source file: "))
    precondition(notice.title + notice.body == notice.text)
    let paragraphs = notice.body.components(separatedBy: "\n\n")
    precondition(paragraphs.joined(separator: "\n\n") == notice.body)
}
// Internal license separators must remain inside their component.
let internalSeparators = "Title\nSource file: fixture\n\nLicense\n" + String(repeating: "=", count: 20)
    + "\nMore license\n" + String(repeating: "-", count: 72) + "\nEnd\n"
let fixture = "Introduction\n" + ThirdPartyNoticesDocument.sectionDelimiter + internalSeparators
let parsed = ThirdPartyNoticesDocument(text: fixture)
precondition(parsed.notices.count == 1)
precondition(parsed.notices[0].text == internalSeparators && parsed.fullText == fixture)
precondition(ThirdPartyNoticesDocument(text: "Introduction only\n").notices.isEmpty)
print("Validated \(document.notices.count) notice sections and \(data.count) original bytes")
'''
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            source = directory / "main.swift"
            source.write_text(program)
            resource = directory / "ThirdPartyNotices.txt"
            resource.write_bytes(generated)
            subprocess.run(["swift", str(source), str(resource), str(len(expected_sources))],
                           check=True, timeout=120)

    def test_new_sdk_pin_cannot_silently_reuse_old_notices(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            relative = "apps/ios/Aegis.xcodeproj/project.xcworkspace/xcshareddata/swiftpm/Package.resolved"
            path = root / relative
            path.parent.mkdir(parents=True)
            pins = json.loads(notices.read(notices.ROOT, relative))
            pins["pins"][0]["state"]["revision"] = "a" * 40
            path.write_text(json.dumps(pins))
            manifest = root / notices.SDK / "manifest.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_bytes(notices.read(notices.ROOT, f"{notices.SDK}/manifest.json"))
            with self.assertRaisesRegex(ValueError, "pins changed"):
                notices.sdk_records(root)

    def test_archive_preserves_all_required_license_resources(self):
        with self.archive() as archive:
            notices.verify_ipa(archive)

    def test_omitted_vendor_license_or_rule_notice_rejects_export(self):
        for name in notices.ipa_members():
            with self.subTest(name=name), self.archive(missing=name) as archive:
                with self.assertRaisesRegex(ValueError, "Missing IPA license resource"):
                    notices.verify_ipa(archive)

    def test_original_unlicense_only_phishing_notice_is_rejected(self):
        name = "Payload/Aegis.app/Frameworks/BrowserKit.framework/phishing-LICENSE.txt"
        with self.archive(replaced={name: b"This is free and unencumbered software released into the public domain."}) as archive:
            with self.assertRaisesRegex(ValueError, "differs from source"):
                notices.verify_ipa(archive)

    def test_duplicate_zip_names_do_not_hide_alternate_notices(self):
        with self.archive(duplicate="Payload/Aegis.app/ThirdPartyNotices.txt") as archive:
            with self.assertRaisesRegex(ValueError, "Ambiguous IPA members"):
                notices.verify_ipa(archive)

    def test_macos_app_copy_carries_the_complete_libtorrent_notice(self):
        with tempfile.TemporaryDirectory() as temp:
            app = Path(temp) / "Aegis.app"
            app.mkdir()
            notices.install_macos(app)
            actual = (app / "Contents/Resources/AegisThirdPartyNotices.txt").read_bytes()
            original = notices.read(notices.ROOT, "apps/browser/overlay/third_party/aegis_libtorrent/LICENSE")
            self.assertIn(original.rstrip(), actual)
            self.assertIn(b"Copyright (c) 2003-2020, Arvid Norberg", actual)
            self.assertIn(b"Boost Software License", actual)
            for name in ["TRY_SIGNAL_LICENSE.txt", "ED25519_LICENSE.txt", "UPSTREAM_LICENSE.txt", "APSL-2.0.txt", "SOURCE_OFFER.txt"]:
                original = notices.read(notices.ROOT, "apps/browser/overlay/third_party/aegis_libtorrent/" + name)
                self.assertIn(original.rstrip(), actual)
            source = notices.read(notices.ROOT, "apps/browser/overlay/third_party/aegis_libtorrent/UPSTREAM_ROUTE_H.txt")
            self.assertEqual(hashlib.sha256(source).hexdigest(), "3a32fc91cdb7ea12d0acc468072ae4e9de163939872ff277391032d8d4fc8929")
            self.assertEqual((app / "Contents/Resources/AegisLibtorrentRouteSource.txt").read_bytes(), source)
            self.assertIn(b"Apple Inc.", actual)
            self.assertIn(b"Berkeley", actual)

    def test_crashlytics_includes_the_libunwind_copyright(self):
        content = notices.render("ios")
        self.assertIn(b"Hewlett-Packard", content)
        original = notices.read(notices.ROOT, f"{notices.SDK}/firebase-crashlytics.txt")
        self.assertIn(original.rstrip(), content)

    def test_macos_notice_writer_does_not_follow_bundle_symlinks(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            app = directory / "Aegis.app"
            app.mkdir()
            outside = directory / "preserve"
            outside.mkdir()
            (outside / "marker").write_text("keep")
            (app / "Contents").symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "Symlink"):
                notices.install_macos(app)
            self.assertEqual(sorted(p.name for p in outside.iterdir()), ["marker"])


if __name__ == "__main__":
    unittest.main()
