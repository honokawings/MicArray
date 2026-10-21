cmake_minimum_required(VERSION 3.19)
project(MicArray LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia Test Charts Concurrent)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CheckIPOSupported)
check_ipo_supported(RESULT MICARRAY_IPO_SUPPORTED LANGUAGES CXX)
option(MICARRAY_ENABLE_IPO "Enable release link-time optimization" ON)
if(MICARRAY_IPO_SUPPORTED AND MICARRAY_ENABLE_IPO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
set(MICARRAY_TEST_PATH "${QT_BIN_DIR};$ENV{PATH}")
string(REPLACE ";" "\\;" MICARRAY_TEST_PATH_ESCAPED "${MICARRAY_TEST_PATH}")

set(MICARRAY_SOURCES
    src/analysis/arraygeometry.cpp
    src/analysis/audioanalysisworker.cpp
    src/analysis/calibrationprofile.cpp
    src/analysis/cameraviewcalibrator.cpp
    src/analysis/directionprojection.cpp
    src/analysis/gccphatlocator.cpp
    src/analysis/livelocalizationworker.cpp
    src/analysis/neuralsrppeakrefiner.cpp
    src/analysis/multisourcedirectiontracker.cpp
    src/analysis/plspatialprojector.cpp
    src/analysis/pllocalizationcontroller.cpp
    src/analysis/radix2fft.cpp
    src/analysis/robustsrpphatscorer.cpp
    src/analysis/soundsourcelocatorregistry.cpp
    src/analysis/wavefilereader.cpp
    src/app/appsettings.cpp
    src/app/logger.cpp
    src/control/controlvalues.cpp
    src/control/directioncontrolmapper.cpp
    src/control/pldenoisesession.cpp
    src/device/deviceclient.cpp
    src/device/networkworker.cpp
    src/diagnostics/alertcoalescer.cpp
    src/diagnostics/diagnosticexporter.cpp
    src/media/audioframedecoder.cpp
    src/media/plspatialmap.cpp
    src/media/pldenoisetransport.cpp
    src/media/audiolocationsideband.cpp
    src/media/audiomonitorbuffer.cpp
    src/media/automaticgaincontrol.cpp
    src/media/audioprocessor.cpp
    src/media/audiorecorder.cpp
    src/media/bandpassfilter.cpp
    src/media/channelgainprocessor.cpp
    src/media/fractionaldelaybeamformer.cpp
    src/media/gainprofilestore.cpp
    src/media/logmmseprocessor.cpp
    src/media/mediaframequeue.cpp
    src/media/multichannelwavwriter.cpp
    src/media/noisereductionframedecoder.cpp
    src/media/noisereductiontypes.cpp
    src/media/noisereductionmailbox.cpp
    src/media/noisereductionworker.cpp
    src/media/wienernoisereducer.cpp
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
    src/ui/noisereductionvideowidget.cpp
    src/ui/noisereductionpage.cpp
    src/ui/powerspectrumwidget.cpp
    src/ui/processingnodelistwidget.cpp
    src/ui/displayformat.cpp
    src/ui/mainwindow.cpp
    src/ui/trendchartwidget.cpp
    src/ui/uitheme.cpp
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
    src/analysis/neuralmodeldata.h
    src/analysis/neuralsrppeakrefiner.h
    src/analysis/multisourcedirectiontracker.h
    src/analysis/plspatialprojector.h
    src/analysis/pllocalizationcontroller.h
    src/analysis/radix2fft.h
    src/analysis/robustsrpphatscorer.h
    src/analysis/soundsourcelocatorregistry.h
    src/analysis/wavefilereader.h
    src/app/appsettings.h
    src/app/logger.h
    src/control/controlvalues.h
    src/control/directioncontrolmapper.h
    src/control/pldenoisesession.h
    src/device/deviceclient.h
    src/device/networkworker.h
    src/diagnostics/alertcoalescer.h
    src/diagnostics/diagnosticexporter.h
    src/media/audioframedecoder.h
    src/media/plspatialmap.h
    src/media/pldenoisetransport.h
    src/media/audiolocationsideband.h
    src/media/audiomonitorbuffer.h
    src/media/automaticgaincontrol.h
    src/media/audioprocessor.h
    src/media/audiorecorder.h
    src/media/bandpassfilter.h
    src/media/channelgainprocessor.h
    src/media/fractionaldelaybeamformer.h
    src/media/gainprofilestore.h
    src/media/logmmseprocessor.h
    src/media/mediaframequeue.h
    src/media/multichannelwavwriter.h
    src/media/noisereductionframedecoder.h
    src/media/noisereductiontypes.h
    src/media/noisereductionmailbox.h
    src/media/noisereductionworker.h
    src/media/wienernoisereducer.h
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
    src/ui/noisereductionvideowidget.h
    src/ui/noisereductionpage.h
    src/ui/powerspectrumwidget.h
    src/ui/processingnodelistwidget.h
    src/ui/displayformat.h
    src/ui/mainwindow.h
    src/ui/trendchartwidget.h
    src/ui/uitheme.h
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
add_test(NAME test_protocol COMMAND test_protocol -o "${CMAKE_CURRENT_BINARY_DIR}/test_protocol-results.txt,txt")
set_tests_properties(test_protocol PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(test_device
    tests/device/test_deviceclient.cpp
)
target_link_libraries(test_device PRIVATE MicArrayCore Qt::Test Qt::Network)
add_test(NAME test_device COMMAND test_device -o "${CMAKE_CURRENT_BINARY_DIR}/test_device-results.txt,txt")
set_tests_properties(test_device PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(test_media
    tests/media/test_media.cpp
)
target_link_libraries(test_media PRIVATE MicArrayCore Qt::Test Qt::Gui Qt::Multimedia)
add_test(NAME test_media COMMAND test_media -o "${CMAKE_CURRENT_BINARY_DIR}/test_media-results.txt,txt")
set_tests_properties(test_media PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(test_app
    tests/app/test_app.cpp
)
target_link_libraries(test_app PRIVATE MicArrayCore Qt::Test)
add_test(NAME test_app COMMAND test_app -o "${CMAKE_CURRENT_BINARY_DIR}/test_app-results.txt,txt")
set_tests_properties(test_app PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(test_ui
    tests/ui/test_dashboard.cpp
)
target_link_libraries(test_ui PRIVATE MicArrayCore Qt::Test Qt::Charts)
add_test(NAME test_ui COMMAND test_ui -o "${CMAKE_CURRENT_BINARY_DIR}/test_ui-results.txt,txt")
set_tests_properties(test_ui PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(test_analysis
    tests/analysis/test_audioanalysis.cpp
)
target_link_libraries(test_analysis PRIVATE MicArrayCore Qt::Test Qt::Gui Qt::Multimedia)
add_test(NAME test_analysis COMMAND test_analysis -o "${CMAKE_CURRENT_BINARY_DIR}/test_analysis-results.txt,txt")
set_tests_properties(test_analysis PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")
# This suite includes wall-clock CPU budgets; concurrent UI tests invalidate that measurement.
set_tests_properties(test_analysis PROPERTIES RUN_SERIAL TRUE)

qt_add_executable(test_plintegration tests/media/test_plintegration.cpp)
target_link_libraries(test_plintegration PRIVATE MicArrayCore Qt::Test)
add_test(NAME test_plintegration COMMAND test_plintegration -o "${CMAKE_CURRENT_BINARY_DIR}/test_plintegration-results.txt,txt")
set_tests_properties(test_plintegration PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(train_neural_srp_peak
    tools/train_neural_srp_peak.cpp
)
set_target_properties(train_neural_srp_peak PROPERTIES
    EXCLUDE_FROM_ALL TRUE
)
target_link_libraries(train_neural_srp_peak PRIVATE MicArrayCore Qt::Core)

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

qt_add_executable(test_plheatmap tests/media/test_plheatmap.cpp)
target_link_libraries(test_plheatmap PRIVATE MicArrayCore Qt::Test)
target_compile_definitions(test_plheatmap PRIVATE
    MICARRAY_PL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hmp_rtl_transport.mem"
    MICARRAY_PL_MAP_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pl_triple_heatmap.mem")
add_test(NAME test_plheatmap COMMAND test_plheatmap -o "${CMAKE_CURRENT_BINARY_DIR}/test_plheatmap-results.txt,txt")
set_tests_properties(test_plheatmap PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

target_compile_definitions(test_plintegration PRIVATE MICARRAY_PL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/hmp_rtl_transport.mem")

qt_add_executable(test_cameralens tests/analysis/test_cameralens.cpp)
target_link_libraries(test_cameralens PRIVATE MicArrayCore Qt::Test)
target_compile_definitions(test_cameralens PRIVATE
    MICARRAY_LENS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/camera_lens_opencv.json")
add_test(NAME test_cameralens COMMAND test_cameralens -o "${CMAKE_CURRENT_BINARY_DIR}/test_cameralens-results.txt,txt")
set_tests_properties(test_cameralens PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(test_pldenoise tests/media/test_pldenoise.cpp)
target_link_libraries(test_pldenoise PRIVATE MicArrayCore Qt::Test)
set(MICARRAY_DENOISE_GOLDEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tests/private-golden"
    CACHE PATH "Frozen original GitHub stage outputs")
target_compile_definitions(test_pldenoise PRIVATE
    MICARRAY_DENOISE_GOLDEN_ROOT="${MICARRAY_DENOISE_GOLDEN_ROOT}")
add_test(NAME test_pldenoise COMMAND test_pldenoise -o "${CMAKE_CURRENT_BINARY_DIR}/test_pldenoise-results.txt,txt")
set_tests_properties(test_pldenoise PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(pl_denoise_replay tools/pl_denoise_replay.cpp)
target_link_libraries(pl_denoise_replay PRIVATE MicArrayCore Qt::Core)

qt_add_executable(test_pldenoise_ui tests/ui/test_pldenoise_ui.cpp)
target_link_libraries(test_pldenoise_ui PRIVATE MicArrayCore Qt::Test)
target_compile_definitions(test_pldenoise_ui PRIVATE
    MICARRAY_PL_UI_PREVIEW="${CMAKE_CURRENT_BINARY_DIR}/pl_denoise_mode_preview.png")
add_test(NAME test_pldenoise_ui COMMAND test_pldenoise_ui -o "${CMAKE_CURRENT_BINARY_DIR}/test_pldenoise_ui-results.txt,txt")
set_tests_properties(test_pldenoise_ui PROPERTIES
    ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED};QT_QPA_PLATFORM=offscreen")

qt_add_executable(test_plwiener tests/media/test_plwiener.cpp)
target_link_libraries(test_plwiener PRIVATE MicArrayCore Qt::Test)
target_compile_definitions(test_plwiener PRIVATE MICARRAY_DENOISE_GOLDEN_ROOT="${MICARRAY_DENOISE_GOLDEN_ROOT}")
add_test(NAME test_plwiener COMMAND test_plwiener -o "${CMAKE_CURRENT_BINARY_DIR}/test_plwiener-results.txt,txt")
set_tests_properties(test_plwiener PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

qt_add_executable(test_plfull tests/media/test_plfull.cpp)
target_link_libraries(test_plfull PRIVATE MicArrayCore Qt::Test)
target_compile_definitions(test_plfull PRIVATE MICARRAY_DENOISE_GOLDEN_ROOT="${MICARRAY_DENOISE_GOLDEN_ROOT}")
target_compile_definitions(test_plfull PRIVATE MICARRAY_FULL_PROFILE_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/firmware/profile/profile_manifest.json")
add_test(NAME test_plfull COMMAND test_plfull -o "${CMAKE_CURRENT_BINARY_DIR}/test_plfull-results.txt,txt")
set_tests_properties(test_plfull PROPERTIES ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED}")

# Synthetic sustained-rate regressions use the same production Core, not private recordings.
option(MICARRAY_BUILD_STREAMING_TESTS "Build Windows real-time audio scheduling regressions" ON)
if(WIN32 AND MICARRAY_BUILD_STREAMING_TESTS)
    qt_add_executable(test_stream_wallclock tests/streaming/test_wallclock.cpp)
    target_compile_definitions(test_stream_wallclock PRIVATE R2_MAILBOX=1)
    target_link_libraries(test_stream_wallclock PRIVATE MicArrayCore Qt::Test)
    qt_add_executable(test_stream_contract tests/streaming/test_legacy_contract.cpp)
    target_link_libraries(test_stream_contract PRIVATE MicArrayCore Qt::Test)
    qt_add_executable(test_stream_mainwindow tests/streaming/mainwindow_pressure/test_mainwindow_pressure.cpp)
    target_link_libraries(test_stream_mainwindow PRIVATE MicArrayCore Qt::Test)
    foreach(stream_test test_stream_wallclock test_stream_contract test_stream_mainwindow)
        add_test(NAME ${stream_test} COMMAND ${stream_test} -o "${CMAKE_CURRENT_BINARY_DIR}/${stream_test}-results.txt,txt")
        set_tests_properties(${stream_test} PROPERTIES RUN_SERIAL TRUE TIMEOUT 240
            ENVIRONMENT "PATH=${MICARRAY_TEST_PATH_ESCAPED};QT_QPA_PLATFORM=offscreen")
    endforeach()
endif()
