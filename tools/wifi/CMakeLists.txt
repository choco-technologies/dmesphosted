set(DMOD_DMR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/wifi.dmr)

dmod_add_executable(wifi ${DMOD_MODULE_VERSION}
    main.c
)

# dmdrvi: the network ioctl types the device's headers use; dmosi: sleep.
dmod_link_modules(wifi
    dmdrvi@>=2.0
    dmosi
)

target_link_libraries(wifi dmesphosted_if)
