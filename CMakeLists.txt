# This file specifies how the project should be built, using CMake.
# If you are unfamiliar with CMake, don't worry about all the details.
# The sections you might want to edit are marked as such, and
# the comments should hopefully make most of it clear.
#
# For many purposes, you may not need to change anything about this file.

cmake_minimum_required(VERSION 3.14)
project(CPP_LEARN VERSION 1.2.3.4)
set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -g")
# Set project name, version and languages here. (change as needed)
# Version numbers are available by including "exampleConfig.h" in
# the source. See exampleConfig.h.in for some more details.



configure_file (
  "${PROJECT_SOURCE_DIR}/include/exampleConfig.h.in"
  "${PROJECT_BINARY_DIR}/exampleConfig.h"
)
# Add the binary tree to the search path for include files
# so that we will find exampleConfig.h
include_directories("${PROJECT_BINARY_DIR}")

# Ask CMake to output a compile_commands.json file for use with things like Vim YCM.
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)


# Include stuff. No change is needed.
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
# --------------------------------------------------------------------------------
#                         Locate files (change as needed).
# --------------------------------------------------------------------------------
set(SOURCES src/calculator.cpp src/ffmpeg.cpp src/utils/error.cpp src/utils/queue.cpp)
set(LIBRARY_NAME engine)  # Default name for the library built from src/*.cpp (change if you wish)

# --------------------------------------------------------------------------------
#                            Build! (Change as needed)
# --------------------------------------------------------------------------------
# Compile all sources into a library.
#
set(requiredlibs)
add_library(${LIBRARY_NAME} OBJECT ${SOURCES})
list(APPEND requiredlibs ${LIBRARY_NAME})

find_package( OpenCV REQUIRED )
include_directories( ${OpenCV_INCLUDE_DIRS} )
list(APPEND requiredlibs ${OpenCV_LIBS})

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavdevice
    libavfilter
    libavformat
    libavcodec
    libswresample
    libswscale
    libavutil
)

list(APPEND requiredlibs PkgConfig::LIBAV)

include_directories( ${LIBAV_INCLUDE_DIRS} )
# Lib needs its header files, and users of the library must also see these (PUBLIC). (No change needed)


find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-curlpp CONFIG REQUIRED)
find_package(unofficial-inih CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

list(APPEND requiredLibs
  CURL::libcurl
  unofficial::curlpp::curlpp
  unofficial::inih::inireader
  nlohmann_json::nlohmann_json
  pugixml
  OpenSSL::SSL OpenSSL::Crypto
)

find_library(MINIOCPP_LIBRARIES miniocpp)
find_path(MINIOCPP_INCLUDE_DIRS miniocpp/client.h)
include_directories( ${MINIOCPP_INCLUDE_DIRS} )

list(APPEND requiredlibs ${MINIOCPP_LIBRARIES})

target_include_directories(${LIBRARY_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/include)
message(STATUS "Found required libs: ${requiredlibs}")


# Add an executable for the file app/main.cpp.
# If you add more executables, copy these lines accordingly.
add_executable(main app/main.cpp)   # Name of exec. and location of file.
target_link_libraries(main ${requiredlibs})
#
# Set the properties you require, e.g. what C++ standard to use. Here applied to the library and main (change as needed).
set_target_properties(
    ${LIBRARY_NAME} main
      PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

