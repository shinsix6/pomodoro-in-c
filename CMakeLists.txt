cmake_minimum_required(VERSION 3.10)
project(pomodoro C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(pomodoro pomodoro.c)
target_link_libraries(pomodoro PRIVATE m)

install(TARGETS pomodoro DESTINATION bin)
