cmake_minimum_required(VERSION 3.10)
project(matching_engine)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    include_directories(/opt/homebrew/include)
endif()

find_package(Boost REQUIRED COMPONENTS container)
find_package(Boost 1.81 REQUIRED)

add_executable(tests tests.cpp)

target_include_directories(tests PRIVATE /opt/homebrew/include)
target_link_libraries(tests PRIVATE Boost::boost)
target_link_libraries(my_program PRIVATE Boost::container)
# add_executable(engine main.cpp)   # uncomment if you want to keep main.cpp around