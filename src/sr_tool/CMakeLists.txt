add_executable(sr_tool sr_tool.cpp)
target_link_libraries(sr_tool PRIVATE citra_common citra_core video_core audio_core input_common)
