cmake_minimum_required(VERSION 3.16)
project(idydb LANGUAGES C CXX)

option(IDYDB_BUILD_SHARED "Build idydb as a shared library" OFF)

set(IDYDB_SOURCES
    impl/db.cpp
)

if (IDYDB_BUILD_SHARED)
    add_library(idydb SHARED ${IDYDB_SOURCES})
    target_compile_definitions(idydb PRIVATE IDYDB_BUILD_DLL=1)
    # This helps on MSVC when you don't want to manually export every symbol.
    set_target_properties(idydb PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    add_library(idydb STATIC ${IDYDB_SOURCES})
endif()

target_include_directories(idydb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# OpenSSL (EVP/RAND live in Crypto)
find_package(OpenSSL REQUIRED)
target_link_libraries(idydb PUBLIC OpenSSL::Crypto)

# shm_open sometimes needs -lrt on older Linux
if (UNIX AND NOT APPLE)
    target_link_libraries(idydb PUBLIC rt)
endif()

# nice-to-have
set_target_properties(idydb PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Silence MSVC CRT warnings if desired
if (MSVC)
    target_compile_definitions(idydb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
