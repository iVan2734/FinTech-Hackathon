cmake_minimum_required(VERSION 3.15)
project(fintech_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fintech_server backend/src/main.cpp)
target_include_directories(fintech_server PRIVATE backend/include backend/src)

find_package(Threads REQUIRED)
target_link_libraries(fintech_server PRIVATE Threads::Threads)

# Test program za logiku rizika
add_executable(risk_tests tests/risk_tests.cpp)
target_include_directories(risk_tests PRIVATE backend/include backend/src)

# Generator velikog dataseta + merenje efikasnosti
add_executable(gen_data tools/gen_data.cpp)
target_include_directories(gen_data PRIVATE backend/include backend/src)
