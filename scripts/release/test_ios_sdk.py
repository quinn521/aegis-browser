"""SDK policy regressions independent of the host Xcode installation."""
import unittest
from unittest.mock import patch

from ios_sdk import device_sdk, verify_toolchain


class IosSdkTests(unittest.TestCase):
    def test_toolchain_reports_the_xcode_build_and_the_device_sdk(self):
        with patch('subprocess.check_output', side_effect=['Xcode 27.2\nBuild version 27B52\n', '27.2\n']):
            self.assertEqual(verify_toolchain(), {'xcode': '27.2', 'xcodeBuild': '27B52', 'iphoneosSdk': '27.2'})

    def test_toolchain_fails_when_the_selected_xcode_or_device_sdk_differs(self):
        with patch('subprocess.check_output', return_value='Xcode 26.6\nBuild version 17F113\n'):
            with self.assertRaisesRegex(ValueError, 'Xcode 27.2 is required'):
                verify_toolchain()
        with patch('subprocess.check_output', side_effect=['Xcode 27.2\nBuild version 27B52\n', '26.5\n']):
            with self.assertRaisesRegex(ValueError, 'iPhoneOS SDK 27.2 is required'):
                verify_toolchain()

    def test_only_an_unambiguous_ios_device_command_qualifies(self):
        device = 'Load command 11\n      cmd LC_BUILD_VERSION\n  cmdsize 32\n platform IOS\n    minos 18.4\n      sdk 27.0\n'
        self.assertEqual(device_sdk(device, 'Aegis (arm64)'), '27.0')
        for invalid in (device.replace('IOS\n', 'IOSSIMULATOR\n'), device + device,
                        device.replace('LC_BUILD_VERSION', 'LC_VERSION_MIN_IPHONEOS'), ''):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, 'iOS device LC_BUILD_VERSION'):
                device_sdk(invalid, 'Aegis (arm64)')
