add_library(slp STATIC
    src/frame.c
    src/security.c
    src/app.c
    src/slot.c
)
target_include_directories(slp PUBLIC include)
target_compile_options(slp PRIVATE -Wall -Wextra -Werror)

if(SMARTLOCK_BUILD_TESTS)
    add_executable(slp_tests tests/test_slp.c)
    target_link_libraries(slp_tests PRIVATE slp)
    target_compile_options(slp_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME slp_tests COMMAND slp_tests)
endif()
