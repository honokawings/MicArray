cmake_minimum_required(VERSION 3.19)
project(MicArray LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia Test Charts Concurrent)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)

set(MICARRAY_SOURCES
    src/analysis/arraygeometry.cpp
    src/analysis/audioanalysisworker.cpp
    src/analysis/calibrationprofile.cpp
    src/analysis/cameraviewcalibrator.cpp
    src/analysis/directionprojection.cpp
    src/analysis/gccphatlocator.cpp
    src/analysis/livelocalizationworker.cpp
    src/analysis/radix2fft.cpp
    src/analysis/soundsourcelocatorregistry.cpp
    src/analysis/wavefilereader.cpp
    src/app/appsettings.cpp
    src/app/logger.cpp
    src/control/controlvalues.cpp
    src/device/deviceclient.cpp
    src/device/networkworker.cpp
    src/diagnostics/alertcoalescer.cpp
    src/diagnostics/diagnosticexporter.cpp
    src/media/audioframedecoder.cpp
    src/media/audioprocessor.cpp
    src/media/audiorecorder.cpp
    src/media/channelgainprocessor.cpp
    src/media/gainprofilestore.cpp
    src/media/mediaframequeue.cpp
    src/media/multichannelwavwriter.cpp
    src/media/videodecoder.cpp
    src/monitoring/healthmonitor.cpp
    src/protocol/protocolcodec.cpp
    src/protocol/protocolworker.cpp
    src/ui/arrayheatmapwidget.cpp
    src/ui/audioanalysispage.cpp
    src/ui/audiowaveformwidget.cpp
    src/ui/directionphotowidget.cpp
    src/ui/livelocalizationview.cpp
    src/ui/livelocalizationpage.cpp
    src/ui/displayformat.cpp
    src/ui/mainwindow.cpp
    src/ui/trendchartwidget.cpp
    src/ui/videopreviewwidget.cpp
)

set(MICARRAY_HEADERS
    src/analysis/arraygeometry.h
    src/analysis/audioanalysisworker.h
    src/analysis/calibrationprofile.h
    src/analysis/cameraviewcalibrator.h
    src/analysis/directionprojection.h
    src/analysis/gccphatlocator.h
    src/analysis/isoundsourcelocator.h
    src/analysis/ispatialresponseprovider.h
    src/analysis/localizationtypes.h
    src/analysis/livelocalizationworker.h
    src/analysis/radix2fft.h
    src/analysis/soundsourcelocatorregistry.h
    src/analysis/wavefilereader.h
    src/app/appsettings.h
    src/app/logger.h
    src/control/controlvalues.h
    src/device/deviceclient.h
    src/device/networkworker.h
    src/diagnostics/alertcoalescer.h
    src/diagnostics/diagnosticexporter.h
    src/media/audioframedecoder.h
    src/media/audioprocessor.h
    src/media/audiorecorder.h
    src/media/channelgainprocessor.h
    src/media/gainprofilestore.h
    src/media/mediaframequeue.h
    src/media/multichannelwavwriter.h
    src/media/videodecoder.h
    src/monitoring/healthmonitor.h
    src/protocol/protocolcodec.h
    src/protocol/protocolworker.h
    src/ui/arrayheatmapwidget.h
    src/ui/audioanalysispage.h
    src/ui/audiowaveformwidget.h
    src/ui/directionphotowidget.h
    src/ui/livelocalizationview.h
    src/ui/livelocalizationpage.h
    src/ui/displayformat.h
    src/ui/mainwindow.h
    src/ui/trendchartwidget.h
    src/ui/videopreviewwidget.h
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
        Qt::Charts
        Qt::Concurrent
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
        Qt::Charts
        Qt::Concurrent
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

qt_add_executable(test_app
    tests/app/test_app.cpp
)
target_link_libraries(test_app PRIVATE MicArrayCore Qt::Test)
add_test(NAME test_app COMMAND test_app)
set_tests_properties(test_app PROPERTIES ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}")

qt_add_executable(test_ui
    tests/ui/test_dashboard.cpp
)
target_link_libraries(test_ui PRIVATE MicArrayCore Qt::Test Qt::Charts)
add_test(NAME test_ui COMMAND test_ui)
set_tests_properties(test_ui PROPERTIES ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}")

qt_add_executable(test_analysis
    tests/analysis/test_audioanalysis.cpp
)
target_link_libraries(test_analysis PRIVATE MicArrayCore Qt::Test Qt::Gui Qt::Multimedia)
add_test(NAME test_analysis COMMAND test_analysis)
set_tests_properties(test_analysis PROPERTIES ENVIRONMENT "PATH=${QT_BIN_DIR};$ENV{PATH}")

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
