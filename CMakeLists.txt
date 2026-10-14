cmake_minimum_required(VERSION 3.21)
project(PdfApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Pdf PdfWidgets)

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
)

target_link_libraries(PdfApp PRIVATE Qt6::Widgets Qt6::Pdf Qt6::PdfWidgets)