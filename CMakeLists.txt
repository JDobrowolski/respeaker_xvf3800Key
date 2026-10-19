cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(respeaker_xvf3800Key)

# model slowa kluczowego wgrywany do partycji "kws" razem z firmware (idf.py flash)
set(KWS_MODEL "${CMAKE_SOURCE_DIR}/kws/models/punapu.kws")
if(EXISTS ${KWS_MODEL})
    esptool_py_flash_to_partition(flash "kws" "${KWS_MODEL}")
endif()
