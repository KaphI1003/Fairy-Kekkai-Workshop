"""Archive the exact Workshop sources used to build a Windows installer."""

import argparse
from pathlib import Path, PurePosixPath
import subprocess
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def git(path, *arguments):
    return subprocess.check_output(["git", "-C", str(path), *arguments])


def files(path):
    listing = git(path, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
    return sorted(set(listing.decode("utf-8").split("\0")) - {""})


def archive_files(archive, root, prefix=""):
    for name in files(root):
        entry = root / name
        if entry.is_symlink() or not entry.is_file():
            continue
        if name.startswith("tools/") and entry.suffix.lower() in {".dll", ".exe"}:
            continue
        archive.write(entry, PurePosixPath(prefix, name))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    submodule = "third_party/Qt-Fluent-Widgets"
    if not (ROOT / submodule / "CMakeLists.txt").is_file():
        parser.error("Initialize the Qt-Fluent-Widgets submodule before archiving.")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        archive_files(archive, ROOT)
        archive_files(archive, ROOT / submodule, submodule)
        archive.writestr("SOURCE-REVISIONS.txt", git(ROOT, "rev-parse", "HEAD") +
                         git(ROOT, "submodule", "status"))


if __name__ == "__main__":
    main()
