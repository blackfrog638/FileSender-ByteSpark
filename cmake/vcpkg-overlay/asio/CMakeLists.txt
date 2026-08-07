cmake_minimum_required(VERSION 3.10)
project(asio VERSION 1.38.2 LANGUAGES CXX)

add_library(asio INTERFACE)

find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(asio INTERFACE Threads::Threads)
endif()

# Export target
install(TARGETS asio
    EXPORT asio
    INCLUDES DESTINATION include/
)

install(EXPORT asio
    DESTINATION "share/asio"
    FILE asio-targets.cmake
    NAMESPACE asio::
)

install(DIRECTORY
    include/asio
    DESTINATION include/
    FILES_MATCHING
        PATTERN "*.hpp"
        PATTERN "*.ipp"
)

install(FILES
    include/asio.hpp
    DESTINATION include/
)
