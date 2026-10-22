# Third-Party Notices

This project uses the following third-party software.

## Qt 6 (Core, Gui, Widgets, Pdf, PdfWidgets)

- Project: https://www.qt.io
- Version used for the development build: Qt 6.8.0 (MSVC 2022 package).
- License: this project uses Qt under the GNU Lesser General Public License
  v3.0 (LGPL-3.0); see `licenses/LGPL-3.0.txt` and `licenses/GPL-3.0.txt`.
  Qt licensing information: https://www.qt.io/licensing/
- Qt is dynamically linked. The Windows release package keeps Qt DLLs as
  separate files beside the application so users can replace them with
  compatible modified versions. Do not add technical or license restrictions
  that prevent this or LGPL-permitted reverse engineering for debugging changes
  to the Qt libraries.
- Qt 6.8.0 source archives: https://download.qt.io/archive/qt/6.8/6.8.0/
- Qt PDF licensing and third-party notices:
  https://doc.qt.io/qt-6.8/qtpdf-licensing.html
  https://doc.qt.io/qt-6.8/qtpdf-3rdparty-pdfium.html
  https://doc.qt.io/qt-6.8/qtpdf-3rdparty-the-chromium-project.html
- Qt PDF includes PDFium and other third-party components with their own
  licenses. For binary distribution, verify and include the applicable notices
  and license texts for the exact Qt 6.8.0 package being shipped. The links
  above identify the upstream licensing information; this repository does not
  currently vendor the complete Qt PDF third-party notice set.

## MuPDF

- Project: https://mupdf.com (Artifex Software, Inc.)
- Version used for the development build: 1.26.10, installed through vcpkg
  with the `x64-windows-static-md` triplet.
- License: GNU Affero General Public License v3.0
  (see licenses/AGPL-3.0.txt and licenses/vcpkg/libmupdf-copyright.txt)
- Used for: saving PDFs and PDF editing (in development)
- Linked statically.
- A binary release must be accompanied by the corresponding source and build
  materials required by the AGPL-3.0 for the exact MuPDF build and this
  application. The Qt packaging script does not create that source offer.

## Libraries installed with MuPDF through vcpkg

MuPDF is built with vcpkg, which also builds and links its dependencies
(FreeType, HarfBuzz, libjpeg-turbo, OpenJPEG, zlib, Gumbo, jbig2dec, and
related libraries). Each has its own license. The copyright and license
files that vcpkg installed for them are in licenses/vcpkg/.