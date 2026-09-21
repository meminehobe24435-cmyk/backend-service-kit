cmake_minimum_required(VERSION 3.14)
project(backend_service_kit CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

set(CORE_SOURCES
    src/http.cpp
    src/cache.cpp
    src/service.cpp)

add_library(bskcore STATIC ${CORE_SOURCES})
target_include_directories(bskcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(bsk_sim src/main.cpp)
target_link_libraries(bsk_sim PRIVATE bskcore)

add_executable(bsk_test test/test_bsk.cpp)
target_link_libraries(bsk_test PRIVATE bskcore)

enable_testing()
add_test(NAME unit COMMAND bsk_test)

install(TARGETS bsk_sim RUNTIME DESTINATION bin)
