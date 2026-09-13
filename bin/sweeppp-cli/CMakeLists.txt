add_executable(sweeppp-cli
    src/main.cpp
    src/Options.cpp)

target_include_directories(sweeppp-cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# sweeps::cli-core carries the session commands. `sweeppp-cli info` and
# `sweeps info` are then the same code producing the same text, rather than two
# implementations that happen to agree until one is edited.
target_link_libraries(sweeppp-cli PRIVATE
    sweeppp::sweeppp
    sweeps::cli-core
    sweeppp_warnings)
