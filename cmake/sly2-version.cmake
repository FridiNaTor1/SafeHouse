# Writes OUTPUT: the Sly 2 port's version string for the debug menu's title line (Sly2-Release/debugmenu.cpp), the
# same the launcher shows (launcher/update.h describe()): the release version CI builds (RELEASE: the cache variable
# SAFEHOUSE_VERSION, x.y.z, e.g. 0.1.29; else the environment variable SAFEHOUSE_VERSION at build time) and the short
# git hash of ROOT's checkout ("-dirty" when it has uncommitted changes; "unknown" without git): "0.1.29, 1a2b3c4d".
# Without a release version it is a development build: "development build, 1a2b3c4d". (Until 2026-11-10 it was the
# CMake project version, VERSION, so every release read 0.1.0.) The file is only rewritten when the string changes, so a build
# with nothing new recompiles nothing. Run at every build (Sly2-Release/CMakeLists.txt, target sly2_version).
#
# A hash given from outside wins over git: HASH (the cache variable CANE_GIT_HASH, -DCANE_GIT_HASH=... at configure
# time), else the environment variable CANE_GIT_HASH at build time. The packaging container needs it: its git cannot
# resolve the worktree (the worktree's .git file points at the host's repository, which is not mounted), so the
# version read "0.1.0-unknown".
set(hash "")
if(HASH)
  set(hash "${HASH}")
elseif(DEFINED ENV{CANE_GIT_HASH} AND NOT "$ENV{CANE_GIT_HASH}" STREQUAL "")
  set(hash "$ENV{CANE_GIT_HASH}")
endif()
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE AND NOT hash)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${ROOT}" rev-parse --short=8 HEAD
                  OUTPUT_VARIABLE hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    set(hash "")
  endif()
  if(hash)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${ROOT}" diff --quiet HEAD -- RESULT_VARIABLE dirty ERROR_QUIET)
    if(NOT dirty EQUAL 0)
      set(hash "${hash}-dirty")
    endif()
  endif()
endif()
if(NOT hash)
  set(hash "unknown")
endif()
set(release "${RELEASE}")
if(NOT release AND DEFINED ENV{SAFEHOUSE_VERSION})
  set(release "$ENV{SAFEHOUSE_VERSION}")
endif()
if(release AND NOT release MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  set(release "")
endif()
if(release)
  set(shown "${release}")
else()
  set(shown "development build")
endif()
set(text "#pragma once\n#define SLY2_PORT_VERSION \"${shown}, ${hash}\"\n")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" old)
else()
  set(old "")
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUTPUT}" "${text}")
endif()
