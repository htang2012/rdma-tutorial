cmake_minimum_required(VERSION 3.10)
project(rdma-tutorial C)

set(CMAKE_C_STANDARD 99)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED libibverbs)

include_directories(
    ${IBVERBS_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    /workspace/hl-thunk/include
    /workspace/hl-thunk/include/uapi
    /workspace/habanalabs/include/uapi
    /workspace/hldk/include/specs/misc
    /workspace/hl-thunk/include/specs/common
    /usr/include
    )

link_directories(
    ${IBVERBS_LIBRARY_DIRS}
    /workspace/hl-thunk/build/lib
    /usr/lib/x86_64-linux-gnu
)

file(GLOB HLTHUNK_LIBRARIES "/workspace/hl-thunk/build/lib/*.so" "/workspace/hl-thunk/build/lib/*.a")

add_executable(client client.c rdma_common.c)
target_link_libraries(client ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES})

add_executable(server server.c rdma_common.c)
target_link_libraries(server ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES})

add_executable(dmabuf dmabuf.c rdma_common.c)
target_link_libraries(dmabuf ${IBVERBS_LIBRARIES} ${HLTHUNK_LIBRARIES})
