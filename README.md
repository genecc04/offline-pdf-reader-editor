# Offline PDF Reader/Editor

A desktop PDF reader and editor for Windows, built with C++, Qt 6 and MuPDF.
Everything happens on your computer: your files are never uploaded, and the app has no
network features.

## Features

**Reading**
- Open PDFs, including password-protected ones. Drag a file onto the window, use
  File > Open Recent, or open one from the command line
- Page navigation and zoom: zoom in and out, fit width, fit page, Ctrl + mouse wheel
- Page thumbnails, drawn on demand so large files open quickly
- Full-text search with highlighted results
- Bookmarks / table of contents
- Select text with the mouse and copy it, across pages
- Remembers your window size and panel layout

**Page editing**
- Rotate or delete pages. Select several thumbnails with Shift + click or Ctrl + click
- Reorder pages by dragging thumbnails
- Extract a page range into a new PDF
- Insert pages from another PDF (this also merges documents)
- Undo and redo (up to 20 steps)
- Save in place (Ctrl+S) or save a copy (Save As). Saving replaces the file safely,
  so a failed save never costs you the original

**Marking up and filling out**
- Fill out forms: type text into the blanks, draw check marks, and add your signature
- Add text in any font installed on your computer, with size, bold, italic and a
  full color picker. Drag a box on the page, type, then click elsewhere to place it
- Draw with a pen, a highlighter or white-out, with adjustable size, color and opacity
- Highlight every occurrence of a word or phrase
- Add images and signatures. Pick a file, click the page, then drag to move it and
  drag a corner to resize

**Security**
- Add, change or remove a password (AES-256)

## Download

Get `PdfApp-windows.zip` from the
[Releases page](https://github.com/genecc04/offline-pdf-reader-editor/releases),
unzip it anywhere, and run `PdfApp.exe`. No installer is needed.

**Keep all the files in the folder together.** `PdfApp.exe` needs the Qt libraries that
come with it and does not run on its own.

It also needs the
[Microsoft Visual C++ Redistributable (x64)](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist),
which most computers already have.

## Keyboard shortcuts

| Action | Shortcut |
|---|---|
| Open / Save / Save As | Ctrl+O / Ctrl+S / Ctrl+Shift+S |
| Undo / Redo | Ctrl+Z / Ctrl+Y |
| Find | Ctrl+F |
| Show or hide the Pages panel / Bookmarks panel | F4 / Ctrl+B |
| Copy selected text / Select all text | Ctrl+C / Ctrl+A |
| Rotate left / right | Ctrl+L / Ctrl+R |
| Highlight text | Ctrl+H |
| Add text | Ctrl+T |
| Draw | Ctrl+D |
| Add image | Ctrl+I |
| Zoom | Ctrl + mouse wheel |
| Cancel what you are placing | Esc |

## Known limitations

- **Form fields are not interactive.** The app does not detect a PDF's built-in fillable
  fields. What you add is placed on top of the page, so the form's own fields stay empty
  and other programs that read form data will not see it.
- **White-out only covers.** The text under it is still in the file, so search and
  copy still find it. Don't use it to hide anything sensitive.
- **Added text is placed as a picture** (so any font works), which means it can't be
  selected, searched or edited afterwards. Undo and delete still work.
- Links inside PDFs are not clickable yet, and double-click does not select a word.
- Scanned PDFs have no text to select or search (there is no OCR).
- Inserting pages from a password-protected PDF is not supported.
- Bookmarks are not updated after you delete or reorder pages.
- Reordered or extracted files lose their accessibility tagging.
- Built and tested on Windows with MSVC only.

## Building from source

Requirements: CMake 3.21+, a C++17 compiler (developed with MSVC), Qt 6 with the
Widgets, Pdf and PdfWidgets modules (developed with Qt 6.8.0), and
[vcpkg](https://github.com/microsoft/vcpkg).

Install MuPDF with vcpkg:

```
vcpkg install libmupdf:x64-windows-static-md --host-triplet=x64-windows-static-md
```

Then configure and build, adjusting the paths to your machine:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64;C:/vcpkg/installed/x64-windows-static-md"
cmake --build build --config Debug
```

### Packaging a release

```
powershell -ExecutionPolicy Bypass -File .\package.ps1
```

This builds in Release mode and creates `dist/PdfApp-windows.zip` with the program, the
Qt libraries it needs and the license files. Adjust the paths at the top of
`package.ps1` to match your machine.

## Reporting problems

Found a bug? Please open an
[issue](https://github.com/genecc04/offline-pdf-reader-editor/issues) and say which
Windows version you use and what you were doing. If you can share a PDF that shows the
problem, that helps a lot.

## License

This project is licensed under the **GNU Affero General Public License v3.0**.
See [LICENSE](LICENSE). Copyright (C) 2026 genecc04.

It uses [Qt](https://www.qt.io) (LGPL-3.0) and [MuPDF](https://mupdf.com)
(AGPL-3.0, Artifex Software, Inc.), among other libraries. Their licenses are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the full texts are in the
[licenses](licenses/) folder.