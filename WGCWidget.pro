QT += core gui widgets
TEMPLATE = app
TARGET = WGC_Capture
CONFIG += c++20
!win32:error("This project requires Windows")
!win32-msvc:error("Select a Qt MSVC x64 kit")
DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN
QMAKE_CXXFLAGS += /utf-8
QMAKE_CXXFLAGS_WARN_ON = /W4
LIBS += -luser32 -lgdi32 -ld3d11 -ldxgi -ld2d1 -lwindowsapp
SOURCES += main.cpp MainWindow.cpp WGCWidget.cpp tests/SmokeTest.cpp
HEADERS += MainWindow.h WGCWidget.h tests/SmokeTest.h
