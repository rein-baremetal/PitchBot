cmake_minimum_required(VERSION 3.22.1)
project(pitchbot_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(pitchbot SHARED
    engine.cpp
    pitch_engine.cpp
    jni_bridge.cpp)

find_library(log-lib log)
target_link_libraries(pitchbot ${log-lib})

# Optional OpenCV ball detection (findContours RETR_TREE + HoughCircles).
# Download the OpenCV Android SDK, then add to gradle.properties:
#   opencv.dir=C:/path/OpenCV-android-sdk/sdk/native/jni
# Without it the dependency-free detector in pitch_engine.cpp is used.
if(OpenCV_DIR)
    set(OpenCV_STATIC ON)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc)
    target_compile_definitions(pitchbot PRIVATE PITCHBOT_WITH_OPENCV=1)
    target_include_directories(pitchbot PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(pitchbot ${OpenCV_LIBS})
endif()
