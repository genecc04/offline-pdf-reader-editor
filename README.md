# Offline PDF Reader/Editor

A desktop PDF reader and page editor built with C++ and Qt 6.

## Features

Reading:
- Open PDFs, including password-protected files
- Page navigation and zoom (in, out, fit width, fit page)
- Page thumbnails, rendered on demand
- Full-text search with highlighted results
- Bookmarks / table of contents
- Recent files list, drag a PDF onto the window to open it, and remembered window layout

Editing:
- Rotate or delete the current page
- Reorder pages by dragging thumbnails
- Extract a page range to a new PDF
- Insert pages from another PDF (also works for merging)
- Undo and redo
- Highlights and Annotations
- Add text in any installed font, size and color
- Add images and signatures
- Freehand pen and highlighter with adjustable size, color and opacity
- Save the edited PDF as a new file (File > Save As)
- Save in place (Ctrl+S) or save a copy (File > Save As)

More editing features are in development.

## Building

Requirements: CMake 3.21+, a C++17 compiler (developed with MSVC), Qt 6
with the Widgets, Pdf, and PdfWidgets modules (developed with Qt 6.8.0),
and [vcpkg](https://github.com/microsoft/vcpkg).

Install MuPDF with vcpkg:

```
vcpkg install libmupdf:x64-windows-static-md --host-triplet=x64-windows-static-md
```

Then configure and build, adjusting the paths to your machine:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64;C:/vcpkg/installed/x64-windows-static-md"
cmake --build build --config Debug
```

## License

This project is licensed under the **GNU Affero General Public License v3.0**.
See [LICENSE](LICENSE). Copyright (C) 2026 genecc04.

Third-party software used by this project is covered by its own licenses.
See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); the full license texts
are in the [licenses](licenses/) folder.