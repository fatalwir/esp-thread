# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Thread network credentials and site settings stay out of git, in
# network.defaults at the repository root (see network.defaults.example).
# Applied after sdkconfig.defaults, so its values win.
set(network_defaults "${CMAKE_CURRENT_LIST_DIR}/../../network.defaults")
if(EXISTS "${network_defaults}")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;${network_defaults}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_ot_br_eth)
