cmake_minimum_required(VERSION 3.21)
project(PdfApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Pdf PdfWidgets)
find_package(unofficial-libmupdf CONFIG REQUIRED)

add_executable(PdfApp
    main.cpp
    MainWindow.cpp
    MainWindow.h
    ThumbnailPane.cpp
    ThumbnailPane.h
    ThumbnailModel.cpp
    ThumbnailModel.h
    SearchPane.cpp
    SearchPane.h
    BookmarkPane.cpp
    BookmarkPane.h
    PdfEditor.cpp
    PdfEditor.h
    PageRangeDialog.cpp
    PageRangeDialog.h
    EditHistory.cpp
    EditHistory.h
)

target_link_libraries(PdfApp PRIVATE
    Qt6::Widgets Qt6::Pdf Qt6::PdfWidgets
    unofficial::libmupdf::libmupdf
)