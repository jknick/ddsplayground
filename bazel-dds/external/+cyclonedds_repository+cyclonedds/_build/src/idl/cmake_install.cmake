# Install script for directory: /Users/jknick/Library/Caches/bazel/_bazel_jknick/51cb5a03bd06a36ee3e16e84ef3df93d/external/+cyclonedds_repository+cyclonedds/src/idl

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/Users/jknick/Library/Caches/bazel/_bazel_jknick/51cb5a03bd06a36ee3e16e84ef3df93d/external/+cyclonedds_repository+cyclonedds/_install")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE DIRECTORY FILES
    "/Users/jknick/Library/Caches/bazel/_bazel_jknick/51cb5a03bd06a36ee3e16e84ef3df93d/external/+cyclonedds_repository+cyclonedds/src/idl/include/idl"
    "/Users/jknick/Library/Caches/bazel/_bazel_jknick/51cb5a03bd06a36ee3e16e84ef3df93d/external/+cyclonedds_repository+cyclonedds/_build/src/idl/include/idl"
    FILES_MATCHING REGEX "/[^/]*\\.h$")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES
    "/Users/jknick/Library/Caches/bazel/_bazel_jknick/51cb5a03bd06a36ee3e16e84ef3df93d/external/+cyclonedds_repository+cyclonedds/_build/lib/libcycloneddsidl.0.10.5.dylib"
    "/Users/jknick/Library/Caches/bazel/_bazel_jknick/51cb5a03bd06a36ee3e16e84ef3df93d/external/+cyclonedds_repository+cyclonedds/_build/lib/libcycloneddsidl.0.dylib"
    )
  foreach(file
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcycloneddsidl.0.10.5.dylib"
      "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcycloneddsidl.0.dylib"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      execute_process(COMMAND /usr/bin/install_name_tool
        -add_rpath "@loader_path/../lib"
        "${file}")
      if(CMAKE_INSTALL_DO_STRIP)
        execute_process(COMMAND "/usr/bin/strip" -x "${file}")
      endif()
    endif()
  endforeach()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE SHARED_LIBRARY FILES "/Users/jknick/Library/Caches/bazel/_bazel_jknick/51cb5a03bd06a36ee3e16e84ef3df93d/external/+cyclonedds_repository+cyclonedds/_build/lib/libcycloneddsidl.dylib")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/jknick/Library/Caches/bazel/_bazel_jknick/51cb5a03bd06a36ee3e16e84ef3df93d/external/+cyclonedds_repository+cyclonedds/_build/src/idl/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
