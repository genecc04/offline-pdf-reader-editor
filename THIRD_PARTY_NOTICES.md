# Third-Party Notices

This project uses the following third-party software.

## Qt 6 (Core, Gui, Widgets, Pdf, PdfWidgets)

- Project: https://www.qt.io
- License: used under the GNU Lesser General Public License v3.0
  (see licenses/LGPL-3.0.txt and licenses/GPL-3.0.txt).
  Qt is also available under GPL and commercial terms: https://www.qt.io/licensing/
- Qt is linked dynamically. Qt's source code for each release is available
  from https://download.qt.io/ (choose the version you build against).
- The Qt PDF module includes PDFium and other third-party code under their
  own licenses: https://doc.qt.io/qt-6/qtpdf-licensing.html

## MuPDF

- Project: https://mupdf.com (Artifex Software, Inc.)
- License: GNU Affero General Public License v3.0
  (see licenses/AGPL-3.0.txt and licenses/vcpkg/libmupdf-copyright.txt)
- Used for: saving PDFs and PDF editing (in development)
- Linked statically.

## Libraries installed with MuPDF through vcpkg

MuPDF is built with vcpkg, which also builds and links its dependencies
(FreeType, HarfBuzz, libjpeg-turbo, OpenJPEG, zlib, Gumbo, jbig2dec, and
related libraries). Each has its own license. The copyright and license
files that vcpkg installed for them are in licenses/vcpkg/.