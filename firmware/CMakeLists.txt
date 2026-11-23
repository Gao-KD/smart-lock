# Host build of the lock firmware logic (hardware-independent parts + host HAL).
add_library(lock_logic STATIC
    lock/core/lock_core.c
    lock/credential/credential.c
    lock/credential/offline_pwd.c
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
target_link_libraries(lock_hal_host PUBLIC slp slp_crypto_host)
target_compile_options(lock_hal_host PRIVATE -Wall -Wextra -Werror)

add_executable(lock_host_demo lock/app/main_host.c)
target_link_libraries(lock_host_demo PRIVATE lock_logic lock_hal_host slp_crypto_host)

if(SMARTLOCK_BUILD_TESTS)
    add_executable(lock_tests lock/tests/test_lock.c)
    target_include_directories(lock_tests PRIVATE ${CMAKE_SOURCE_DIR}/common/slp/tests)
    target_link_libraries(lock_tests PRIVATE lock_logic lock_hal_host slp_crypto_host)
    target_compile_options(lock_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME lock_tests COMMAND lock_tests)

    # The EFR32 board pin map, checked against the FG28 QFN68 facts.
    add_executable(board_tests lock/tests/test_board.c)
    target_include_directories(board_tests PRIVATE ${CMAKE_SOURCE_DIR}/common/slp/tests lock/port/efr32)
    target_compile_options(board_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME board_tests COMMAND board_tests)
endif()

# PC simulator: hundreds of virtual locks running this firmware logic, the
# simulated 433 MHz air and radio co-processors (see port/sim/locksim.c).
if(UNIX)
    add_executable(locksim
        lock/port/sim/locksim.c
        lock/port/sim/hal_sim.c
        lock/port/sim/sim_net.c
    )
    target_include_directories(locksim PRIVATE lock/port/sim)
    target_link_libraries(locksim PRIVATE lock_logic slp_crypto_host)
    target_compile_options(locksim PRIVATE -Wall -Wextra -Werror)
endif()

if(SMARTLOCK_BUILD_TESTS)
    add_executable(sim_net_tests lock/tests/test_sim_net.c lock/port/sim/sim_net.c)
    target_include_directories(sim_net_tests PRIVATE ${CMAKE_SOURCE_DIR}/common/slp/tests lock/port/sim)
    target_link_libraries(sim_net_tests PRIVATE slp slp_crypto_host)
    target_compile_options(sim_net_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME sim_net_tests COMMAND sim_net_tests)
endif()
