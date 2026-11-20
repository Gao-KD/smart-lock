add_library(slp STATIC
    src/frame.c
    src/security.c
    src/app.c
    src/slot.c
    src/secure.c
)
target_include_directories(slp PUBLIC include)
target_compile_options(slp PRIVATE -Wall -Wextra -Werror)

# Software AES-128 / CCM* backend for host builds (tests, simulator).
# On EFR32 the crypto port is implemented with PSA Crypto instead.
add_library(slp_crypto_host STATIC port/host/crypto_host.c)
target_link_libraries(slp_crypto_host PUBLIC slp)
target_compile_options(slp_crypto_host PRIVATE -Wall -Wextra -Werror)

if(SMARTLOCK_BUILD_TESTS)
    add_executable(slp_tests tests/test_slp.c)
    target_link_libraries(slp_tests PRIVATE slp slp_crypto_host)
    target_compile_options(slp_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME slp_tests COMMAND slp_tests)

    add_executable(slp_secure_tests tests/test_secure.c)
    target_link_libraries(slp_secure_tests PRIVATE slp slp_crypto_host)
    target_compile_options(slp_secure_tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME slp_secure_tests COMMAND slp_secure_tests)
endif()
