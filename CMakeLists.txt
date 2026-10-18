cmake_minimum_required(VERSION 3.18)

# dmesphosted itself has no hardware port, but dmspi and dmgpio (whose
# headers it uses) publish them only inside their per-CPU-family packages -
# the headers are the same for every family, any published one will do.
set(DMOD_CPU_FAMILY "stm32f7" CACHE STRING "CPU family used to fetch the dmspi/dmgpio headers")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED DMOD_MODULE_VERSION)
    set(DMOD_MODULE_VERSION "0.1" CACHE STRING "DMOD module version")
endif()

# If a local dmod checkout is provided, use it instead of fetching from git
if(DEFINED DMOD_DIR AND NOT "${DMOD_DIR}" STREQUAL "")
    set(FETCHCONTENT_SOURCE_DIR_DMOD "${DMOD_DIR}" CACHE PATH "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(
    dmod
    GIT_REPOSITORY https://github.com/choco-technologies/dmod.git
    GIT_TAG        develop
)

set(DMOD_MODE "DMOD_MODULE" CACHE STRING "DMOD build mode")
set(DMOD_BUILD_TESTS OFF CACHE BOOL "Build tests")
set(DMOD_BUILD_EXAMPLES OFF CACHE BOOL "Build examples")
set(DMOD_BUILD_TOOLS OFF CACHE BOOL "Build tools")
set(DMOD_BUILD_TEMPLATES OFF CACHE BOOL "Build templates")

FetchContent_MakeAvailable(dmod)

project(dmesphosted VERSION ${DMOD_MODULE_VERSION}
    DESCRIPTION "dmesphosted - Wi-Fi on an ESP-Hosted co-processor, over SPI"
    LANGUAGES C CXX)

set(DMOD_DIR ${dmod_SOURCE_DIR} CACHE PATH "DMOD source directory")
set(DMOD_SCRIPTS_DIR ${DMOD_DIR}/scripts CACHE PATH "DMOD scripts directory")
include(${DMOD_DIR}/paths.cmake)
dmod_setup_external_module()

# Name of the module
set(DMOD_MODULE_NAME        dmesphosted)

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the .dmr file - enables release package creation in build/packages/
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/dmesphosted.dmr)

# List of MAL interfaces implemented by this module (leave empty if none)
set(DMOD_MAL_IMPLS          )

# List of DIF interfaces implemented by this module (leave empty if none)
set(DMOD_DIF_IMPLS          )

dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/dmesphosted.c
    src/bus.c
    src/rpc.c
    src/wifi.c
    src/queue.c
    src/frame.c
    src/pb.c
    src/rpc_msg.c
    src/priv.c
)

# dmdrvi:  the driver interface (src/dmesphosted.c), with friends and the
#          network ioctls (DMDRVI_IOCTL_NET_*).
# dmini:   .ini configuration.
# dmnetif: the interface registers itself (dmnetif_register()); dmroute:
#          the headers dmnetif's include.
# dmspi, dmgpio: headers only - the bus and the pins are reached through
#          their nodes (Dmod_FileOpen/Dmod_Ioctl), with their ioctl numbers
#          and types.
# dmhaman: the pins' dmgpio interrupt handlers are dispatched by name.
# dmosi:   threads, locks, semaphores, sleep and tick count.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.9
    dmini
    dmnetif
    dmroute
    dmspi
    dmgpio
    dmhaman
    dmosi
)

# dmheap: on a target only - dmod-boot has it built in (its heaps by name);
# on the host dmod_loader does not enable it, and the regular allocator does
if(NOT DMOD_TOOLS_NAME MATCHES "x86")
    dmod_link_modules(${DMOD_MODULE_NAME} dmheap)
    target_compile_definitions(${DMOD_MODULE_NAME} PRIVATE DMESPHOSTED_HEAPS=1)
endif()

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

enable_testing()
add_subdirectory(tests)
