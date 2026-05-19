# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-src")
  file(MAKE_DIRECTORY "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-src")
endif()
file(MAKE_DIRECTORY
  "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-build"
  "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix"
  "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/tmp"
  "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src/googlebenchmark-populate-stamp"
  "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src"
  "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src/googlebenchmark-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src/googlebenchmark-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/ericzhou/Desktop/Productivity/Projects/Serious/MatchingEngine/build-release/_deps/googlebenchmark-subbuild/googlebenchmark-populate-prefix/src/googlebenchmark-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
