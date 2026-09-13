"""Build ownership and failure boundaries for the diagnostic adapter."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import run


class SelectedBuildTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name).resolve() / "selected build"
        self.source = self.build.parent / "archived source"
        self.build.mkdir()
        self.adapter = self.source / "experiments/incremental/probe.c"
        self.adapter.parent.mkdir(parents=True)
        self.adapter.write_text("/* adapter from the archived revision */\n")
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.source}\n"
            "CMAKE_C_COMPILER:FILEPATH=/selected/toolchain/cc\n")

    def test_selected_graph_target_supports_make_and_ninja(self):
        for targets in ("... incremental_probe\n", "incremental_probe: phony\n"):
            with (
                self.subTest(targets=targets),
                patch.object(run.subprocess, "check_output", return_value=targets) as query,
                patch.object(run.subprocess, "run") as command,
            ):
                probe, source, compiler = run.prepare_probe(self.build)
                query.assert_called_once_with(
                    ["cmake", "--build", str(self.build), "--target", "help"], text=True)
                command.assert_called_once_with(
                    ["cmake", "--build", str(self.build), "--target", "incremental_probe"],
                    check=True, capture_output=True)
                self.assertEqual(probe.parent, self.build)
                self.assertEqual(source, self.source)
                self.assertEqual(compiler, "/selected/toolchain/cc")

    def test_missing_target_uses_selected_source_and_compiler(self):
        # Source text can define the target while the selected graph disables
        # it; a similarly named target must not be mistaken for this one.
        cmake = self.source / "packages/markdown-core/tests/CMakeLists.txt"
        cmake.parent.mkdir(parents=True)
        cmake.write_text("add_library(incremental_probe SHARED probe.c)\n")
        with (
            patch.object(run.subprocess, "check_output", return_value="... incremental_probe_test\n"),
            patch.object(run.subprocess, "run") as command,
        ):
            probe, source, _ = run.prepare_probe(self.build)
            args = command.call_args.args[0]
            self.assertEqual(args[0], "/selected/toolchain/cc")
            self.assertIn(str(self.adapter), args)
            for directory in ("include", "core", "elements"):
                self.assertIn("-I" + str(source / "packages/markdown-core" / directory), args)
            self.assertIn("-I" + str(self.build / "packages/markdown-core/core"), args)
            self.assertIn(str(self.build / "packages/markdown-core/elements/libmarkdown-core.a"), args)
            self.assertEqual(args[-2:], ["-o", str(probe)])

    def test_product_archive_cannot_supply_diagnostic_layout(self):
        header = self.source / "packages/markdown-core/core/diagnostics.h"
        header.parent.mkdir(parents=True)
        header.write_text("/* compile-time diagnostic layout */\n")
        with (
            patch.object(run.subprocess, "check_output", return_value="... all\n"),
            patch.object(run.subprocess, "run") as command,
        ):
            with self.assertRaisesRegex(RuntimeError, "MARKDOWN_CORE_TESTS=ON"):
                run.prepare_probe(self.build)
            command.assert_not_called()

    def test_existing_target_failure_is_not_hidden_by_fallback(self):
        with (
            patch.object(run.subprocess, "check_output", return_value="... incremental_probe\n"),
            patch.object(run.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "cmake")) as command,
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                run.prepare_probe(self.build)
            self.assertEqual(command.call_count, 1)

    def test_no_current_checkout_adapter_for_incompatible_archive(self):
        self.adapter.unlink()
        with (
            patch.object(run.subprocess, "check_output", return_value="... all\n"),
            patch.object(run.subprocess, "run") as command,
        ):
            with self.assertRaisesRegex(RuntimeError, "no compatible experiment adapter"):
                run.prepare_probe(self.build)
            command.assert_not_called()

    def test_failed_graph_query_is_not_treated_as_missing_target(self):
        with (
            patch.object(run.subprocess, "check_output", side_effect=subprocess.CalledProcessError(1, "cmake")),
            patch.object(run.subprocess, "run") as command,
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                run.prepare_probe(self.build)
            command.assert_not_called()

    def test_archive_has_no_borrowed_git_revision(self):
        with patch.object(run.subprocess, "check_output", side_effect=subprocess.CalledProcessError(128, "git")):
            self.assertEqual(run.source_revision(self.source), (None, None))
        with patch.object(run.subprocess, "check_output", return_value=str(self.source.parent)):
            self.assertEqual(run.source_revision(self.source), (None, None))

    def test_revision_is_queried_in_selected_source(self):
        revision = "a" * 40
        with patch.object(run.subprocess, "check_output", side_effect=[
            str(self.source), revision, " M packages/markdown-core/core/map.c\n",
        ]) as query:
            self.assertEqual(run.source_revision(self.source), (revision, True))
            self.assertTrue(all(call.kwargs["cwd"] == self.source for call in query.call_args_list))


if __name__ == "__main__":
    unittest.main()
