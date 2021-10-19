project(EchoServer)

cmake_minimum_required(VERSION 3.20)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror")

find_package(PkgConfig)

pkg_check_modules(IO_URING REQUIRED liburing)

add_executable(echo_server_state
		echo_server_state.c)
target_link_libraries(echo_server_state ${IO_URING_LIBRARIES})

add_executable(echo_server_ucontext
		echo_server_ucontext.c)
target_link_libraries(echo_server_ucontext ${IO_URING_LIBRARIES})

add_executable(echo_server_ucontext_function
		echo_server_ucontext_function.c)
target_link_libraries(echo_server_ucontext_function ${IO_URING_LIBRARIES})
