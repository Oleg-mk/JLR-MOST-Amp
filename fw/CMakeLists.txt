cmake_minimum_required(VERSION 3.16)
# idf.py -B build_qemu -DQEMU=1 build, then idf.py -B build_qemu qemu: the QEMU variant. Its own sdkconfig in the build
# folder, sdkconfig.qemu on top of the defaults
if(QEMU)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.qemu")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bitline)
