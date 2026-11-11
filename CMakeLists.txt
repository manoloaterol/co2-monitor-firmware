# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Firmware version. version.txt holds the version being worked on, and a
# release is the commit tagged v<version>. Only a build of exactly that commit,
# with no uncommitted changes, gets the bare version; everything else is marked
# as a development build with the commit it came from, so two different images
# never report the same version:
#
#   1.3.0                     the tagged commit, clean
#   1.3.0-dev+abc1234         any other commit
#   1.3.0-dev+abc1234.dirty   with uncommitted changes
#
# After tagging a release, bump version.txt to the next version.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" FW_BASE_VERSION LIMIT_COUNT 1)
string(STRIP "${FW_BASE_VERSION}" FW_BASE_VERSION)

find_package(Git QUIET)
set(FW_TAGGED FALSE)
set(FW_HASH "")
set(FW_DIRTY "")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --match "v${FW_BASE_VERSION}"
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    RESULT_VARIABLE fw_not_tagged OUTPUT_QUIET ERROR_QUIET)
    if(fw_not_tagged EQUAL 0)
        set(FW_TAGGED TRUE)
    endif()
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE FW_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE fw_changes ERROR_QUIET)
    if(NOT fw_changes STREQUAL "")
        set(FW_DIRTY ".dirty")
    endif()

    # Git state is read when CMake configures, so these make a commit, a
    # checkout, a staged change or a new tag trigger a new configure. An edit
    # that is not staged yet does not, and the ".dirty" mark can lag until the
    # next one. The paths come from git rather than being spelled as .git/...,
    # which is a file, not a directory, in a worktree or a submodule.
    foreach(fw_git_file HEAD index packed-refs refs/tags)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --git-path ${fw_git_file}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                        OUTPUT_VARIABLE fw_git_path OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
        if(NOT IS_ABSOLUTE "${fw_git_path}")
            set(fw_git_path "${CMAKE_CURRENT_LIST_DIR}/${fw_git_path}")
        endif()
        # A dependency that does not exist would make every build reconfigure.
        if(EXISTS "${fw_git_path}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fw_git_path}")
        endif()
    endforeach()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/version.txt")

if(FW_TAGGED AND FW_DIRTY STREQUAL "")
    set(PROJECT_VER "${FW_BASE_VERSION}")
elseif(FW_HASH STREQUAL "")
    set(PROJECT_VER "${FW_BASE_VERSION}-dev")
else()
    set(PROJECT_VER "${FW_BASE_VERSION}-dev+${FW_HASH}${FW_DIRTY}")
endif()
message(STATUS "Firmware version: ${PROJECT_VER}")

project(esp32-s2-scd30)
