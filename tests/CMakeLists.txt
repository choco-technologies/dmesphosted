set(DMOD_MODULE_NAME        test_dmesphosted)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         4096)
set(DMOD_PRIORITY           0)

# The frame, protobuf, RPC and bring-up code is compiled in here too and
# tested as it is; the driver itself through dmdrvi.
dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmesphosted_test.c
    ../src/frame.c
    ../src/pb.c
    ../src/rpc_msg.c
    ../src/priv.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(${DMOD_MODULE_NAME} dmesphosted_if)

# dmdrvi: the DIF the tests call dmesphosted through (like dmdevfs); dmini:
# its configuration.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.9
    dmini
    dmnetif
    dmroute
)

# Register the test module with ctest - this replays the same two steps CI
# runs: install the test module's dependencies with dmf-get, then execute it
# through dmod_loader. The test's exit code equals its number of failed
# steps (see dmod_add_test).
find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND ${CMAKE_COMMAND} -E env DMOD_DMF_DIR=${DMOD_DMF_DIR}
            bash -c "${DMF_GET_EXECUTABLE} install -d ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}-local.dmd -y && ${DMOD_LOADER_EXECUTABLE} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf"
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()
