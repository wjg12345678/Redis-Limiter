cmake_minimum_required(VERSION 3.14)
project(redis_rate_limiter VERSION 1.0.0 LANGUAGES CXX)

option(REDIS_LIMITER_BUILD_PYTHON "Build the redis_limiter Python extension" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 查找依赖
find_package(Threads REQUIRED)
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "hiredis not found. Set HIREDIS_INCLUDE_DIR and HIREDIS_LIBRARY.")
endif()

# C++核心库
add_library(redis_limiter_core STATIC
    src/redis_pool.cpp
    src/sliding_window_limiter.cpp
)
add_library(redis_limiter::core ALIAS redis_limiter_core)
add_library(redis_pool ALIAS redis_limiter_core)
set_target_properties(redis_limiter_core PROPERTIES
    EXPORT_NAME core
    OUTPUT_NAME redis_limiter_core
)

target_include_directories(redis_limiter_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${HIREDIS_INCLUDE_DIR}
)

target_link_libraries(redis_limiter_core PUBLIC
    ${HIREDIS_LIBRARY}
    Threads::Threads
)

# Python模块
if(REDIS_LIMITER_BUILD_PYTHON)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(redis_limiter src/python_binding.cpp)
    target_link_libraries(redis_limiter PRIVATE redis_limiter_core)
    target_include_directories(redis_limiter PRIVATE ${HIREDIS_INCLUDE_DIR})
endif()

# 安装
install(TARGETS redis_limiter_core
    EXPORT redis_limiterTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT redis_limiterTargets
    FILE redis_limiterTargets.cmake
    NAMESPACE redis_limiter::
    DESTINATION lib/cmake/redis_limiter
)

if(REDIS_LIMITER_BUILD_PYTHON)
    install(TARGETS redis_limiter DESTINATION .)
endif()
