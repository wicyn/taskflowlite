from contextlib import redirect_stdout
import io
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

import check_cpp


class ChangedFilesTest(unittest.TestCase):
    def test_push_uses_before_and_after(self):
        with patch.object(check_cpp, "git", return_value=b"examples/space name.cpp\0") as git, \
                patch.object(check_cpp.subprocess, "run") as run:
            run.return_value.returncode = 0
            self.assertEqual(check_cpp.changed_files("push", {"before": "abc", "after": "def"}),
                             ["examples/space name.cpp"])
            git.assert_called_once_with("diff", "--name-only", "--diff-filter=ACMR",
                                        "-z", "abc", "def", "--")

    def test_force_push_missing_before_checks_all_files(self):
        with patch.object(check_cpp, "git", return_value=b"test/a.cpp\0") as git, \
                patch.object(check_cpp.subprocess, "run") as run:
            run.return_value.returncode = 1
            self.assertEqual(check_cpp.changed_files("push", {"before": "abc", "after": "def"}),
                             ["test/a.cpp"])
            git.assert_called_once_with("ls-files", "-z")

    def test_pull_request_uses_merge_base(self):
        with patch.object(check_cpp, "git", side_effect=[b"common\n", b"test/a.hpp\0"]) as git:
            event = {"pull_request": {"base": {"sha": "base"}, "head": {"sha": "head"}}}
            self.assertEqual(check_cpp.changed_files("pull_request", event), ["test/a.hpp"])
            self.assertEqual(git.call_args_list[1].args[-3:], ("common", "head", "--"))

    def test_initial_push_and_manual_run_check_tracked_files(self):
        for kind, event in [("push", {"before": "0" * 40}), ("workflow_dispatch", {})]:
            with self.subTest(kind=kind), patch.object(check_cpp, "git", return_value=b"") as git:
                self.assertEqual(check_cpp.changed_files(kind, event), [])
                git.assert_called_once_with("ls-files", "-z")

    def test_paths_preserve_spaces_and_newlines(self):
        self.assertEqual(check_cpp.paths(b"test/a b.cpp\0test/a\nb.hpp\0"),
                         ["test/a b.cpp", "test/a\nb.hpp"])

    def test_project_sources_exclude_vendored_files(self):
        for name in ["test/a.hpp", "benchmarks/bench_common.hpp", "taskflowlite/core/work.hpp"]:
            self.assertTrue(check_cpp.is_source(name))
        for name in ["README.md", "build/a.cpp", "benchmarks/taskflow/core/a.hpp",
                     "test/catch2/catch_amalgamated.cpp"]:
            self.assertFalse(check_cpp.is_source(name))


class FormatCheckTest(unittest.TestCase):
    def test_style_change_does_not_expand_edited_sources(self):
        existing = {"taskflowlite/core/work.hpp", "taskflowlite/core/task.hpp"}
        self.assertEqual(check_cpp.select_format_files(
            [".clang-format", "taskflowlite/core/work.hpp"], existing),
            ["taskflowlite/core/work.hpp"])

    def test_config_only_change_has_no_source_rewrites(self):
        self.assertEqual(check_cpp.select_format_files(
            [".clang-format"], {"taskflowlite/core/work.hpp"}), [])

    def test_manual_selection_still_includes_all_project_sources(self):
        tracked = {"taskflowlite/core/work.hpp", "examples/a.cpp", "README.md"}
        self.assertEqual(check_cpp.select_format_files(tracked, tracked),
                         ["examples/a.cpp", "taskflowlite/core/work.hpp"])

    def test_selection_excludes_missing_vendor_and_duplicate_paths(self):
        existing = {"test/a.hpp", "benchmarks/taskflow/core/a.hpp"}
        self.assertEqual(check_cpp.select_format_files(
            ["test/a.hpp", "test/a.hpp", "test/deleted.cpp",
             "benchmarks/taskflow/core/a.hpp"], existing), ["test/a.hpp"])

    def test_config_is_validated_even_without_selected_sources(self):
        with patch.object(check_cpp, "git", return_value=b""), \
                patch.object(check_cpp.subprocess, "run") as run, \
                redirect_stdout(io.StringIO()):
            check_cpp.check("format", [".clang-format"], Path("build"))
            run.assert_called_once()
            self.assertIn("--dump-config", run.call_args.args[0])
            self.assertTrue(run.call_args.kwargs["check"])

    def test_invalid_style_fails_even_without_selected_sources(self):
        with patch.object(check_cpp, "git", return_value=b""), \
                patch.object(check_cpp.subprocess, "run",
                             side_effect=subprocess.CalledProcessError(1, "clang-format-18")):
            with self.assertRaises(subprocess.CalledProcessError):
                check_cpp.check("format", [".clang-format"], Path("build"))

    def test_reports_all_failed_files_before_exiting(self):
        files = ["taskflowlite/core/async_task.hpp", "taskflowlite/core/task.hpp"]
        output = io.StringIO()
        results = [subprocess.CompletedProcess([], code) for code in [0, 1, 1]]
        with patch.object(check_cpp, "git", return_value="\0".join(files).encode()), \
                patch.object(check_cpp.subprocess, "run", side_effect=results) as run, \
                redirect_stdout(output):
            with self.assertRaises(SystemExit) as error:
                check_cpp.check("format", files, Path("build"))
            self.assertEqual(run.call_count, 3)
            for name in files:
                self.assertIn(name, str(error.exception))
                self.assertIn(name, output.getvalue())


if __name__ == "__main__":
    unittest.main()
