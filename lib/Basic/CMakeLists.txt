# On non-Darwin require UUID.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(UUID_INCLUDE "")
        set(UUID_LIBRARIES "")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(UUID_INCLUDE "")
        set(UUID_LIBRARIES "ole32.lib")
else()
        find_package(UUID REQUIRED)
        set(UUID_INCLUDE "-I${UUID_INCLUDE_DIRS}")
endif()

add_library(
        swiftBasic STATIC
        Version.cpp
        UUID.cpp
        )