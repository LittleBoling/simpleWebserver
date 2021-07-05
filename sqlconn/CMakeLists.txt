# find all source file of this directory 
# and add it to varible DIR_LIB_HTTP
aux_source_directory(. DIR_LIB_HTTP)

# include other .h file
include_directories(../locker)
include_directories(../log)

# Create httpconn lib
add_library(sqlconn ${DIR_LIB_HTTP})

# ADD DEBUG INFO
SET(CMAKE_BUILD_TYPE "Debug")
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g2 -ggdb")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
