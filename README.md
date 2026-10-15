# Offline PDF Reader/Editor

A desktop PDF reader (and, in progress, editor) built with C++ and Qt 6.

## Features

- Open PDFs, including password-protected files
- Page navigation and zoom (in, out, fit width, fit page)
- Page thumbnails, rendered on demand
- Full-text search with highlighted results
- Bookmarks / table of contents

Editing features are in development.

## Building

Requirements: CMake 3.21+, a C++17 compiler (developed with MSVC), and Qt 6
with the Widgets, Pdf, and PdfWidgets modules.

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=<path to your Qt, e.g. C:/Qt/6.x.x/msvc2022_64>
cmake --build build --config Debug
```

## License

This project is licensed under the **GNU Affero General Public License v3.0**.
See [LICENSE](LICENSE). Copyright (C) 2026 genecc04.

Third-party software used by this project is covered by its own licenses.
See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); the full license texts
are in the [licenses](licenses/) folder.