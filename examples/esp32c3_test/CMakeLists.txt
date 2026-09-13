# A standalone ESP-IDF project, built with idf.py through scripts/idf.ps1 - not
# an add_subdirectory() target of the host build. The host CMakeLists.txt
# compiles the library for POSIX; this one compiles it as an IDF component,
# which is what platform/esp_idf is for. The two never meet.

cmake_minimum_required(VERSION 3.16)

# The library is pulled in as a component rather than copied: the repository
# root IS the component - idf_component.yml sits there - so this points one
# directory at it instead of at the examples/ tree it happens to live under.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

# Order matters: project.cmake must come after EXTRA_COMPONENT_DIRS is set, or
# the component is never found.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# CXX is named because main/ is C++, the library is C++23, and project() is
# where a project declares what it actually contains. IDF still builds its own
# components as C - that is IDF's business, not this list's.
project(esp32c3_test CXX)
