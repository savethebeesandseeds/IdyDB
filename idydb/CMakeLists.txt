cmake_minimum_required(VERSION 3.16)
project(idydb VERSION 0.1.0 LANGUAGES C CXX)

# Keep the supported Windows toolchain and its dependencies on one shared CRT.
# Callbacks should use idydb_alloc/idydb_free for library-owned allocations.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_options(/utf-8)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(IDYDB_BUILD_SHARED "Build idydb as a shared library" OFF)
option(IDYDB_ENABLE_MMAP "Map small read-only databases" ON)
set(IDYDB_SIZING_MODE "3" CACHE STRING "Sizing mode: 1 (tiny), 2 (small), 3 (big)")
if(NOT IDYDB_SIZING_MODE MATCHES "^[123]$")
    message(FATAL_ERROR "IDYDB_SIZING_MODE must be 1, 2, or 3")
endif()

set(IDYDB_SOURCES
    impl/db.cpp
)
if(WIN32)
    list(APPEND IDYDB_SOURCES impl/platform_windows.cpp)
else()
    list(APPEND IDYDB_SOURCES impl/platform_posix.cpp)
endif()

if (IDYDB_BUILD_SHARED)
    add_library(idydb SHARED ${IDYDB_SOURCES})
    target_compile_definitions(idydb PRIVATE IDYDB_BUILD_DLL=1)
else()
    add_library(idydb STATIC ${IDYDB_SOURCES})
endif()
add_library(idydb::idydb ALIAS idydb)

target_include_directories(idydb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
target_link_libraries(idydb PRIVATE OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(idydb PRIVATE advapi32 ws2_32 crypt32 gdi32)
endif()

# shm_open sometimes needs -lrt on older Linux
if (UNIX AND NOT APPLE)
    target_link_libraries(idydb PRIVATE rt)
endif()

# A C-only application links with the C driver, which does not add the C++
# runtime or libm implicitly. Preserve just the GNU C++ driver's extra runtime
# dependencies in the static export, without requiring C clients to enable CXX.
if(NOT IDYDB_BUILD_SHARED AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_idydb_cxx_runtime_libraries ${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES})
    if(CMAKE_C_IMPLICIT_LINK_LIBRARIES)
        list(REMOVE_ITEM _idydb_cxx_runtime_libraries ${CMAKE_C_IMPLICIT_LINK_LIBRARIES})
    endif()
    if(_idydb_cxx_runtime_libraries)
        target_link_libraries(idydb PRIVATE ${_idydb_cxx_runtime_libraries})
    endif()
    unset(_idydb_cxx_runtime_libraries)
endif()

include(CTest)
if(BUILD_TESTING)
    foreach(suite core query)
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
    target_link_libraries(idydb_test_instrumented PRIVATE OpenSSL::Crypto)
    if(WIN32)
        target_link_libraries(idydb_test_instrumented PRIVATE advapi32 ws2_32 crypt32 gdi32)
    endif()
    if(UNIX)
        target_compile_definitions(idydb_test_instrumented PRIVATE _FILE_OFFSET_BITS=64)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(idydb_test_instrumented PRIVATE rt)
    endif()
    add_executable(idydb_persistence_tests tests/persistence_tests.cpp)
    target_link_libraries(idydb_persistence_tests PRIVATE idydb_test_instrumented)
    add_test(NAME persistence COMMAND idydb_persistence_tests)
    add_executable(idydb_failure_tests tests/failure_tests.cpp)
    target_link_libraries(idydb_failure_tests PRIVATE idydb_test_instrumented)
    add_test(NAME failure COMMAND idydb_failure_tests)
    add_executable(idydb_directory_tests tests/directory_tests.cpp)
    target_link_libraries(idydb_directory_tests PRIVATE idydb_test_instrumented)
    add_test(NAME directory COMMAND idydb_directory_tests)
    add_executable(idydb_platform_tests tests/platform_tests.cpp)
    target_link_libraries(idydb_platform_tests PRIVATE idydb)
    if(WIN32)
        # This test directly edits/reads DACLs in addition to using the C API.
        target_link_libraries(idydb_platform_tests PRIVATE advapi32)
    endif()
    add_test(NAME platform COMMAND idydb_platform_tests)
    add_executable(idydb_compatibility_tests tests/compatibility_tests.cpp)
    target_link_libraries(idydb_compatibility_tests PRIVATE idydb)
    target_compile_definitions(idydb_compatibility_tests PRIVATE
        IDYDB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME compatibility COMMAND idydb_compatibility_tests)
endif()

# nice-to-have
set_target_properties(idydb PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Silence MSVC CRT warnings if desired
if (MSVC)
    target_compile_definitions(idydb PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(TARGET idydb_test_instrumented)
        target_compile_definitions(idydb_test_instrumented PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

install(TARGETS idydb EXPORT idydbTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(MINGW AND IDYDB_BUILD_SHARED)
    # Resolve the exact compiler's runtimes, never an unrelated host installation.
    foreach(runtime libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${runtime}"
            RESULT_VARIABLE runtime_status OUTPUT_VARIABLE runtime_path
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT runtime_status EQUAL 0 OR runtime_path STREQUAL runtime OR NOT EXISTS "${runtime_path}")
            message(FATAL_ERROR "The selected MinGW compiler cannot resolve required runtime ${runtime}")
        endif()
        install(FILES "${runtime_path}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    endforeach()
    # The supported cross-build environment supplies the matching package notices.
    set(IDYDB_MINGW_RUNTIME_LICENSE_DIR "/usr/share/doc" CACHE PATH
        "Directory containing the pinned Debian MinGW package copyright notices")
    foreach(package gcc-mingw-w64-x86-64-posix-runtime mingw-w64-common)
        set(notice "${IDYDB_MINGW_RUNTIME_LICENSE_DIR}/${package}/copyright")
        if(NOT EXISTS "${notice}")
            message(FATAL_ERROR "Missing MinGW runtime notice: ${notice}; run the pinned dependency setup")
        endif()
        install(FILES "${notice}" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/idydb
            RENAME "${package}-copyright.txt")
    endforeach()
endif()
install(FILES include/db.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT idydbTargets FILE idydbTargets.cmake NAMESPACE idydb::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/idydb)
configure_package_config_file(cmake/idydbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/idydbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/idydb)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/idydbConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/idydbConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/idydbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/idydb)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/idydb)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE")
    install(FILES ../LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/idydb)
endif()
if(WIN32 AND EXISTS "${OPENSSL_INCLUDE_DIR}/../share/openssl/copyright")
    install(FILES "${OPENSSL_INCLUDE_DIR}/../share/openssl/copyright"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/idydb RENAME OpenSSL-copyright)
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CPACK_PACKAGE_NAME idydb)
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_VENDOR "IdyDB contributors")
    set(CPACK_GENERATOR ZIP)
    include(CPack)
endif()
