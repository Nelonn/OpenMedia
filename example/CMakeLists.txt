
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
