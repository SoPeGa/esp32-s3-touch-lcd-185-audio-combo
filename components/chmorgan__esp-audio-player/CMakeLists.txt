
set(srcs "audio_player.cpp")
set(includes "include")

if(CONFIG_AUDIO_PLAYER_ENABLE_MP3)
    list(APPEND srcs "audio_mp3.cpp")
endif()

if(CONFIG_AUDIO_PLAYER_ENABLE_WAV)
    list(APPEND srcs "audio_wav.cpp")
endif()

if(CONFIG_AUDIO_PLAYER_ENABLE_AAC)
    list(APPEND srcs "audio_aac.cpp")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${includes}
    REQUIRES esp-libhelix-mp3 driver
)

if(CONFIG_AUDIO_PLAYER_ENABLE_AAC)
    idf_component_optional_requires(PRIVATE espressif__esp_audio_codec)
endif()
