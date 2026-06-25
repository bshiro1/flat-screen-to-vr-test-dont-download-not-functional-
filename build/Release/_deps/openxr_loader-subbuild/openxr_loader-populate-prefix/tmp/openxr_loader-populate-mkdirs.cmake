# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-src")
  file(MAKE_DIRECTORY "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-src")
endif()
file(MAKE_DIRECTORY
  "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-build"
  "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-subbuild/openxr_loader-populate-prefix"
  "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-subbuild/openxr_loader-populate-prefix/tmp"
  "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-subbuild/openxr_loader-populate-prefix/src/openxr_loader-populate-stamp"
  "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-subbuild/openxr_loader-populate-prefix/src"
  "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-subbuild/openxr_loader-populate-prefix/src/openxr_loader-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-subbuild/openxr_loader-populate-prefix/src/openxr_loader-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/shiro/vr-game-converter/flat screen to vr (test dont download not functional)/build/Release/_deps/openxr_loader-subbuild/openxr_loader-populate-prefix/src/openxr_loader-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
