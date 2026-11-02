import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('native_control', Path(__file__).with_name('native_control.py'))
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class InputBoundary(unittest.TestCase):
    def archive(self, root, name='chromium/src/input.ts', link=False):
        path=root/'inputs.tar.gz'
        with tarfile.open(path,'w:gz') as tar:
            member=tarfile.TarInfo(name)
            if link:
                member.type=tarfile.SYMTYPE;member.linkname='/outside'
                tar.addfile(member)
            else:
                member.size=4;tar.addfile(member,io.BytesIO(b'test'))
        return path

    def test_exact_hash_and_regular_relative_inputs_extract(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);archive=self.archive(root);target=root/'target';target.mkdir()
            control.extract_inputs(archive,target,hashlib.sha256(archive.read_bytes()).hexdigest())
            self.assertEqual((target/'chromium/src/input.ts').read_bytes(),b'test')

    def test_changed_archive_refuses_before_any_extraction(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);archive=self.archive(root);target=root/'target';target.mkdir()
            with self.assertRaisesRegex(ValueError,'INPUT_ARCHIVE_HASH'):
                control.extract_inputs(archive,target,'0'*64)
            self.assertEqual(list(target.iterdir()),[])

    def test_absolute_traversal_symlink_or_unexpected_prefix_refuse(self):
        for name,link in [('/outside',False),('chromium/src/../../outside',False),('chromium/src/a',True),('credentials.json',False)]:
            with self.subTest(name=name,link=link),tempfile.TemporaryDirectory() as temp:
                root=Path(temp);archive=self.archive(root,name,link);target=root/'target';target.mkdir()
                with self.assertRaisesRegex(ValueError,'INPUT_MEMBER'):
                    control.extract_inputs(archive,target,hashlib.sha256(archive.read_bytes()).hexdigest())
                self.assertEqual(list(target.iterdir()),[])

    def test_minimal_compiler_environment_does_not_forward_tokens_or_gc_overrides(self):
        value=control.compiler_environment()
        self.assertEqual(value['GOMAXPROCS'],'1')
        self.assertEqual(value['QEMU_RESERVED_VA'],'1099511627776')
        self.assertNotIn('GITHUB_TOKEN',value)
        self.assertNotIn('ACTIONS_RUNTIME_TOKEN',value)
        self.assertNotIn('GODEBUG',value)
        self.assertNotIn('GOGC',value)


if __name__=='__main__':unittest.main()
