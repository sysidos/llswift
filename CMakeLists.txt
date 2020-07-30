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

llvm_map_components_to_libnames(llvm_libs support core irreader tablegen)

#function(add_swift_library arg)
#    add_library(${arg})
##    target_link_libraries(${name} ${llvm_libs})
#endfunction(add_swift_library)


include_directories(include)
add_subdirectory(lib)
#link_directories(lib)


add_library(swift STATIC lib)
#add_executable(swift main.cpp)


target_link_libraries(swift ${llvm_libs})