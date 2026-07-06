cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aquahmet)

# Build LittleFS image from web/ directory and flash it together with the app.
# The image is written to the 'littlefs' partition defined in partitions.csv.
# In P6 this directory will contain the minified+gzipped SPA; for P5 it holds
# the placeholder index.html that exercises the WebSocket API.
littlefs_create_partition_image(littlefs ${CMAKE_CURRENT_SOURCE_DIR}/web FLASH_IN_PROJECT)
