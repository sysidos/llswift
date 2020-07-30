cmake_minimum_required(VERSION 3.16)

# vcpkg
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake
        CACHE STRING "vcpkg toolchain file")

project(swift)
set(CMAKE_CXX_STANDARD 14)

set(SWIFT_VERSION "3.1.1")
message(STATUS "Swift version: ${SWIFT_VERSION}")


find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(llvm_libs support core analysis irreader bitreader bitwriter coverage debuginfoDWARF
        profiledata instrumentation object objcarcopts mc mcparser
        bitreader bitwriter lto ipo option core support)






string(REGEX REPLACE "([0-9]+)\\.[0-9]+(\\.[0-9]+)?" "\\1" SWIFT_VERSION_MAJOR
        ${SWIFT_VERSION})
string(REGEX REPLACE "[0-9]+\\.([0-9]+)(\\.[0-9]+)?" "\\1" SWIFT_VERSION_MINOR
        ${SWIFT_VERSION})

set_property(SOURCE lib/Basic/Version.cpp APPEND_STRING PROPERTY COMPILE_FLAGS
        " -DSWIFT_VERSION=${SWIFT_VERSION} -DSWIFT_VERSION_MAJOR=${SWIFT_VERSION_MAJOR} -DSWIFT_VERSION_MINOR=${SWIFT_VERSION_MINOR}")





include_directories(include)
add_subdirectory(lib)


add_library(swift include lib)
target_link_libraries(swift ${llvm_libs})