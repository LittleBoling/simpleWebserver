cmake_minimum_required(VERSION 3.1)

project(webserver)

aux_source_directory(. DIR_SRCS)

add_subdirectory(httpconn)
# add_subdirectory(locker)
add_subdirectory(threadpool)
add_subdirectory(log)
add_subdirectory(sqlconn)
# 加入多线程
find_package(Threads REQUIRED)

include_directories(locker)
include_directories(threadpool)
include_directories(httpconn)
include_directories(timer)
include_directories(log)
include_directories(sqlconn)
include_directories(/usr/include/mysql)

add_executable(webserver server.cpp)

target_link_libraries(webserver httpconn log sqlconn Threads::Threads mysqlclient)

SET(CMAKE_BUILD_TYPE "Debug")
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
