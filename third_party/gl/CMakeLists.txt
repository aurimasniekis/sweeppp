add_library(sweeppp_gl STATIC src/sweeppp_gl.c)

target_include_directories(sweeppp_gl PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(APPLE)
    target_link_libraries(sweeppp_gl PUBLIC "-framework OpenGL")
elseif(WIN32)
    target_link_libraries(sweeppp_gl PUBLIC opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(sweeppp_gl PUBLIC OpenGL::GL)
endif()
