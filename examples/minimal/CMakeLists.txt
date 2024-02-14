find_package(Qt6 REQUIRED Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)

file(GLOB_RECURSE SRC_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

add_executable(minimal ${SRC_FILES})
target_link_libraries(minimal lvm2_s ${LUA_LIBRARIES} glog Qt6::Widgets)




