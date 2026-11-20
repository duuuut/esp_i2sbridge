cmake_minimum_required(VERSION 3.16)

# Standalone on purpose: this bridge uses nothing from wt_bsp (which has no I2S
# support anyway -- it covers the RGB LED, the button, the camera and USB).
# Add the BSP later only if a status light is wanted.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nam_i2s_bridge)
