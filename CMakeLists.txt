cmake_minimum_required(VERSION 3.10)
project(rdma-tutorial C)

set(CMAKE_C_STANDARD 99)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED libibverbs)

include_directories(
    ${IBVERBS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

link_directories(
    ${IBVERBS_LIBRARY_DIRS}
)

add_executable(client client.c rdma_common.c)
target_link_libraries(client ${IBVERBS_LIBRARIES})

add_executable(server server.c rdma_common.c)
target_link_libraries(server ${IBVERBS_LIBRARIES})

