# paimon-cpp ExternalProject, shared by this repo's standalone build and by
# builds that compile these sources into another extension. Include with:
#
#   PAIMON_CPP_SOURCE_DIR      - paimon-cpp checkout
#   PAIMON_CPP_PREFIX          - install prefix for the built libs/headers
#   PAIMON_CPP_PATCHES_DIR     - directory holding the paimon-cpp-*.patch set
#   PAIMON_CPP_EP_ENABLE_JINDO - optional, default ON (JindoSDK / oss://)
#
# Defines: PAIMON_CPP_INCLUDE, PAIMON_CPP_LIB, PAIMON_CPP_LINK_LIBS,
# PAIMON_CPP_RUNTIME_LIBS (shared objects to ship) and the paimon_cpp_ep target.

include(ExternalProject)

if(NOT DEFINED PAIMON_CPP_EP_ENABLE_JINDO)
  set(PAIMON_CPP_EP_ENABLE_JINDO ON)
endif()

set(PAIMON_CPP_INCLUDE ${PAIMON_CPP_PREFIX}/include)
set(PAIMON_CPP_LIB ${PAIMON_CPP_PREFIX}/lib64)
file(MAKE_DIRECTORY ${PAIMON_CPP_INCLUDE})

# JindoSDK: the installed filename differs by platform, but bundling always
# uses the short name that matches the dylib's install_name.
if(PAIMON_CPP_EP_ENABLE_JINDO)
  file(STRINGS ${PAIMON_CPP_SOURCE_DIR}/third_party/versions.txt
       _jindosdk_ver REGEX "^PAIMON_JINDOSDK_C_BUILD_VERSION=")
  string(REGEX REPLACE ".*=" "" _jindosdk_ver "${_jindosdk_ver}")
  string(REGEX REPLACE "([0-9]+)\\..*" "\\1" _jindosdk_major "${_jindosdk_ver}")
  if(APPLE)
    set(JINDOSDK_C_INSTALLED_NAME "libjindosdk_c.${_jindosdk_ver}.dylib")
    set(JINDOSDK_C_RUNTIME_NAME "libjindosdk_c.${_jindosdk_major}.dylib")
  else()
    set(JINDOSDK_C_INSTALLED_NAME "libjindosdk_c.so")
    set(JINDOSDK_C_RUNTIME_NAME "libjindosdk_c.so.${_jindosdk_major}")
  endif()
endif()

set(PAIMON_CPP_PATCHES
  ${PAIMON_CPP_PATCHES_DIR}/paimon-cpp-install-arrow-headers.patch
  ${PAIMON_CPP_PATCHES_DIR}/paimon-cpp-macos-ndebug-for-checked-cast.patch
  ${PAIMON_CPP_PATCHES_DIR}/paimon-cpp-disable-global-constructors-warning.patch
  ${PAIMON_CPP_PATCHES_DIR}/paimon-cpp-disable-re2-tests.patch
  ${PAIMON_CPP_PATCHES_DIR}/paimon-cpp-ofs-not-object-store.patch)

# Apply each patch idempotently. Subshell parens keep the || within each step
# (and need no ';', which CMake would split as a list): without grouping a
# failed apply is short-circuited into the NEXT step's apply branch and the
# command exits 0 on an unpatched tree.
set(PAIMON_CPP_PATCH_SCRIPT "true")
foreach(patch IN LISTS PAIMON_CPP_PATCHES)
  string(APPEND PAIMON_CPP_PATCH_SCRIPT
    " && (git -C ${PAIMON_CPP_SOURCE_DIR} apply --reverse --check ${patch} 2>/dev/null || git -C ${PAIMON_CPP_SOURCE_DIR} apply ${patch})")
endforeach()

# The paimon shared libs, in link order. Format/index/fs libs sit between
# --no-as-needed/--as-needed on Linux: nothing references them directly, they
# register factories via global constructors.
set(_paimon_cpp_solibs
  ${PAIMON_CPP_LIB}/libpaimon_file_index${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${PAIMON_CPP_LIB}/libpaimon_global_index${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${PAIMON_CPP_LIB}/libpaimon_local_file_system${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${PAIMON_CPP_LIB}/libpaimon_parquet_file_format${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${PAIMON_CPP_LIB}/libpaimon_orc_file_format${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${PAIMON_CPP_LIB}/libpaimon_avro_file_format${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${PAIMON_CPP_LIB}/libpaimon_blob_file_format${CMAKE_SHARED_LIBRARY_SUFFIX})
if(PAIMON_CPP_EP_ENABLE_JINDO)
  list(APPEND _paimon_cpp_solibs
    ${PAIMON_CPP_LIB}/libpaimon_jindo_file_system${CMAKE_SHARED_LIBRARY_SUFFIX}
    ${PAIMON_CPP_LIB}/${JINDOSDK_C_INSTALLED_NAME})
endif()

ExternalProject_Add(paimon_cpp_ep
  SOURCE_DIR ${PAIMON_CPP_SOURCE_DIR}
  PATCH_COMMAND sh -c "${PAIMON_CPP_PATCH_SCRIPT}"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_INSTALL_PREFIX=${PAIMON_CPP_PREFIX}
    -DCMAKE_INSTALL_LIBDIR=lib64
    -DLIBUNWIND_LIBRARY=FALSE
    -DPAIMON_DEPENDENCY_SOURCE=BUNDLED
    -DPAIMON_BUILD_TESTS=OFF
    -DPAIMON_BUILD_SHARED=ON
    -DPAIMON_BUILD_STATIC=OFF
    -DPAIMON_ENABLE_JINDO=${PAIMON_CPP_EP_ENABLE_JINDO}
    -DPAIMON_ENABLE_ORC=ON
    -DPAIMON_ENABLE_AVRO=ON
    -DPAIMON_ENABLE_LUMINA=OFF
    -DPAIMON_ENABLE_LUCENE=OFF
  BUILD_BYPRODUCTS
    ${PAIMON_CPP_LIB}/libpaimon${CMAKE_SHARED_LIBRARY_SUFFIX}
    ${_paimon_cpp_solibs}
)

# On macOS, paimon-cpp installs the JindoSDK dylib with a versioned filename
# (e.g. libjindosdk_c.6.10.2.dylib) but its install_name uses the short
# version (libjindosdk_c.6.dylib). Provide the short-named copy runtime
# lookup expects.
if(APPLE AND PAIMON_CPP_EP_ENABLE_JINDO)
  ExternalProject_Add_Step(paimon_cpp_ep jindosdk_short_name
    DEPENDEES install
    BYPRODUCTS ${PAIMON_CPP_LIB}/${JINDOSDK_C_RUNTIME_NAME}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${PAIMON_CPP_LIB}/${JINDOSDK_C_INSTALLED_NAME}
      ${PAIMON_CPP_LIB}/${JINDOSDK_C_RUNTIME_NAME})
endif()

set(PAIMON_CPP_LINK_LIBS
  ${PAIMON_CPP_LIB}/libpaimon${CMAKE_SHARED_LIBRARY_SUFFIX}
  $<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,--no-as-needed>
  ${_paimon_cpp_solibs}
  $<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,--as-needed>)

# Shipping uses the runtime (soname) JindoSDK filename, not the installed one.
set(PAIMON_CPP_RUNTIME_LIBS
  ${PAIMON_CPP_LIB}/libpaimon${CMAKE_SHARED_LIBRARY_SUFFIX}
  ${_paimon_cpp_solibs})
if(PAIMON_CPP_EP_ENABLE_JINDO)
  list(REMOVE_ITEM PAIMON_CPP_RUNTIME_LIBS ${PAIMON_CPP_LIB}/${JINDOSDK_C_INSTALLED_NAME})
  list(APPEND PAIMON_CPP_RUNTIME_LIBS ${PAIMON_CPP_LIB}/${JINDOSDK_C_RUNTIME_NAME})
endif()
