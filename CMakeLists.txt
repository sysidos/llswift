cmake_minimum_required(VERSION 3.16)
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
        bitreader bitwriter lto ipo option config)






string(REGEX REPLACE "([0-9]+)\\.[0-9]+(\\.[0-9]+)?" "\\1" SWIFT_VERSION_MAJOR
        ${SWIFT_VERSION})
string(REGEX REPLACE "[0-9]+\\.([0-9]+)(\\.[0-9]+)?" "\\1" SWIFT_VERSION_MINOR
        ${SWIFT_VERSION})

set_property(SOURCE lib/Basic/Version.cpp APPEND_STRING PROPERTY COMPILE_FLAGS
        " -DSWIFT_VERSION=${SWIFT_VERSION} -DSWIFT_VERSION_MAJOR=${SWIFT_VERSION_MAJOR} -DSWIFT_VERSION_MINOR=${SWIFT_VERSION_MINOR}")


configure_file(include/swift/Config.h.in include/swift/Config.h
        ESCAPE_QUOTES @ONLY)


include_directories(include)
add_subdirectory(lib)


add_executable(swift main.cpp)
target_link_libraries(swift ${llvm_libs})
target_link_libraries(swift swiftBasic swiftAST swiftParse swiftSema swiftSerialization swiftSIL)



# On non-Darwin require UUID.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(UUID_INCLUDE "")
  set(UUID_LIBRARIES "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(UUID_INCLUDE "")
  set(UUID_LIBRARIES "ole32.lib")
else()
#   find_package(UUID REQUIRED)
#   set(UUID_INCLUDE "-I${UUID_INCLUDE_DIRS}")

  find_path(LIBUUID_INCLUDE_DIR uuid.h PATH_SUFFIXES uuid)
  find_library(LIBUUID_LIBRARY libuuid.a)
  include_directories(${LIBUUID_INCLUDE_DIR})
  target_link_libraries(swift ${LIBUUID_LIBRARY})
endif()
#
#project(llswift)
#add_executable(llswift main.cpp)
#target_link_libraries(llswift swift)
