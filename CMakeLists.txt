cmake_minimum_required(VERSION 3.16)

project(swift)

set(CMAKE_CXX_STANDARD 14)

set(SWIFT_VERSION "3.1.1")
message(STATUS "Swift version: ${SWIFT_VERSION}")

# LLVM 8.0.1
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(llvm_libs support core analysis irreader bitreader bitwriter coverage debuginfoDWARF
        profiledata instrumentation object objcarcopts mc mcparser
        bitreader bitwriter lto ipo option)

## CLANG
#find_package(CLANG REQUIRED CONFIG)
#message(STATUS "Found CLANG ${CLANG_VERSION}")
#message(STATUS "Using CLANGConfig.cmake in: ${CLANG_DIR}")
#include_directories(${CLANG_INCLUDE_DIRS})
#add_definitions(${CLANG_DEFINITIONS})


# SOURCE
include_directories(include)
add_subdirectory(lib)


# TARGETS
add_executable(swift main.cpp)
target_link_libraries(swift ${llvm_libs})