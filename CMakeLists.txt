cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aquahmet)

# Build LittleFS image from web/dist/ and flash it together with the app.
#
# web/dist/ contains the pre-built SPA (committed to the repo as a single-file
# standalone HTML; replaced by the optimised Vite build after `npm run build`).
#
# Source SPA is in web/src/ (Vite + uPlot).  Build flow:
#   cd web && npm install && npm run build   → writes web/dist/
#   idf.py build                             → packages web/dist/ into LittleFS
#
# The dist/index.html pre-built standalone works immediately without npm build.
littlefs_create_partition_image(littlefs ${CMAKE_CURRENT_SOURCE_DIR}/web/dist FLASH_IN_PROJECT)
