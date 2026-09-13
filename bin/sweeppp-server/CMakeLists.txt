add_executable(sweeppp-server src/main.cpp)

target_include_directories(sweeppp-server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# nlohmann/json is scoped to this target alone -- the web wire format. No
# config file anywhere in the project is JSON; that is toml++'s job.
target_link_libraries(sweeppp-server PRIVATE
    sweeppp::sweeppp
    nlohmann_json::nlohmann_json
    sweeppp_warnings)
