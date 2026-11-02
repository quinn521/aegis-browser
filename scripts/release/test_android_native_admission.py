import importlib.util
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('android_native_admission',Path(__file__).with_name('android-native-admission.py'))


class NativeAdmissionTests(unittest.TestCase):
    def test_skipped_native_checks_cannot_become_a_successful_admission(self):
        self.assertTrue(Path(spec.origin).exists(),'Native admission must reject unittest skips')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        class MissingRuntime(unittest.TestCase):
            @unittest.skip('No actual Linux ARM64 runtime')
            def test_loader(self): pass
        with self.assertRaisesRegex(ValueError,'skipped|native'):
            module.run_suite(unittest.defaultTestLoader.loadTestsFromTestCase(MissingRuntime))

    def test_empty_native_suite_cannot_become_a_successful_admission(self):
        self.assertTrue(Path(spec.origin).exists(),'Native admission must reject missing tests')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with self.assertRaises(ValueError):module.run_suite(unittest.TestSuite())


if __name__=='__main__': unittest.main()
