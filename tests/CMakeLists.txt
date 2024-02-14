set(TEST_LIBS lvm2_s ${LUA_LIBRARIES} glog gtest)

set(target test1)
add_executable(${target} ${target}.cpp)
target_link_libraries(${target} ${TEST_LIBS})
add_test(NAME ${target} COMMAND ${target})
set_tests_properties(${target} PROPERTIES LABELS ${target})



#set(target test2)
#add_executable(${target} ${target}.cpp)
#target_link_libraries(${target} ${TEST_LIBS})
#add_test(NAME ${target} COMMAND ${target})
#set_tests_properties(${target} PROPERTIES LABELS ${target})

