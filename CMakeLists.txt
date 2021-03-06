cmake_minimum_required(VERSION 3.12.0)
project("libndt")

include(CheckCXXCompilerFlag)
include(CheckFunctionExists)
include(CheckIncludeFileCXX)
include(CheckLibraryExists)
include(CheckSymbolExists)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
list(APPEND CMAKE_REQUIRED_LIBRARIES Threads::Threads)
if(("${WIN32}"))
  list(APPEND CMAKE_REQUIRED_LIBRARIES ws2_32 crypt32)
endif()

if((${APPLE}))
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I/usr/local/include")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/local/lib")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -L/usr/local/lib")
endif()

check_function_exists(strtonum LIBNDT_HAVE_STRTONUM)
if(${LIBNDT_HAVE_STRTONUM})
  add_definitions(-DLIBNDT_HAVE_STRTONUM)
endif()

CHECK_INCLUDE_FILE_CXX("curl/curl.h" MK_HAVE_CURL_CURL_H)
if(NOT ("${MK_HAVE_CURL_CURL_H}"))
  message(FATAL_ERROR "cannot find: curl/curl.h")
endif()
CHECK_LIBRARY_EXISTS("curl" "curl_easy_init" "" MK_HAVE_LIBCURL)
if(NOT ("${MK_HAVE_LIBCURL}"))
  message(FATAL_ERROR "cannot find: curl")
endif()
LIST(APPEND CMAKE_REQUIRED_LIBRARIES "curl")

if(("${APPLE}"))
  if(EXISTS "/usr/local/opt/openssl@1.1")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -I/usr/local/opt/openssl@1.1/include")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I/usr/local/opt/openssl@1.1/include")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/local/opt/openssl@1.1/lib")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -L/usr/local/opt/openssl@1.1/lib")
  endif()
endif()
CHECK_INCLUDE_FILE_CXX("openssl/rsa.h" MK_HAVE_OPENSSL_RSA_H)
if(NOT ("${MK_HAVE_OPENSSL_RSA_H}"))
  message(FATAL_ERROR "cannot find: openssl/rsa.h")
endif()
CHECK_LIBRARY_EXISTS("crypto" "RSA_new" "" MK_HAVE_LIBCRYPTO)
if(NOT ("${MK_HAVE_LIBCRYPTO}"))
  message(FATAL_ERROR "cannot find: crypto")
endif()
LIST(APPEND CMAKE_REQUIRED_LIBRARIES "crypto")
CHECK_INCLUDE_FILE_CXX("openssl/ssl.h" MK_HAVE_OPENSSL_SSL_H)
if(NOT ("${MK_HAVE_OPENSSL_SSL_H}"))
  message(FATAL_ERROR "cannot find: openssl/ssl.h")
endif()
CHECK_LIBRARY_EXISTS("ssl" "SSL_read" "" MK_HAVE_LIBSSL)
if(NOT ("${MK_HAVE_LIBSSL}"))
  message(FATAL_ERROR "cannot find: ssl")
endif()
LIST(APPEND CMAKE_REQUIRED_LIBRARIES "ssl")

macro(MKSetRestrictiveCompilerFlags)
  if(("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU") OR
     ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Werror")
    # https://www.owasp.org/index.php/C-Based_Toolchain_Hardening_Cheat_Sheet
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wall")
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wextra")
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wconversion")
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wcast-align")
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wformat=2")
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wformat-security")
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -fno-common")
    # Some options are only supported by GCC when we're compiling C code:
    if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
      set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wmissing-prototypes")
      set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wstrict-prototypes")
    else()
      set(MK_C_FLAGS "${MK_C_FLAGS} -Wmissing-prototypes")
      set(MK_C_FLAGS "${MK_C_FLAGS} -Wstrict-prototypes")
    endif()
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wmissing-declarations")
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wstrict-overflow")
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
      set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -Wtrampolines")
    endif()
    set(MK_CXX_FLAGS "${MK_CXX_FLAGS} -Woverloaded-virtual")
    set(MK_CXX_FLAGS "${MK_CXX_FLAGS} -Wreorder")
    set(MK_CXX_FLAGS "${MK_CXX_FLAGS} -Wsign-promo")
    set(MK_CXX_FLAGS "${MK_CXX_FLAGS} -Wnon-virtual-dtor")
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} -fstack-protector-all")
    if(NOT "${APPLE}" AND NOT "${MINGW}")
      set(MK_LD_FLAGS "${MK_LD_FLAGS} -Wl,-z,noexecstack")
      set(MK_LD_FLAGS "${MK_LD_FLAGS} -Wl,-z,now")
      set(MK_LD_FLAGS "${MK_LD_FLAGS} -Wl,-z,relro")
      set(MK_LD_FLAGS "${MK_LD_FLAGS} -Wl,-z,nodlopen")
      set(MK_LD_FLAGS "${MK_LD_FLAGS} -Wl,-z,nodump")
    elseif(("${MINGW}"))
      set(MK_LD_FLAGS "${MK_LD_FLAGS} -static")
    endif()
    add_definitions(-D_FORTIFY_SOURCES=2)
  elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    # TODO(bassosimone): add support for /Wall and /analyze
    set(MK_COMMON_FLAGS "${MK_COMMON_FLAGS} /WX /W4 /EHs")
    set(MK_LD_FLAGS "${MK_LD_FLAGS} /WX")
  else()
    message(FATAL_ERROR "Compiler not supported: ${CMAKE_CXX_COMPILER_ID}")
  endif()
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${MK_COMMON_FLAGS} ${MK_C_FLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${MK_COMMON_FLAGS} ${MK_CXX_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${MK_LD_FLAGS}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${MK_LD_FLAGS}")
  if("${WIN32}")
    add_definitions(-D_WIN32_WINNT=0x0600) # for NI_NUMERICSERV and WSAPoll
  endif()
endmacro()
MKSetRestrictiveCompilerFlags()

set(MK_LIBNDT_AMALGAMATE_DEST single_include/libndt.hpp)
file(REMOVE ${MK_LIBNDT_AMALGAMATE_DEST})
foreach(SOURCE IN ITEMS
        include/libndt/preamble.hpp
        third_party/github.com/nlohmann/json/json.hpp
        include/libndt/internal/assert.hpp
        include/libndt/internal/sys.hpp
        include/libndt/internal/logger.hpp
        include/libndt/internal/curlx.hpp
        include/libndt/internal/err.hpp
        include/libndt/timeout.hpp
        include/libndt/libndt.hpp)
  file(READ ${SOURCE} CONTENT)
  file(APPEND ${MK_LIBNDT_AMALGAMATE_DEST} "${CONTENT}")
endforeach()

add_definitions(${CMAKE_REQUIRED_DEFINITIONS})
include_directories(${CMAKE_REQUIRED_INCLUDES}
                    ${CMAKE_SOURCE_DIR}
                    ${CMAKE_SOURCE_DIR}/include)

add_executable(curlx_test test/curlx_test.cpp)
target_link_libraries(curlx_test ${CMAKE_REQUIRED_LIBRARIES})

add_executable(libndt-client libndt-client.cpp)
target_link_libraries(libndt-client ${CMAKE_REQUIRED_LIBRARIES})

add_executable(libndt-standalone-builds libndt-standalone-builds.cpp)
target_link_libraries(libndt-standalone-builds ${CMAKE_REQUIRED_LIBRARIES})

add_executable(sys_test test/sys_test.cpp)
target_link_libraries(sys_test ${CMAKE_REQUIRED_LIBRARIES})

add_executable(tests-libndt test/libndt_test.cpp)
target_link_libraries(tests-libndt ${CMAKE_REQUIRED_LIBRARIES})

enable_testing()

add_test(NAME curlx_unit_tests COMMAND curlx_test)
add_test(NAME other_unit_tests COMMAND tests-libndt)
add_test(NAME sys_unit_tests COMMAND sys_test)

add_test(NAME simple_test COMMAND libndt-client
         -download -upload -verbose)
add_test(NAME modern_test COMMAND libndt-client
         -ca-bundle-path ${CMAKE_SOURCE_DIR}/third_party/curl.haxx.se/ca/cacert.pem
         -verbose -download -upload -tls -json -websocket)
