project(eta)


# Specify dependencies.
if(POLICY CMP0167)
    # Boost config/bjam/vcpkg is generally a mess,
    # until Boost gets rid of bjam, set this policy
    # to OLD.
    cmake_policy(SET CMP0167 OLD)
endif()

find_package(Boost 1.88 REQUIRED COMPONENTS unit_test_framework)
if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost not found.")
endif()

add_subdirectory(core) 
add_subdirectory(test)
add_subdirectory(fuzz)