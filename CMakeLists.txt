cmake_minimum_required(VERSION 3.16)
project(swift)

set(CMAKE_CXX_STANDARD 14)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(SWIFT_VERSION "3.1.1" CACHE STRING
    "The user-visible version of the Swift compiler")


#add_executable(swift main.cpp)
include_directories(include)
add_subdirectory(lib)

llvm_map_components_to_libnames(llvm_libs core)
target_link_libraries(swift ${llvm_libs})