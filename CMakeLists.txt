cmake_minimum_required(VERSION 3.20)

project(DeltaDCT LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
include(ExternalProject)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ---------------------------
# zstd (static)
# ---------------------------
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    SOURCE_SUBDIR build/cmake
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(zstd)

set(DELTADCT_ZSTD_TARGET "")
if(TARGET libzstd_static)
    set(DELTADCT_ZSTD_TARGET libzstd_static)
elseif(TARGET libzstd)
    set(DELTADCT_ZSTD_TARGET libzstd)
elseif(TARGET zstd::libzstd_static)
    set(DELTADCT_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd)
    set(DELTADCT_ZSTD_TARGET zstd::libzstd)
else()
    message(FATAL_ERROR "Failed to locate zstd static target (libzstd_static).")
endif()

# ---------------------------
# libjpeg-turbo (static, JPEG8 API)
# ---------------------------
FetchContent_Declare(
    libjpeg_turbo_src
    GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
    GIT_TAG 3.0.4
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(libjpeg_turbo_src)
if(NOT libjpeg_turbo_src_POPULATED)
    FetchContent_Populate(libjpeg_turbo_src)
endif()

set(LIBJPEG_TURBO_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/libjpeg_turbo-install")
set(LIBJPEG_TURBO_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/libjpeg_turbo-build")

if(MSVC)
    set(LIBJPEG_TURBO_STATIC_LIB "${LIBJPEG_TURBO_INSTALL_DIR}/lib/jpeg-static${CMAKE_STATIC_LIBRARY_SUFFIX}")
else()
    set(LIBJPEG_TURBO_STATIC_LIB "${LIBJPEG_TURBO_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()

file(MAKE_DIRECTORY "${LIBJPEG_TURBO_INSTALL_DIR}/include")
file(MAKE_DIRECTORY "${LIBJPEG_TURBO_INSTALL_DIR}/lib")

set(LIBJPEG_TURBO_CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_JPEG8=ON
    -DWITH_TURBOJPEG=OFF
)

if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "")
    list(APPEND LIBJPEG_TURBO_CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()

if(CMAKE_BUILD_TYPE)
    list(APPEND LIBJPEG_TURBO_CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})
endif()

ExternalProject_Add(libjpeg_turbo_ep
    SOURCE_DIR "${libjpeg_turbo_src_SOURCE_DIR}"
    BINARY_DIR "${LIBJPEG_TURBO_BINARY_DIR}"
    INSTALL_DIR "${LIBJPEG_TURBO_INSTALL_DIR}"
    CMAKE_ARGS ${LIBJPEG_TURBO_CMAKE_ARGS}
    BUILD_BYPRODUCTS "${LIBJPEG_TURBO_STATIC_LIB}"
    UPDATE_DISCONNECTED TRUE
)

add_library(deltadct_jpeg STATIC IMPORTED GLOBAL)
set_target_properties(deltadct_jpeg PROPERTIES
    IMPORTED_LOCATION "${LIBJPEG_TURBO_STATIC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBJPEG_TURBO_INSTALL_DIR}/include"
)
add_dependencies(deltadct_jpeg libjpeg_turbo_ep)

set(DELTADCT_JPEG_TARGET deltadct_jpeg)

# ---------------------------
# pybind11 (Python bindings)
# ---------------------------
set(PYBIND11_FINDPYTHON ON)
set(Python_FIND_VIRTUALENV FIRST)
set(Python_FIND_STRATEGY LOCATION)
if(DEFINED ENV{CONDA_PREFIX} AND NOT DEFINED Python_ROOT_DIR)
    set(Python_ROOT_DIR "$ENV{CONDA_PREFIX}")
endif()

FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(pybind11)

add_library(deltadct_core
    src/phase1_dct_codec.cpp
    src/phase2_similarity.cpp
    src/phase3_delta.cpp
    src/phase4_storage.cpp
    src/phase5_restore.cpp
    src/phase6_benchmark.cpp
)

target_include_directories(deltadct_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(deltadct_core
    PRIVATE
        DELTADCT_HAVE_ZSTD=1
)

target_link_libraries(deltadct_core
    PUBLIC
        ${DELTADCT_JPEG_TARGET}
        ${DELTADCT_ZSTD_TARGET}
)

add_library(deltadct_api
    src/deltadct.cpp
)

target_include_directories(deltadct_api
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(deltadct_api
    PUBLIC
        deltadct_core
)

add_executable(deltadct_tool
    src/main.cpp
)

target_link_libraries(deltadct_tool
    PRIVATE
        deltadct_core
)

add_executable(deltadct_phase3_tests
    tests/phase3_tests.cpp
)

target_link_libraries(deltadct_phase3_tests
    PRIVATE
        deltadct_core
)

add_executable(deltadct_phase45_tests
    tests/phase45_tests.cpp
)

target_link_libraries(deltadct_phase45_tests
    PRIVATE
        deltadct_core
)

pybind11_add_module(pydeltadct MODULE
    bindings/bindings.cpp
)

target_link_libraries(pydeltadct
    PRIVATE
        deltadct_api
)

target_include_directories(pydeltadct
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(pydeltadct PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
