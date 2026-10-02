cmake_minimum_required(VERSION 3.21)
project(MicArray128 VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia MultimediaWidgets Test)
qt_standard_project_setup()
# Keep the Windows executable and runtime DLLs together in the independent package.
set(CMAKE_INSTALL_BINDIR ".")

# Only protocol-independent algorithms are shared with the 48-channel application.
set(SHARED_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_library(Array128Core STATIC
    src/protocol.cpp src/diagnostics.cpp src/hardwareprofile.cpp src/geometry.cpp src/dsp.cpp src/localizationfft.cpp
    src/recorder.cpp src/pipeline.cpp src/mainwindow.cpp src/views.cpp src/cameraprofile.cpp
    src/sys3protocol.cpp src/sys3client.cpp src/sys3panel.cpp
    ${SHARED_ROOT}/analysis/radix2fft.cpp
    ${SHARED_ROOT}/analysis/arraygeometry.cpp
    ${SHARED_ROOT}/analysis/directionprojection.cpp
    ${SHARED_ROOT}/analysis/cameraviewcalibrator.cpp
    ${SHARED_ROOT}/media/wienernoisereducer.cpp
    ${SHARED_ROOT}/media/bandpassfilter.cpp
    ${SHARED_ROOT}/media/logmmseprocessor.cpp
    ${SHARED_ROOT}/media/automaticgaincontrol.cpp
    ${SHARED_ROOT}/media/audiomonitorbuffer.cpp
)
target_include_directories(Array128Core PUBLIC src ${SHARED_ROOT})
target_link_libraries(Array128Core PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets
    Qt6::Network Qt6::Multimedia Qt6::MultimediaWidgets)
qt_add_resources(Array128Core reference PREFIX /m128 FILES resources/selftest_pcm.bin resources/check.svg resources/up.svg resources/down.svg)
qt_add_executable(MicArray128 WIN32 src/main.cpp)
target_link_libraries(MicArray128 PRIVATE Array128Core)
enable_testing()
qt_add_executable(test_array128 tests/test_array128.cpp ${SHARED_ROOT}/analysis/wavefilereader.cpp)
target_link_libraries(test_array128 PRIVATE Array128Core Qt6::Test)
target_compile_definitions(test_array128 PRIVATE M128_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME array128_core COMMAND test_array128)
qt_add_executable(test_array128_imaging tests/test_imaging.cpp)
target_link_libraries(test_array128_imaging PRIVATE Array128Core Qt6::Test)
add_test(NAME array128_imaging COMMAND test_array128_imaging)
set_tests_properties(array128_imaging PROPERTIES TIMEOUT 60)
get_target_property(M128_QT_CORE Qt6::Core IMPORTED_LOCATION_RELEASE)
get_filename_component(M128_QT_BIN "${M128_QT_CORE}" DIRECTORY)
set_tests_properties(array128_core PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${M128_QT_BIN}/../plugins/platforms"
    TIMEOUT 45)
# Set the runtime search path per test, not only in the launching shell. CTest
# restores environment variables between tests; inherited Path/PATH variants on
# Windows can otherwise select an unrelated MSYS/Qt runtime for the next test.
if(WIN32)
    string(REPLACE ";" "\\;" M128_TEST_PATH "${M128_QT_BIN};$ENV{PATH}")
    set_property(TEST array128_core array128_imaging APPEND PROPERTY ENVIRONMENT "PATH=${M128_TEST_PATH}")
    if(EXISTS "$ENV{WINDIR}/Fonts/msyh.ttc")
        set_property(TEST array128_core APPEND PROPERTY ENVIRONMENT "M128_FONT=$ENV{WINDIR}/Fonts/msyh.ttc")
    endif()
endif()
install(TARGETS MicArray128 RUNTIME DESTINATION .)
qt_generate_deploy_app_script(TARGET MicArray128 OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(CODE "set(QT_DEPLOY_BIN_DIR \".\")")
install(SCRIPT ${deploy_script})
