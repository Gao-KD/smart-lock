# Host build of the lock firmware logic (hardware-independent parts + host HAL).
add_library(lock_logic STATIC
    lock/core/lock_core.c
    lock/credential/credential.c
    lock/app/lock_app.c
    lock/mac/lock_mac.c
)
target_include_directories(lock_logic PUBLIC
    lock/hal
    lock/core
    lock/credential
    lock/app
    lock/mac
)
target_link_libraries(lock_logic PUBLIC slp)
target_compile_options(lock_logic PRIVATE -Wall -Wextra -Werror)

add_library(lock_hal_host STATIC lock/port/host/hal_host.c)
target_include_directories(lock_hal_host PUBLIC lock/hal lock/port/host)
target_compile_options(lock_hal_host PRIVATE -Wall -Wextra -Werror)

add_executable(lock_host_demo lock/app/main_host.c)
target_link_libraries(lock_host_demo PRIVATE lock_logic lock_hal_host)

if(SMARTLOCK_BUILD_TESTS)
    add_executable(lock_tests lock/tests/test_lock.c)
    target_include_directories(lock_tests PRIVATE ${CMAKE_SOURCE_DIR}/common/slp/tests)
    target_link_libraries(lock_tests PRIVATE lock_logic lock_hal_host)
    target_compile_options(lock_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME lock_tests COMMAND lock_tests)
endif()
