seekdb_apply_unity_inventory(ob_sql SEEKDB_SQL_UNITY)
seekdb_apply_standalone_inventory(ob_sql SEEKDB_SQL_STANDALONE)
seekdb_apply_standalone_inventory(ob_sql_extra SEEKDB_SQL_EXTRA)

ob_add_new_object_target(ob_sql ob_sql)
ob_add_new_object_target(ob_sql_extra ob_sql_extra)
target_link_libraries(ob_sql PUBLIC ob_base config)
target_link_libraries(ob_sql_extra PUBLIC ob_base_without_pass config)

# Arrow only decodes the projected Parquet columns. SQL runs in seekdb.
set(FILE_SQL_VENDOR_DEPS "${PROJECT_SOURCE_DIR}/deps/3rd/usr/local/oceanbase/deps/devel")
set(FILE_SQL_ARROW_INCLUDES "${FILE_SQL_VENDOR_DEPS}/include/apache-arrow")
set(FILE_SQL_ARROW_LIBDIR "${FILE_SQL_VENDOR_DEPS}/lib64")
set(FILE_SQL_READER "${CMAKE_CURRENT_SOURCE_DIR}/engine/basic/file_scan_utils.cpp")
set(FILE_SQL_BRIDGE "${CMAKE_CURRENT_SOURCE_DIR}/engine/basic/file_parquet_bridge.cpp")
if(EXISTS "${FILE_SQL_ARROW_INCLUDES}/parquet/arrow/reader.h"
   AND EXISTS "${FILE_SQL_ARROW_LIBDIR}/libparquet.a"
   AND EXISTS "${FILE_SQL_ARROW_LIBDIR}/libarrow.a"
   AND EXISTS "${FILE_SQL_ARROW_LIBDIR}/libarrow_bundled_dependencies.a")
  set_property(SOURCE "${FILE_SQL_READER}" APPEND PROPERTY COMPILE_DEFINITIONS SEEKDB_FILE_SQL_ARROW)
  set_property(SOURCE "${FILE_SQL_BRIDGE}" APPEND PROPERTY INCLUDE_DIRECTORIES "${FILE_SQL_ARROW_INCLUDES}")
  set_property(SOURCE "${FILE_SQL_BRIDGE}" APPEND PROPERTY COMPILE_OPTIONS
    -U_GLIBCXX_USE_CXX11_ABI -D_GLIBCXX_USE_CXX11_ABI=0)
  find_library(FILE_SQL_BZIP2_LIBRARY NAMES bz2 libbz2.so.1.0 libbz2.so.1 REQUIRED)
  set(FILE_SQL_PARQUET_LIBS
    "${FILE_SQL_ARROW_LIBDIR}/libparquet.a"
    "${FILE_SQL_ARROW_LIBDIR}/libarrow.a"
    "${FILE_SQL_ARROW_LIBDIR}/libarrow_bundled_dependencies.a"
    "${FILE_SQL_VENDOR_DEPS}/lib/libz.a"
    "${FILE_SQL_BZIP2_LIBRARY}")
  target_link_libraries(ob_sql PUBLIC ${FILE_SQL_PARQUET_LIBS})
elseif(NOT WIN32)
  find_package(Arrow 16 CONFIG QUIET)
  find_package(Parquet 16 CONFIG QUIET)
  if(TARGET Arrow::arrow_shared AND TARGET Parquet::parquet_shared)
    set_property(SOURCE "${FILE_SQL_READER}" APPEND PROPERTY COMPILE_DEFINITIONS SEEKDB_FILE_SQL_ARROW)
    set(FILE_SQL_PARQUET_LIBS Parquet::parquet_shared Arrow::arrow_shared)
    target_link_libraries(ob_sql PUBLIC ${FILE_SQL_PARQUET_LIBS})
  endif()
endif()
if(FILE_SQL_PARQUET_LIBS)
  target_sources(ob_sql PRIVATE "${FILE_SQL_BRIDGE}")
endif()

seekdb_apply_unity_inventory(ob_sql_simd SEEKDB_SQL_SIMD_UNITY)
ob_server_add_target(ob_sql_simd)
if(ARCHITECTURE STREQUAL "x86_64" OR ARCHITECTURE STREQUAL "amd64")
  target_compile_options(ob_sql_simd PRIVATE
    -mtune=core-avx2 -mavx2 -mfma -mbmi2 -mavx512vl -mavx512bw)
endif()

if(WIN32)
  find_program(PWSH_EXE NAMES pwsh pwsh.exe powershell powershell.exe REQUIRED)
  execute_process(
    COMMAND ${PWSH_EXE} -NoProfile -ExecutionPolicy Bypass -File gen_parser_win.ps1
    RESULT_VARIABLE RET
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/src/sql/parser
    OUTPUT_FILE _gen_parser.output
    ERROR_FILE _gen_parser.error)
else()
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env NEED_PARSER_CACHE=${NEED_PARSER_CACHE} bash gen_parser.sh
    RESULT_VARIABLE RET
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/src/sql/parser
    OUTPUT_FILE _gen_parser.output
    ERROR_FILE _gen_parser.error)
endif()
if(RET AND NOT RET EQUAL 0)
  message(FATAL_ERROR
    "Generate sql parser failed; inspect src/sql/parser/_gen_parser.error")
endif()
add_subdirectory(parser)

add_library(ob_sql_static STATIC EXCLUDE_FROM_ALL)
target_link_libraries(ob_sql_static PUBLIC
  ob_sql ob_sql_extra ob_sql_server_parser_static)

option(SEEKDB_FILE_SQL_TESTS "Build the file SQL parser and reader tests" OFF)
if(SEEKDB_FILE_SQL_TESTS AND NOT WIN32)
  set(FILE_SQL_TEST_ROOT "${PROJECT_SOURCE_DIR}/unittest/sql")
  set(FILE_SQL_GTEST_LIBS "${FILE_SQL_VENDOR_DEPS}/lib/libgtest_main.a"
                          "${FILE_SQL_VENDOR_DEPS}/lib/libgtest.a")
  add_executable(test_file_sql_parser EXCLUDE_FROM_ALL
    "${FILE_SQL_TEST_ROOT}/parser/test_file_sql_parser.cpp")
  target_include_directories(test_file_sql_parser SYSTEM PRIVATE "${FILE_SQL_VENDOR_DEPS}/include")
  target_link_libraries(test_file_sql_parser PRIVATE ob_sql_server_parser_static oblib ${FILE_SQL_GTEST_LIBS})
  add_executable(test_file_scan_utils EXCLUDE_FROM_ALL
    "${FILE_SQL_TEST_ROOT}/engine/basic/test_file_scan_utils.cpp" "${FILE_SQL_READER}")
  target_include_directories(test_file_scan_utils PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_include_directories(test_file_scan_utils SYSTEM PRIVATE "${FILE_SQL_VENDOR_DEPS}/include")
  target_compile_features(test_file_scan_utils PRIVATE cxx_std_17)
  target_link_libraries(test_file_scan_utils PRIVATE ${FILE_SQL_GTEST_LIBS}
    ${FILE_SQL_PARQUET_LIBS} pthread ${CMAKE_DL_LIBS})
  if(FILE_SQL_PARQUET_LIBS)
    target_compile_definitions(test_file_scan_utils PRIVATE SEEKDB_FILE_SQL_ARROW)
    target_sources(test_file_scan_utils PRIVATE "${FILE_SQL_BRIDGE}")
  endif()
endif()
