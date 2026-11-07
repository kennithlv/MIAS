cmake_minimum_required(VERSION 3.16)
project(MIAS VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MIAS_WITH_HIKROBOT "Build the Hikrobot MVS camera backend" OFF)
option(MIAS_WITH_FLIR "Build the FLIR Spinnaker camera backend" OFF)
option(MIAS_WITH_PREVIEW "Build optional OpenCV GUI preview support" OFF)
set(MVS_ROOT "/opt/MVS" CACHE PATH "Hikrobot MVS SDK installation root")
set(SPINNAKER_ROOT "/opt/spinnaker" CACHE PATH "FLIR Spinnaker SDK installation root")

set(_opencv_components core imgproc videoio)
if(MIAS_WITH_PREVIEW)
    list(APPEND _opencv_components highgui)
endif()
find_package(OpenCV 4 REQUIRED COMPONENTS ${_opencv_components})
find_package(nlohmann_json 3.2 REQUIRED)
find_package(Threads REQUIRED)

add_executable(MIAS
    src/main.cpp
    src/config.cpp
    src/recorder.cpp
    src/simulated_camera.cpp
    src/hikrobot_camera.cpp
    src/flir_camera.cpp
)
target_include_directories(MIAS PRIVATE include ${OpenCV_INCLUDE_DIRS})
target_link_libraries(MIAS PRIVATE ${OpenCV_LIBS} nlohmann_json::nlohmann_json Threads::Threads)
if(MSVC)
    target_compile_options(MIAS PRIVATE /W4 /EHsc)
else()
    target_compile_options(MIAS PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(MIAS_WITH_PREVIEW)
    target_compile_definitions(MIAS PRIVATE MIAS_WITH_PREVIEW)
endif()

if(MIAS_WITH_HIKROBOT)
    find_path(MVS_INCLUDE_DIR MvCameraControl.h
        HINTS "${MVS_ROOT}/include" "${MVS_ROOT}/Development/Includes")
    find_library(MVS_LIBRARY NAMES MvCameraControl
        HINTS "${MVS_ROOT}/lib/64" "${MVS_ROOT}/lib/aarch64"
              "${MVS_ROOT}/lib" "${MVS_ROOT}/lib64"
              "${MVS_ROOT}/Development/Libraries/win64")
    if(NOT MVS_INCLUDE_DIR OR NOT MVS_LIBRARY)
        message(FATAL_ERROR
            "MIAS_WITH_HIKROBOT=ON requires the MVS SDK for your architecture. "
            "Install the manufacturer's SDK and set -DMVS_ROOT=/path/to/MVS, "
            "or set MVS_INCLUDE_DIR and MVS_LIBRARY explicitly. "
            "Use -DMIAS_WITH_HIKROBOT=OFF for simulator-only tests.")
    endif()
    target_include_directories(MIAS PRIVATE "${MVS_INCLUDE_DIR}")
    target_link_libraries(MIAS PRIVATE "${MVS_LIBRARY}")
    target_compile_definitions(MIAS PRIVATE MIAS_WITH_HIKROBOT)
    get_filename_component(_mvs_library_dir "${MVS_LIBRARY}" DIRECTORY)
    set_property(TARGET MIAS APPEND PROPERTY BUILD_RPATH "${_mvs_library_dir}")
    set_property(TARGET MIAS APPEND PROPERTY INSTALL_RPATH "${_mvs_library_dir}")
endif()

if(MIAS_WITH_FLIR)
    find_path(SPINNAKER_INCLUDE_DIR Spinnaker.h
        HINTS "${SPINNAKER_ROOT}/include" /usr/include/spinnaker)
    find_library(SPINNAKER_LIBRARY NAMES Spinnaker
        HINTS "${SPINNAKER_ROOT}/lib" "${SPINNAKER_ROOT}/lib64"
              /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu
              /usr/lib/aarch64-linux-gnu)
    if(NOT SPINNAKER_INCLUDE_DIR OR NOT SPINNAKER_LIBRARY)
        message(FATAL_ERROR
            "MIAS_WITH_FLIR=ON requires the Spinnaker SDK for your architecture. "
            "Install the manufacturer's SDK and set -DSPINNAKER_ROOT=/path/to/spinnaker, "
            "or set SPINNAKER_INCLUDE_DIR and SPINNAKER_LIBRARY explicitly. "
            "Use -DMIAS_WITH_FLIR=OFF for simulator-only tests.")
    endif()
    target_include_directories(MIAS PRIVATE "${SPINNAKER_INCLUDE_DIR}")
    target_link_libraries(MIAS PRIVATE "${SPINNAKER_LIBRARY}")
    target_compile_definitions(MIAS PRIVATE MIAS_WITH_FLIR)
    get_filename_component(_spinnaker_library_dir "${SPINNAKER_LIBRARY}" DIRECTORY)
    set_property(TARGET MIAS APPEND PROPERTY BUILD_RPATH "${_spinnaker_library_dir}")
    set_property(TARGET MIAS APPEND PROPERTY INSTALL_RPATH "${_spinnaker_library_dir}")
endif()

install(TARGETS MIAS RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
    add_test(NAME simulated_video_recording
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_recording.py"
                --executable "$<TARGET_FILE:MIAS>")
    set_tests_properties(simulated_video_recording PROPERTIES TIMEOUT 120)
endif()

message(STATUS "MIAS backends: Hikrobot=${MIAS_WITH_HIKROBOT}, FLIR=${MIAS_WITH_FLIR}; preview=${MIAS_WITH_PREVIEW}")
message(STATUS "Simulator always available. SDK-free builds do NOT validate physical cameras.")
