
add_executable(OpenMediaExample WIN32
        audio_sink.hpp
        av_clock.hpp
        frame_queue.hpp
        main.cpp
        media_player.hpp
        ring_buffer.hpp
        ui.hpp
        video_renderer.hpp
)
target_link_libraries(OpenMediaExample PRIVATE SDL3::SDL3 OpenMedia::OpenMedia)
if (WIN32 OR LINUX)
    target_link_libraries(OpenMediaExample PRIVATE Vulkan::Headers)
endif ()
if (WIN32)
    target_compile_definitions(OpenMediaExample PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif ()
if (OPENMEDIA_VAAPI)
    target_compile_definitions(OpenMediaExample PRIVATE OPENMEDIA_VAAPI)
endif ()
