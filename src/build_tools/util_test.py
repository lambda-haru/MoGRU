# -*- coding: utf-8 -*-
# Copyright 2010-2021, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import os
import stat
import tempfile
import unittest
import zipfile

from build_tools import util


class UtilTest(unittest.TestCase):

  def test_extract_zip_symlinks(self):
    with tempfile.TemporaryDirectory() as td:
      zip_path = os.path.join(td, 'test.zip')
      extract_dir = os.path.join(td, 'extracted')

      zinfo_file = zipfile.ZipInfo('test.txt')
      zinfo_file.external_attr = 0o755 << 16
      zinfo_sym = zipfile.ZipInfo('test_link')
      zinfo_sym.external_attr = (stat.S_IFLNK | 0o777) << 16

      with zipfile.ZipFile(zip_path, 'w') as zf:
        zf.writestr(zinfo_file, 'echo hello')
        zf.writestr(
            zinfo_sym,
            '/Library/Input Methods/MoGRU.app/Contents/Resources/ConfigDialog.app',
        )

      util.ExtractZip(zip_path, extract_dir)

      file_path = os.path.join(extract_dir, 'test.txt')
      self.assertTrue(os.path.isfile(file_path))

      if not util.IsWindows():
        self.assertEqual(os.stat(file_path).st_mode & 0o777, 0o755)
        sym_path = os.path.join(extract_dir, 'test_link')
        self.assertTrue(os.path.islink(sym_path))
        self.assertEqual(
            os.readlink(sym_path),
            '/Library/Input Methods/MoGRU.app/Contents/Resources/ConfigDialog.app',
        )


if __name__ == '__main__':
  unittest.main()
