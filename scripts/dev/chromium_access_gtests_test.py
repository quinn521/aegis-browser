#!/usr/bin/env python3
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import chromium_access_gtests as sut


class ChromiumAccessGTestsRunnerTests(unittest.TestCase):
    def test_target_inventory_is_complete_and_unique(self):
        self.assertEqual(len(sut.TARGETS), 17)
        labels = [item[0] for item in sut.TARGETS]
        binaries = [item[2] for item in sut.TARGETS]
        self.assertEqual(len(labels), len(set(labels)))
        self.assertEqual(len(binaries), len(set(binaries)))
        self.assertIn(
            "//chrome/browser/aegis/access:access_service_coordinator_unittests",
            labels,
        )
        self.assertIn(
            "//chrome/browser/aegis/access:access_network_context_transport_unittests",
            labels,
        )
        self.assertIn("//components/aegis_access:aegis_access_unittests", labels)

    def test_count_gtests_counts_only_test_rows(self):
        listing = """SuiteOne.
  First
  Second
SuiteTwo/Variant.
  Third/0  # GetParam() = 1
"""
        self.assertEqual(sut.count_gtests(listing), 3)

    def test_series_rejects_duplicate_and_unsafe_paths(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            (directory / "a.patch").write_text("patch", encoding="utf-8")
            (directory / "series").write_text("a.patch\na.patch\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicated"):
                sut.series(directory)

            (directory / "series").write_text("../a.patch\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "unsafe"):
                sut.series(directory)

    def test_verify_tree_requires_exact_replayed_tree(self):
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            patches = directory / "patches"
            patches.mkdir()
            (patches / "series").write_text("a.patch\n", encoding="utf-8")
            (patches / "a.patch").write_text("patch", encoding="utf-8")

            def fake_git(_repo, *args, env=None):
                del env
                if args == ("status", "--porcelain"):
                    return ""
                if args == ("write-tree",):
                    return "expected-tree"
                if args == ("rev-parse", "HEAD^{tree}"):
                    return "expected-tree"
                return ""

            with mock.patch.object(sut, "git", side_effect=fake_git):
                self.assertEqual(
                    sut.verify_tree(directory, "base", patches),
                    "expected-tree",
                )

            def mismatched_git(_repo, *args, env=None):
                del env
                if args == ("write-tree",):
                    return "expected-tree"
                if args == ("rev-parse", "HEAD^{tree}"):
                    return "other-tree"
                return ""

            with mock.patch.object(sut, "git", side_effect=mismatched_git):
                with self.assertRaisesRegex(ValueError, "does not match"):
                    sut.verify_tree(directory, "base", patches)

    def test_run_target_builds_lists_and_executes_without_retry(self):
        with tempfile.TemporaryDirectory() as root:
            base = Path(root)
            src = base / "src"
            out = base / "out"
            report = base / "report"
            src.mkdir()
            out.mkdir()
            report.mkdir()

            autoninja = base / "autoninja"
            autoninja.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            autoninja.chmod(0o755)

            binary = out / "fake_unittests"
            trace = base / "trace.txt"
            binary.write_text(
                "#!/usr/bin/env python3\n"
                "import os, sys\n"
                "if '--gtest_list_tests' in sys.argv:\n"
                "    print('FakeSuite.')\n"
                "    print('  First')\n"
                "    print('  Second')\n"
                "    raise SystemExit(0)\n"
                "with open(os.environ['AEGIS_TEST_TRACE'], 'w', encoding='utf-8') as f:\n"
                "    f.write(' '.join(sys.argv[1:]))\n",
                encoding="utf-8",
            )
            binary.chmod(0o755)
            env = dict(os.environ, AEGIS_TEST_TRACE=str(trace))

            result = sut.run_target(
                ("//fake:fake_unittests", "fake:fake_unittests", "fake_unittests"),
                src=src,
                out=out,
                report=report,
                env=env,
                autoninja=str(autoninja),
                jobs=3,
            )

            self.assertEqual(result["tests"], 2)
            self.assertEqual(result["result"], "PASS")
            invocation = trace.read_text(encoding="utf-8")
            self.assertIn("--test-launcher-jobs=3", invocation)
            self.assertIn("--test-launcher-retry-limit=0", invocation)
            self.assertTrue((report / "fake_unittests.build.log").is_file())
            self.assertTrue((report / "fake_unittests.list.log").is_file())
            self.assertTrue((report / "fake_unittests.test.log").is_file())

    def test_acquire_lock_rejects_candidate_or_parallel_run(self):
        with tempfile.TemporaryDirectory() as root:
            parent = Path(root)
            src = parent / "src"
            src.mkdir()

            candidate = parent / ".aegis-ci-lock"
            candidate.mkdir()
            with self.assertRaisesRegex(ValueError, "candidate build"):
                sut.acquire_lock(src)
            candidate.rmdir()

            lock = sut.acquire_lock(src)
            try:
                with self.assertRaisesRegex(ValueError, "already active"):
                    sut.acquire_lock(src)
            finally:
                lock.rmdir()


if __name__ == "__main__":
    unittest.main()
