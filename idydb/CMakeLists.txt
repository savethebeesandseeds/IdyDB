cmake_minimum_required(VERSION 3.16)
project(idydb LANGUAGES C CXX)

option(IDYDB_BUILD_SHARED "Build idydb as a shared library" OFF)
option(IDYDB_ENABLE_MMAP "Map small read-only databases" ON)
set(IDYDB_SIZING_MODE "3" CACHE STRING "Sizing mode: 1 (tiny), 2 (small), 3 (big)")
if(NOT IDYDB_SIZING_MODE MATCHES "^[123]$")
    message(FATAL_ERROR "IDYDB_SIZING_MODE must be 1, 2, or 3")
endif()

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
target_compile_features(idydb PUBLIC cxx_std_17)
target_compile_definitions(idydb PUBLIC IDYDB_SIZING_MODE=${IDYDB_SIZING_MODE}
    IDYDB_MMAP_ALLOWED=$<BOOL:${IDYDB_ENABLE_MMAP}>)
if(UNIX)
    target_compile_definitions(idydb PRIVATE _FILE_OFFSET_BITS=64)
endif()
if(IDYDB_BUILD_SHARED AND WIN32)
    target_compile_definitions(idydb INTERFACE IDYDB_USE_DLL=1)
endif()

# OpenSSL (EVP/RAND live in Crypto)
find_package(OpenSSL REQUIRED)
target_link_libraries(idydb PUBLIC OpenSSL::Crypto)

# shm_open sometimes needs -lrt on older Linux
if (UNIX AND NOT APPLE)
    target_link_libraries(idydb PUBLIC rt)
endif()

include(CTest)
if(BUILD_TESTING)
    foreach(suite core query persistence)
        add_executable(idydb_${suite}_tests tests/${suite}_tests.cpp)
        target_link_libraries(idydb_${suite}_tests PRIVATE idydb)
        add_test(NAME ${suite} COMMAND idydb_${suite}_tests)
    endforeach()
    add_executable(idydb_c_api_test tests/c_api_test.c)
    target_link_libraries(idydb_c_api_test PRIVATE idydb)
    set_target_properties(idydb_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME c_api COMMAND idydb_c_api_test)
    # Fault hooks are excluded from the production library and Node addon.
    add_library(idydb_test_instrumented STATIC ${IDYDB_SOURCES})
    target_include_directories(idydb_test_instrumented PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(idydb_test_instrumented PUBLIC cxx_std_17)
    target_compile_definitions(idydb_test_instrumented PRIVATE IDYDB_TESTING=1)
    target_compile_definitions(idydb_test_instrumented PUBLIC IDYDB_SIZING_MODE=${IDYDB_SIZING_MODE}
        IDYDB_MMAP_ALLOWED=$<BOOL:${IDYDB_ENABLE_MMAP}>)
    target_link_libraries(idydb_test_instrumented PUBLIC OpenSSL::Crypto)
    if(UNIX)
        target_compile_definitions(idydb_test_instrumented PRIVATE _FILE_OFFSET_BITS=64)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(idydb_test_instrumented PUBLIC rt)
    endif()
    add_executable(idydb_failure_tests tests/failure_tests.cpp)
    target_link_libraries(idydb_failure_tests PRIVATE idydb_test_instrumented)
    add_test(NAME failure COMMAND idydb_failure_tests)
    add_executable(idydb_directory_tests tests/directory_tests.cpp)
    target_link_libraries(idydb_directory_tests PRIVATE idydb_test_instrumented)
    add_test(NAME directory COMMAND idydb_directory_tests)
endif()

# nice-to-have
set_target_properties(idydb PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Silence MSVC CRT warnings if desired
if (MSVC)
    target_compile_definitions(idydb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
