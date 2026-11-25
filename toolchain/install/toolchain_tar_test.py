#!/usr/bin/env python3

"""Check that a release tar contains the same files as a prefix root."""

__copyright__ = """
Part of the Carbon Language project, under the Apache License v2.0 with LLVM
Exceptions. See /LICENSE for license information.
SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""

import os
import re
import stat
import sys
import tarfile
import unittest
import zipfile
from pathlib import Path, PurePosixPath


class ToolchainTarTest(unittest.TestCase):
    def test_archive(self) -> None:
        install_data_manifest = Path(os.environ["INSTALL_DATA_MANIFEST"])

        # Gather install data files.
        with open(install_data_manifest) as manifest:
            # Remove everything up to and including the package path
            # `toolchain/install`.
            install_files = set(
                [
                    re.sub("^.*/toolchain/install/", "", entry.strip())
                    for entry in manifest.readlines()
                ]
            )
        self.assertTrue(install_files, f"`{install_data_manifest}` is empty.")

        if "ZIP_FILE" in os.environ:
            zip_file = Path(os.environ["ZIP_FILE"])
            archive_files = set()
            with zipfile.ZipFile(zip_file) as zf:
                for info in zf.infolist():
                    if info.is_dir():
                        continue
                    mode = info.external_attr >> 16
                    self.assertFalse(
                        stat.S_ISLNK(mode),
                        f"Unexpected symlink `{info.filename}` in `{zip_file}`",
                    )
                    parts = PurePosixPath(info.filename).parts[1:]
                    archive_files.add(PurePosixPath(*parts).as_posix())
            self.assertTrue(archive_files, f"`{zip_file}` is empty.")
        else:
            tar_file = Path(os.environ["TAR_FILE"])
            with tarfile.open(tar_file) as tar:
                # Remove the first path component.
                archive_files = set(
                    [
                        PurePosixPath(
                            *PurePosixPath(tarinfo.name).parts[1:]
                        ).as_posix()
                        for tarinfo in tar
                        if not tarinfo.isdir()
                    ]
                )
            self.assertTrue(archive_files, f"`{tar_file}` is empty.")

        # Check that the `carbon` entry is in the archive.
        expected_bin = (
            "bin/carbon.exe" if sys.platform == "win32" else "bin/carbon"
        )
        self.assertIn(expected_bin, archive_files)
        archive_files.remove(expected_bin)

        # Remove the `lib/carbon` prefix which should be on every other file.
        archive_files = set(
            [
                re.sub("^lib/carbon/", "", entry.strip())
                for entry in archive_files
            ]
        )

        # The install files and the archive files should now be identical.
        self.assertSetEqual(install_files, archive_files)


if __name__ == "__main__":
    unittest.main()
