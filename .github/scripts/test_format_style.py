"""Readability regressions for the project's clang-format-18 configuration."""

import os
from pathlib import Path
import re
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / ".github/tests/format/style.cpp"


class FormatStyleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = os.environ.get("CLANG_FORMAT", "clang-format-18")
        version = subprocess.run([cls.tool, "--version"], check=True,
                                 capture_output=True, text=True).stdout
        if not re.search(r"\bversion 18\.", version):
            raise RuntimeError("Style tests require clang-format 18; set CLANG_FORMAT if needed")
        cls.source = FIXTURE.read_text(encoding="utf-8")
        cls.formatted = cls.format_text(cls.source)

    @classmethod
    def format_text(cls, source):
        return subprocess.run(
            [cls.tool, "--style=file:" + str(ROOT / ".clang-format"),
             "--assume-filename=" + str(FIXTURE)],
            input=source, capture_output=True, text=True, encoding="utf-8", check=True,
        ).stdout

    def test_fixture_is_canonical_and_idempotent(self):
        self.assertEqual(self.formatted, self.source)
        self.assertEqual(self.format_text(self.formatted), self.formatted)

    def test_chinese_doxygen_comment_is_not_reflowed(self):
        comment = next(line for line in self.source.splitlines() if "超过一百列" in line)
        self.assertGreater(len(comment), 100)
        self.assertIn(comment, self.formatted.splitlines())

    def test_member_names_and_trailing_comments_are_aligned(self):
        lines = [line for line in self.formatted.splitlines() if "///<" in line]
        self.assertEqual(len(lines), 3)
        self.assertEqual(len({re.search(r"\bm_\w+", line).start() for line in lines}), 1)
        self.assertEqual(len({line.index("///<") for line in lines}), 1)
        self.assertTrue(any("m_count{0};" in line for line in lines))
        self.assertTrue(any("m_payload{nullptr};" in line for line in lines))

    def test_dependent_requires_clause_stays_on_its_own_line(self):
        self.assertIn("        requires (Payload::template valid_type<T>)\n"
                      "    [[nodiscard]] T& target(T& value) noexcept {\n", self.formatted)

    def test_nonempty_functions_remain_multiline(self):
        self.assertIn("bool empty() const noexcept {\n"
                      "        return m_count.load(std::memory_order_relaxed) == 0;\n"
                      "    }", self.formatted)

    def test_include_and_using_order_is_preserved(self):
        self.assertLess(self.formatted.index("#include <utility>"),
                        self.formatted.index("#include <atomic>"))
        self.assertLess(self.formatted.index("using Unsigned"),
                        self.formatted.index("using Integer"))

    def test_bad_indentation_still_fails_strict_check(self):
        result = subprocess.run(
            [self.tool, "--style=file:" + str(ROOT / ".clang-format"),
             "--assume-filename=" + str(FIXTURE), "--dry-run", "--Werror"],
            input="int broken( ){return 1;}\n", capture_output=True,
            text=True, encoding="utf-8",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("code should be clang-formatted", result.stderr)


if __name__ == "__main__":
    unittest.main()
