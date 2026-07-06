cmake_minimum_required(VERSION 3.19)
project(MicArray LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia Test)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)

set(MICARRAY_SOURCES
    src/app/appsettings.cpp
    src/app/logger.cpp
    src/control/controlvalues.cpp
    src/device/deviceclient.cpp
    src/media/audioframedecoder.cpp
    src/media/audioprocessor.cpp
    src/media/videodecoder.cpp
    src/protocol/protocolcodec.cpp
    src/ui/audiowaveformwidget.cpp
    src/ui/mainwindow.cpp
)

set(MICARRAY_HEADERS
    src/app/appsettings.h
    src/app/logger.h
    src/control/controlvalues.h
    src/device/deviceclient.h
    src/media/audioframedecoder.h
    src/media/audioprocessor.h
    src/media/videodecoder.h
    src/protocol/protocolcodec.h
    src/ui/audiowaveformwidget.h
    src/ui/mainwindow.h
)

list(TRANSFORM MICARRAY_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(TRANSFORM MICARRAY_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

qt_add_executable(MicArray
    WIN32 MACOSX_BUNDLE
    main.cpp
    ${MICARRAY_SOURCES}
    ${MICARRAY_HEADERS}
)

target_include_directories(MicArray PRIVATE src)

target_link_libraries(MicArray
    PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Network
        Qt::Multimedia
)

enable_testing()

add_library(MicArrayCore STATIC
    ${MICARRAY_SOURCES}
    ${MICARRAY_HEADERS}
)
target_include_directories(MicArrayCore PUBLIC src)
target_link_libraries(MicArrayCore
    PUBLIC
        Qt::Core
        Qt::Gui
        Qt::Widgets
        Qt::Network
        Qt::Multimedia
)

qt_add_executable(test_protocol
    tests/protocol/test_protocolcodec.cpp
)
target_link_libraries(test_protocol PRIVATE MicArrayCore Qt::Test)
add_test(NAME test_protocol COMMAND test_protocol)
set_tests_properties(test_protocol PROPERTIES ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}")

qt_add_executable(test_device
    tests/device/test_deviceclient.cpp
)
target_link_libraries(test_device PRIVATE MicArrayCore Qt::Test Qt::Network)
add_test(NAME test_device COMMAND test_device)
set_tests_properties(test_device PROPERTIES ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}")

qt_add_executable(test_media
    tests/media/test_media.cpp
)
target_link_libraries(test_media PRIVATE MicArrayCore Qt::Test Qt::Gui Qt::Multimedia)
add_test(NAME test_media COMMAND test_media)
set_tests_properties(test_media PROPERTIES ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}")

install(TARGETS MicArray
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET MicArray
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
