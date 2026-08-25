find_package(Threads REQUIRED)

add_unit_test(
    region_test
    CATCH2
    FILES
    test/region_test.cpp
    INCLUDE_DIRECTORIES
    ..
    LIBRARIES
    Threads::Threads
    SYSTEM_LIBRARIES
    async
)

