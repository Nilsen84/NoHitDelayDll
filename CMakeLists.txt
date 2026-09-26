cmake_minimum_required(VERSION 3.30)
project(NoHitDelayDll)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(JNI REQUIRED)

add_library(NoHitDelay SHARED dllmain.cpp scope_guard.h)
target_link_libraries(NoHitDelay PRIVATE JNI::JNI)