cmake_minimum_required(VERSION 3.30)
project(NoHitDelayDll)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(JNI REQUIRED)
find_package(Threads REQUIRED)

add_library(NoHitDelay SHARED dllmain.cpp scope_guard.h)
target_link_libraries(NoHitDelay PRIVATE JNI::JNI Threads::Threads ${CMAKE_DL_LIBS})

if(APPLE)
    target_link_libraries(NoHitDelay PRIVATE "-framework CoreFoundation")
endif()

if(UNIX)
    file(GENERATE
        OUTPUT "$<TARGET_FILE_DIR:NoHitDelay>/inject.sh"
        INPUT "${CMAKE_CURRENT_SOURCE_DIR}/inject.sh.in"
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
endif()
