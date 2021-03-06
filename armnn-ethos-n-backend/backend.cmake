#
# Copyright © 2018-2020 Arm Limited. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#

if(ETHOSN_SUPPORT)
    # Add the Ethos-N support library
    find_path(SUPPORT_LIBRARY_INCLUDE_DIR ethosn_support_library/Support.hpp
              HINTS ${ETHOSN_ROOT}/include)
    include_directories(${SUPPORT_LIBRARY_INCLUDE_DIR})

    set(LIB libEthosNSupport.so)
    find_library(ETHOSN_SUPPORT_LIBRARY
                 NAMES ${LIB}
                 HINTS ${ETHOSN_ROOT}/lib)
    if(NOT ETHOSN_SUPPORT_LIBRARY)
        message(WARNING "Ethos-N support library (${LIB}) not found")
    else()
        message(STATUS "Ethos-N support library located at: ${ETHOSN_SUPPORT_LIBRARY}")
        link_libraries(${ETHOSN_SUPPORT_LIBRARY})
    endif()

    # Add the Ethos-N driver library
    find_path(DRIVER_LIBRARY_INCLUDE_DIR ethosn_driver_library/Network.hpp
              HINTS ${ETHOSN_ROOT}/include)
    include_directories(${DRIVER_LIBRARY_INCLUDE_DIR})

    set(LIB libEthosNDriver.so)
    find_library(ETHOSN_DRIVER_LIBRARY
                 NAMES ${LIB}
                 HINTS ${ETHOSN_ROOT}/lib)
    if(NOT ETHOSN_DRIVER_LIBRARY)
        message(WARNING "Ethos-N driver library (${LIB}) not found")
    else()
        message(STATUS "Ethos-N driver library located at: ${ETHOSN_DRIVER_LIBRARY}")
        link_libraries(${ETHOSN_DRIVER_LIBRARY})
    endif()
    add_definitions(-DETHOSN_SUPPORT_ENABLED)

    # Build the Arm NN Ethos-N backend
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR})
    list(APPEND armnnLibraries armnnEthosNBackend)
    list(APPEND armnnLibraries armnnEthosNBackendWorkloads)
    if(BUILD_UNIT_TESTS)
        list(APPEND armnnUnitTestLibraries armnnEthosNBackendUnitTests)
    endif()
endif()
