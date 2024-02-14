cmake_minimum_required(VERSION 3.10)

project(lib-lvm2)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Wall -Wpedantic -Wreturn-type -g -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wall -Wpedantic -Wreturn-type -g")

#SET(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)
#SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/bin)

add_definitions(-g -D_FILE_OFFSET_BITS=64 -D_REENTRANT -DEXTERNAL_LOCKS -DMULTITHREAD)
add_definitions(-fPIC)
add_definitions(-Wno-deprecated)

# Find the Lua package
find_package(Lua REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include ${LUA_INCLUDE_DIR})

file(GLOB_RECURSE SRC_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

add_library( lvm2_s STATIC ${SRC_FILES} )
add_library( lvm2 SHARED ${SRC_FILES} )

INSTALL(DIRECTORY ${CMAKE_SOURCE_DIR}/include/ DESTINATION include)

include(CTest)
enable_testing()
add_subdirectory(tests)
add_subdirectory(examples)

