import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HOOK = ROOT / "scripts" / "update_xtrader_build_info.sh"
VERSION = (ROOT / "apps" / "xtrader" / "VERSION").read_text().strip()


class BuildInfoTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xtrader-build-info-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def update(self, mode="debug", check=True):
        return subprocess.run(
            ["bash", str(HOOK), mode, str(self.root / mode / "apps" / "xtrader"),
             str(self.root / (mode + "-build-info"))],
            check=check, capture_output=True, text=True, timeout=30,
        )

    def artifacts(self, mode="debug"):
        directory = self.root / mode / "apps" / "xtrader"
        directory.mkdir(parents=True)
        return directory

    def record(self, mode="debug"):
        return self.root / (mode + "-build-info") / "xtrader-build-info.txt"

    def test_artifact_changes_and_idempotent_creation(self):
        artifacts = self.artifacts()
        (artifacts / "xtrader_cli").write_bytes(b"executable")
        library = artifacts / "extension.so"
        library.write_bytes(b"library")
        self.assertFalse(self.record().parent.exists())
        self.assertEqual(self.update().stdout.strip(), VERSION + ".1.debug")
        original = self.record().read_bytes()
        internal = artifacts / "CMakeFiles"
        internal.mkdir()
        (internal / "object.o").write_bytes(b"not a final artifact")
        self.assertEqual(self.update().stdout.strip(), VERSION + ".1.debug")
        self.assertEqual(self.record().read_bytes(), original)
        library.write_bytes(b"changed library")
        self.assertEqual(self.update().stdout.strip(), VERSION + ".2.debug")
        library.rename(artifacts / "renamed.so")
        self.assertEqual(self.update().stdout.strip(), VERSION + ".3.debug")
        (artifacts / "renamed.so").unlink()
        self.assertEqual(self.update().stdout.strip(), VERSION + ".4.debug")
        release = self.artifacts("release")
        (release / "xtrader_cli").write_bytes(b"release executable")
        self.assertEqual(self.update("release").stdout.strip(), VERSION + ".1.release")

    def test_history_keeps_exactly_500_latest_entries(self):
        artifacts = self.artifacts()
        (artifacts / "xtrader_cli").write_bytes(b"executable")
        self.update()
        history = "".join(
            f"{serial}\t{'0' * 64}\t2026-11-19T00:00:00Z\n"
            for serial in range(1, 501)
        )
        self.record().write_text(
            f"static_version={VERSION}\nvariant=debug\ncurrent_serial=500\n"
            f"latest_sha256={'0' * 64}\n[history]\n{history}"
        )
        self.assertEqual(self.update().stdout.strip(), VERSION + ".501.debug")
        rows = self.record().read_text().split("[history]\n", 1)[1].splitlines()
        self.assertEqual(len(rows), 500)
        self.assertTrue(rows[0].startswith("2\t"))
        self.assertTrue(rows[-1].startswith("501\t"))

    def test_empty_directory_does_not_create_success_record(self):
        self.artifacts()
        self.assertNotEqual(self.update(check=False).returncode, 0)
        self.assertFalse(self.record().exists())

    def test_header_is_standalone_and_runtime_uses_matching_mode(self):
        template = (ROOT / "apps" / "xtrader" / "version.hh.in").read_text()
        major, minor, feature = VERSION.split(".")
        for mode, build_type in (("debug", "Debug"), ("release", "RelWithDebInfo")):
            with self.subTest(mode=mode):
                artifacts = self.artifacts(mode)
                header = self.root / ("version-" + mode + ".hh")
                contents = template
                for key, value in {
                    "XTRADER_STATIC_VERSION": VERSION, "XTRADER_VERSION_MAJOR": major,
                    "XTRADER_VERSION_MINOR": minor, "XTRADER_VERSION_FEATURE": feature,
                    "CMAKE_BUILD_TYPE": build_type, "GIT_COMMIT_HASH": "test",
                    "GIT_BRANCH": "test",
                }.items():
                    contents = contents.replace("@" + key + "@", value)
                header.write_text(contents)
                source = self.root / ("main-" + mode + ".cc")
                source.write_text(
                    f'#include "{header.name}"\n'
                    'int main() { seastar::xtrader::print_version(); }\n'
                )
                binary = artifacts / "xtrader_version_test"
                subprocess.run(
                    ["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                     str(source), "-o", str(binary)], check=True, timeout=60,
                )
                self.update(mode)
                original = self.record(mode).read_bytes()
                link = self.root / ("linked-" + mode)
                link.symlink_to(binary)
                result = subprocess.run(
                    [str(link)], cwd="/", capture_output=True, text=True,
                    check=True, timeout=5,
                )
                self.assertIn(VERSION + ".1." + mode, result.stdout)
                self.assertEqual(result.stderr, "")
                self.assertEqual(self.record(mode).read_bytes(), original)
                self.record(mode).write_bytes(
                    original.replace(b"static_version=" + VERSION.encode(),
                                     b"static_version=99.99.99")
                )
                mismatch = subprocess.run(
                    [str(binary)], capture_output=True, text=True, check=True, timeout=5,
                )
                self.assertIn(VERSION + ".unknown." + mode, mismatch.stdout)
                self.assertIn("[WARN]", mismatch.stderr)
                self.record(mode).unlink()
                missing = subprocess.run(
                    [str(binary)], capture_output=True, text=True, check=True, timeout=5,
                )
                self.assertIn(VERSION + ".unknown." + mode, missing.stdout)
                self.assertIn("[WARN]", missing.stderr)
                outside = self.root / ("outside-" + mode)
                outside.write_bytes(binary.read_bytes())
                outside.chmod(0o755)
                result = subprocess.run(
                    [str(outside)], capture_output=True, text=True, check=True, timeout=5,
                )
                self.assertIn("[WARN]", result.stderr)


if __name__ == "__main__":
    unittest.main()
