cmake_minimum_required(VERSION 3.12) #using generator $<TARGET_NAME_IF_EXISTS:tgt> which is new in version 3.12.

#
# Suggestion from https://cmake.org/pipermail/cmake/2008-September/023808.html
# If the user specifies -DCMAKE_BUILD_TYPE on the command line, take their definition
# and dump it in the cache along with proper documentation, otherwise set CMAKE_BUILD_TYPE
# to Release prior to calling PROJECT()
#
if(DEFINED CMAKE_BUILD_TYPE)
   SET(CMAKE_BUILD_TYPE ${CMAKE_BUILD_TYPE} CACHE STRING "Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel Coverage.")
else()
   SET(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel.")
endif()
string( TOLOWER "${CMAKE_BUILD_TYPE}" build_type_lower )
message("CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")
if( build_type_lower STREQUAL "debug" )
	message(WARNING "You are building in Debug mode!  HDTN can run order(s) of magnitude slower!")
endif()

# Suggestion from https://gitlab.kitware.com/cmake/cmake/-/issues/19512
# try_compile (check_cxx_source_compiles ) is Debug by default, change to be the build type
set (CMAKE_TRY_COMPILE_CONFIGURATION ${CMAKE_BUILD_TYPE})

#The CMAKE_SYSTEM_PROCESSOR will be set automatically (if not defined) AFTER setting the project name with PROJECT(...)
#To use this cross compile logic, this block is run before the line "project(HDTN.."
if(DEFINED CMAKE_SYSTEM_PROCESSOR)
	if (${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm") 
		message("CMAKE_SYSTEM_PROCESSOR = ${CMAKE_SYSTEM_PROCESSOR}")
		message("You are compiling for ARM, shared libraries, x86 hardware accleration features and related unit tests have been disabled")
		set(NON_ARM_COMPILATION OFF)
		option(BUILD_SHARED_LIBS "Build shared libraries" Off)
		option(LTP_RNG_USE_RDSEED Off)
		option(USE_X86_HARDWARE_ACCELERATION Off) 
		option(Boost_USE_STATIC_LIBS Off)
		set(CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -fPIC)
	endif()
else()
	set(NON_ARM_COMPILATION ON) # assume non ARM compilation if not specified
endif()

include(GenerateExportHeader)
include(CheckIncludeFile)
include(CheckCXXSourceCompiles)
include(CheckCXXSourceRuns)

option(BUILD_SHARED_LIBS "Build shared libraries" Off)

set(HDTN_VERSION "1.0")
project(HDTN VERSION ${HDTN_VERSION})

OPTION(USE_WEB_INTERFACE "Use Boost Beast or CivetWeb HDTN Web Interface" ON)
include(${CMAKE_SOURCE_DIR}/civetweb_builder.cmake) #do this before changing warning level

include(${CMAKE_SOURCE_DIR}/git_hash_detection.cmake)

#set(CMAKE_CXX_STANDARD 11)
include(${CMAKE_SOURCE_DIR}/cpp_standard_detection.cmake) # Determine whether to compile HDTN with C++11 or C++17

SET(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "HDTN shared and static libraries will be installed to CMAKE_INSTALL_PREFIX/CMAKE_INSTALL_LIBDIR/")
SET(CMAKE_INSTALL_INCLUDEDIR "include/HDTN" CACHE PATH "HDTN public headers will be installed to CMAKE_INSTALL_PREFIX/CMAKE_INSTALL_INCLUDEDIR/")
SET(CMAKE_INSTALL_BINDIR "bin" CACHE PATH "HDTN binaries will be installed to CMAKE_INSTALL_PREFIX/CMAKE_INSTALL_BINDIR/")
SET(CMAKE_INSTALL_DATADIR "share/HDTN" CACHE PATH "HDTN data assets will be installed to CMAKE_INSTALL_PREFIX/CMAKE_INSTALL_DATADIR/")

if( ${CMAKE_BUILD_TYPE} STREQUAL "Coverage" )
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
endif()

set(MIN_BOOST_VERSION 1.66.0)
find_package(Boost ${MIN_BOOST_VERSION} REQUIRED system date_time filesystem timer thread regex filesystem program_options unit_test_framework iostreams random log log_setup)

if(WIN32)
	# define _WIN32_WINNT to use Windows 10 for boost
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
	list(APPEND COMPILE_DEFINITIONS_TO_EXPORT _CRT_SECURE_NO_WARNINGS)
	list(APPEND COMPILE_DEFINITIONS_TO_EXPORT _WIN32_WINNT=0x0A00)

	#remove any existing level 3 warnings
	string(REGEX REPLACE "/W[01234X]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(REGEX REPLACE "/W[01234X]" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
	#compile with warning level 4
	add_compile_options(/W4)
	#Treats all compiler warnings as errors
	add_compile_options(/WX)
	#Treat linker warnings as errors
	add_link_options(/WX)
else()
	#enable all warnings
	add_compile_options(-Wall)
	#enable some extra warning flags that are not enabled by -Wall
	add_compile_options(-Wextra)
	#Issue all the warnings demanded by strict ISO C and ISO C++
	add_compile_options(-Wpedantic)
	#Needed to disable warning for false positive in boost: -Wno-use-after-free only on integrated_tests.cpp
endif()

include(${CMAKE_SOURCE_DIR}/boost_test_link_type.cmake) # Determine whether to compile HDTN with BOOST_ALL_DYN_LINK for logger and unit-tests

#Log level options
SET(LOG_LEVEL_TYPE INFO CACHE STRING "Choose the log level. Options are: TRACE, DEBUG, INFO, WARNING, ERROR, FATAL, NONE")

string( TOLOWER "${LOG_LEVEL_TYPE}" log_level_lower )
if( log_level_lower STREQUAL "trace" )
    SET(LOG_LEVEL 0)
elseif( log_level_lower STREQUAL "debug" )
    SET(LOG_LEVEL 1)
elseif( log_level_lower STREQUAL "info" )
    SET(LOG_LEVEL 2)
elseif( log_level_lower STREQUAL "warning" )
    SET(LOG_LEVEL 3)
elseif( log_level_lower STREQUAL "error" )
    SET(LOG_LEVEL 4)
elseif( log_level_lower STREQUAL "fatal" )
    SET(LOG_LEVEL 5)
elseif( log_level_lower STREQUAL "none" )
    SET(LOG_LEVEL 6)
else()
    message(FATAL_ERROR "LOG_LEVEL_TYPE ${LOG_LEVEL_TYPE} is invalid!  Must be either TRACE, DEBUG, INFO, WARNING, ERROR, FATAL, NONE")
endif()
add_compile_definitions(LOG_LEVEL=${LOG_LEVEL})
if (LOG_LEVEL GREATER 2)
	message(WARNING "You are building with a log level of ${LOG_LEVEL_TYPE}. You may miss important log messages.")
elseif (LOG_LEVEL LESS 2)
	message(WARNING "You are building with a log level of ${LOG_LEVEL_TYPE}. This contains extra logging that may be too verbose for production.")
endif()

#Log output options
OPTION(LOG_TO_CONSOLE "Should logger write to console window" On)
OPTION(LOG_TO_PROCESS_FILE "Should logger write all messages to a single process.log file" Off)
OPTION(LOG_TO_SUBPROCESS_FILES "Should logger write all sub-process messages to their respective subprocess.log files" Off)
OPTION(LOG_TO_ERROR_FILE "Should logger write all error messages to an error.log file" Off)

#Bundle Storage Manager options
SET(STORAGE_SEGMENT_ID_SIZE_BITS "32" CACHE STRING "Set word size of storage's segment_id_t to either 32-bit (default recommended) or 64-bit (high memory only systems for greater than 17TB storage).")
if (STORAGE_SEGMENT_ID_SIZE_BITS STREQUAL "32")
	message("Defining storage segment_id_t to uint32_t")
	add_compile_definitions(STORAGE_SEGMENT_ID_SIZE_BITS=32)
elseif (STORAGE_SEGMENT_ID_SIZE_BITS STREQUAL "64")
	message("Defining storage segment_id_t to uint64_t")
	add_compile_definitions(STORAGE_SEGMENT_ID_SIZE_BITS=64)
else()
	message(FATAL_ERROR "STORAGE_SEGMENT_ID_SIZE_BITS must be set to 32 or 64 in CMakeCache.txt")
endif()
SET(STORAGE_SEGMENT_SIZE_MULTIPLE_OF_4KB "1" CACHE STRING "Set storage segment size multiple (default 1 (1*4096 bytes) recommended for 4KB block sizes).")
if (STORAGE_SEGMENT_SIZE_MULTIPLE_OF_4KB MATCHES "^[1-9][0-9]*$")
	math(EXPR SEGMENT_SIZE "${STORAGE_SEGMENT_SIZE_MULTIPLE_OF_4KB} * 4096")
	message("Defining storage segment size to ${SEGMENT_SIZE} bytes")
	add_compile_definitions("STORAGE_SEGMENT_SIZE_MULTIPLE_OF_4KB=${STORAGE_SEGMENT_SIZE_MULTIPLE_OF_4KB}")
else()
	message(FATAL_ERROR "STORAGE_SEGMENT_SIZE_MULTIPLE_OF_4KB must be set to an integer of at least 1 in CMakeCache.txt")
endif()


if((CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64") OR (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")) #apple m2 (arm64) or linux arm64 (aarch64)
	OPTION(USE_X86_HARDWARE_ACCELERATION "Use ARM CPU NEON instructions (translated from x86 to ARM through CMake-downloaded sse2neon.h header-only library)" ON)
	OPTION(LTP_RNG_USE_RDSEED "NOT SUPPORTED ON ARM - Use CPU RDSEED instruction as a source of randomness for LTP Random Number Generator" OFF)
	if(LTP_RNG_USE_RDSEED)
		message(FATAL_ERROR "LTP_RNG_USE_RDSEED is set to ON in the CMakeCache.txt.  It must be turned off on ARM CPUs")
	endif()
	set(NON_ARM_COMPILATION OFF)
	include(${CMAKE_SOURCE_DIR}/hardware_acceleration_neon.cmake) # Determine if hardware acceleration can be used
else() #amd64
	OPTION(USE_X86_HARDWARE_ACCELERATION "Use CPU SSE/SSE2/BMI1/BMI2 instructions" ON)
	OPTION(LTP_RNG_USE_RDSEED "Use CPU RDSEED instruction as a source of randomness for LTP Random Number Generator" ON)
	include(${CMAKE_SOURCE_DIR}/hardware_acceleration.cmake) # Determine if hardware acceleration can be used
endif()



if (WIN32)
	#add_compile_definitions(ZMQ_STATIC)
	set(libzmq_INCLUDE "C:/libzmq_msvc_x64/include" CACHE PATH "Folder to Libzmq headers")
	set(libzmq_LIB "C:/libzmq_msvc_x64/lib/libzmq-v141-mt-4_3_4.lib" CACHE FILEPATH "Libzmq library")
else()
	set(libzmq_INCLUDE "" CACHE PATH "Folder to Libzmq headers")
	set(libzmq_LIB "zmq" CACHE FILEPATH "Libzmq library")
endif()

if (APPLE)
	find_library(COCOA_LIBRARY Cocoa)
endif()

#Stats option
OPTION(DO_STATS_LOGGING "Should stats be logged to files" Off)
if (DO_STATS_LOGGING)
	add_compile_definitions(DO_STATS_LOGGING)
endif()
#add_compile_definitions(USE_WEB_INTERFACE) #not needed globally, rather this is set specifically to HdtnOneProcessRunner.cpp in the one process CMakeLists.txt


if (DO_STATS_LOGGING OR USE_WEB_INTERFACE)
	set(RUN_TELEMETRY ON)
endif()

OPTION(ENABLE_OPENSSL_SUPPORT "TcpclV4 and GUI Web Server can use OpenSSL" ON)
OPTION(ENABLE_BPSEC "Enable BpSec" ON)
if(ENABLE_BPSEC AND (NOT ENABLE_OPENSSL_SUPPORT))
	message(FATAL_ERROR "ENABLE_BPSEC is set to ON, but ENABLE_OPENSSL_SUPPORT is set to OFF. Bpsec requires OpenSSL to be enabled")
endif()
if(ENABLE_OPENSSL_SUPPORT)
    add_compile_definitions(OPENSSL_SUPPORT_ENABLED)
	list(APPEND COMPILE_DEFINITIONS_TO_EXPORT OPENSSL_SUPPORT_ENABLED) #used by TcpAsyncSender.h and various TcpclV4 includes
	OPTION(OPENSSL_USE_STATIC_LIBS Off)
    if (WIN32)
		set(OPENSSL_ROOT_DIR "C:/openssl-1.1.1e_msvc2017" CACHE PATH "Folder to the root directory of an OpenSSL installation")
	endif()
	find_package(OpenSSL REQUIRED)
endif()
if(ENABLE_BPSEC)
	add_compile_definitions(BPSEC_SUPPORT_ENABLED) #TODO TARGET ONLY SPECIFIC CPP FILES
	list(APPEND COMPILE_DEFINITIONS_TO_EXPORT BPSEC_SUPPORT_ENABLED)
endif()

if(Boost_VERSION_MACRO) #new in CMake 3.15 (in which Boost_VERSION became a string with dots)
	set(BOOST_INTEGER_VERSION_FOR_COMPARISION ${Boost_VERSION_MACRO})
else() #CMake 3.14 or older
	set(BOOST_INTEGER_VERSION_FOR_COMPARISION ${Boost_VERSION})
endif()
if(USE_WEB_INTERFACE)
	if(WEB_INTERFACE_USE_CIVETWEB)
		message("Building lightweight Web Interface using CivetWeb (without OpenSSL/HTTPS support)")
	else()
		set(WEB_INTERFACE_USE_BEAST ON)
		if(ENABLE_OPENSSL_SUPPORT AND (${BOOST_INTEGER_VERSION_FOR_COMPARISION} LESS 107000))
			message(WARNING "USE_WEB_INTERFACE and ENABLE_OPENSSL_SUPPORT are set to ON, but boost version ${BOOST_INTEGER_VERSION_FOR_COMPARISION} is less than 107000 (v1.70).  HTTPS will be disabled for the web interface gui.")
		elseif(ENABLE_OPENSSL_SUPPORT)
			message("Building Web Interface (with OpenSSL/HTTPS support)")
		else()
			message("Building Web Interface (without OpenSSL/HTTPS support)")
		endif()
	endif()
else()
	message("NOT Building Web Interface!")
endif()

OPTION(ENABLE_MASKING "Enable ability to mask destination EIDs in Ingress" OFF)
if(ENABLE_MASKING)
	add_compile_definitions(MASKING_ENABLED)
	list(APPEND COMPILE_DEFINITIONS_TO_EXPORT MASKING_ENABLED)
endif()

OPTION(ENABLE_BPV7_PRIORITY "Enable priority block for bpv7" OFF)

include(${CMAKE_SOURCE_DIR}/add_hdtn_package_export.cmake)

add_subdirectory(common/bpcodec)
if(ENABLE_BPSEC)
    add_subdirectory(common/bpsec)
endif()
add_subdirectory(common/telemetry_definitions)
add_subdirectory(common/logger)
add_subdirectory(common/util)
add_subdirectory(common/config)
add_subdirectory(common/cgr)
add_subdirectory(common/induct_manager)
add_subdirectory(common/outduct_manager)
add_subdirectory(common/tcpcl)
add_subdirectory(common/ltp)
add_subdirectory(common/stcp)
add_subdirectory(common/udp)
add_subdirectory(common/slip_over_uart)
add_subdirectory(common/stats_logger)
if(RUN_TELEMETRY)
	add_subdirectory(module/telem_cmd_interface)
endif()
if(ENABLE_MASKING)
	add_subdirectory(common/masker)
endif()
OPTION(ENABLE_STREAMING_SUPPORT "Should video streaming libraries/apps be compiled?  Requires GStreamer and pkg-config dependencies." Off)
if(ENABLE_STREAMING_SUPPORT)
	add_subdirectory(common/streaming)
endif()
add_subdirectory(module/storage)
add_subdirectory(module/ingress)
add_subdirectory(module/egress)
add_subdirectory(module/router)
add_subdirectory(module/hdtn_one_process)
add_subdirectory(module/encap_repeater)
add_subdirectory(module/udp_delay_sim)
add_subdirectory(module/cli)
add_subdirectory(tests/unit_tests)
add_subdirectory(tests/integrated_tests)

