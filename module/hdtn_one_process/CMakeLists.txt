cmake_minimum_required(VERSION 3.0)
project(hdtn-one-process)
set(CMAKE_CXX_STANDARD 11)


add_executable(hdtn-one-process 
    src/HdtnOneProcessRunner.cpp
	src/HdtnOneProcessMain.cpp
)

target_link_libraries(hdtn-one-process
	hdtn_reg
	bpcodec
	hdtn_util
    log_lib
	ingress_async_lib
	storage_lib
	egress_async_lib
	${libzmq_LIB}
	${pthread_LIB}
	${Boost_LIBRARIES}
)
target_include_directories(hdtn-one-process PUBLIC
	include
	${Boost_INCLUDE_DIRS}
	${libzmq_INCLUDE}
)
