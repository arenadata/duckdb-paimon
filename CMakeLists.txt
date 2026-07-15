cmake_minimum_required(VERSION 3.17)

# Set extension name here
set(TARGET_NAME paimon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

# paimon-cpp ExternalProject (also consumed by builds that compile these
# sources into another extension - keep the fragment self-contained).
set(PAIMON_CPP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/paimon-cpp)
set(PAIMON_CPP_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/paimon-cpp-install)
set(PAIMON_CPP_PATCHES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/patches)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/paimon_cpp_ep.cmake)

# Extension sources
set(EXTENSION_SOURCES
  src/paimon_extension.cpp
  src/paimon_functions.cpp
  src/paimon_storage/duckdb_vfs_file_system.cpp
  src/paimon_storage/paimon_scan.cpp
  src/paimon_storage/paimon_snapshots.cpp
  src/paimon_storage/paimon_catalog.cpp
  src/paimon_storage/paimon_insert.cpp
  src/paimon_storage/paimon_schema_entry.cpp
  src/paimon_storage/paimon_schema_set.cpp
  src/paimon_storage/paimon_table_entry.cpp
  src/paimon_storage/paimon_table_set.cpp
  src/paimon_storage/paimon_transaction_manager.cpp
  src/paimon_storage/paimon_type_utils.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Include paimon-cpp and arrow headers
target_include_directories(${EXTENSION_NAME} SYSTEM PRIVATE ${PAIMON_CPP_INCLUDE})
target_include_directories(${TARGET_NAME}_loadable_extension SYSTEM PRIVATE ${PAIMON_CPP_INCLUDE})

# Build paimon-cpp before building the extension
add_dependencies(${EXTENSION_NAME} paimon_cpp_ep)
add_dependencies(${TARGET_NAME}_loadable_extension paimon_cpp_ep)

# Link with paimon-cpp
target_link_libraries(${EXTENSION_NAME} ${PAIMON_CPP_LINK_LIBS})
target_link_libraries(${TARGET_NAME}_loadable_extension ${PAIMON_CPP_LINK_LIBS})

if(APPLE)
  # Propagate rpath so unittest/shell can find paimon-cpp shared libraries.
  target_link_options(${EXTENSION_NAME} INTERFACE "LINKER:-rpath,${PAIMON_CPP_LIB}")
  set(PAIMON_RPATH "@loader_path")
else()
  set(PAIMON_RPATH "\$ORIGIN")
endif()

# Bundle paimon-cpp runtime dependencies next to paimon.duckdb_extension.
foreach(lib IN LISTS PAIMON_CPP_RUNTIME_LIBS)
  add_custom_command(
    TARGET ${TARGET_NAME}_loadable_extension
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${lib}
      $<TARGET_FILE_DIR:${TARGET_NAME}_loadable_extension>
  )
endforeach()
set_target_properties(${TARGET_NAME}_loadable_extension PROPERTIES
  BUILD_RPATH "${PAIMON_RPATH}"
  INSTALL_RPATH "${PAIMON_RPATH}"
  BUILD_WITH_INSTALL_RPATH ON
  INSTALL_RPATH_USE_LINK_PATH OFF
)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
