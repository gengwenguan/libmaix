############### Add include ###################
list(APPEND ADD_INCLUDE "dep/codec/inc"
                        "dep/asound/inc"
                        "dep/ffmpeg/inc"
                        "src/h264Enc"
                        "src/tcpServer"
                        "src/terminal"
                        "src/utilTools"
                        "src/fileMng"
                        "src/opusEnc"
    )
list(APPEND ADD_PRIVATE_INCLUDE "")
###############################################

file(GLOB ADD_SRCS  "src/*.cpp"
                    "src/h264Enc/*.cpp"
                    "src/tcpServer/*.cpp"
                    "src/terminal/*.cpp"
                    "src/utilTools/*.cpp"
                    "src/fileMng/*.cpp"
                    "src/opusEnc/*.cpp"
    )
############ Add source files #################
# list(APPEND ADD_SRCS    "src/main.c"
#                         "src/main.cpp"
#                         "src/terminal.cpp"
#                         "src/h264enc.cpp"
#                         "src/tcpserver.cpp"
#     )
# aux_source_directory(src ADD_SRCS)
#append_srcs_dir(ADD_SRCS "src")
# list(REMOVE_ITEM COMPONENT_SRCS "src/test2.c")
###############################################

###### Add required/dependent components ######
list(APPEND ADD_REQUIREMENTS maix_cv_image libmaix)
###############################################

############ Add static libs ##################
# list(APPEND ADD_STATIC_LIB "lib/libtest.a")
###############################################
list(APPEND ADD_DYNAMIC_LIB
            "dep/ffmpeg/lib/libx264.so"
            "dep/ffmpeg/lib/libx264.so.164"
            "dep/ffmpeg/lib/libopus.so"
            "dep/ffmpeg/lib/libopus.so.0"
            "dep/ffmpeg/lib/libssl.so"
            "dep/ffmpeg/lib/libssl.so.1.1"
            "dep/ffmpeg/lib/libcrypto.so"
            "dep/ffmpeg/lib/libcrypto.so.1.1"
            "dep/ffmpeg/lib/libavutil.so"
            "dep/ffmpeg/lib/libavutil.so.56"
            "dep/ffmpeg/lib/libpostproc.so"
            "dep/ffmpeg/lib/libpostproc.so.55"
            "dep/ffmpeg/lib/libavcodec.so"
            "dep/ffmpeg/lib/libavcodec.so.58"
            "dep/ffmpeg/lib/libavdevice.so"
            "dep/ffmpeg/lib/libavdevice.so.58"
            "dep/ffmpeg/lib/libavfilter.so"
            "dep/ffmpeg/lib/libavfilter.so.7"
            "dep/ffmpeg/lib/libavformat.so"
            "dep/ffmpeg/lib/libavformat.so.58"
            "dep/ffmpeg/lib/libswresample.so"
            "dep/ffmpeg/lib/libswresample.so.3"
            "dep/ffmpeg/lib/libswscale.so"
            "dep/ffmpeg/lib/libswscale.so.5"
            )

#### Add compile option for this component ####
#### Just for this component, won't affect other
#### modules, including component that depend
#### on this component
# list(APPEND ADD_DEFINITIONS_PRIVATE -DAAAAA=1)

#### Add compile option for this component
#### and components denpend on this component
# list(APPEND ADD_DEFINITIONS -DAAAAA222=1
#                             -DAAAAA333=1)
###############################################

set(CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS} -lm -g" PARENT_SCOPE) 

register_component()

