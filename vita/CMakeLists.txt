cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define la variable VITASDK apuntando a tu instalacion de VitaSDK")
  endif()
endif()

project(VitaWA C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VitaWA")
set(VITA_TITLEID  "VWAP00001")   # 9 caracteres: 4 letras + 5 numeros
set(VITA_VERSION  "03.60")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2 -std=gnu11")
include_directories(src src/third_party/quirc)

add_executable(${PROJECT_NAME}
  src/main.c
  src/ui.c
  src/sync.c
  src/tex.c
  src/net.c
  src/ime.c
  src/chan.c
  src/audio.c
  src/camera.c
  src/touch.c
  src/gallery.c
  src/scr_pair.c
  src/scr_home.c
  src/scr_chat.c
  src/scr_status.c
  src/scr_contact.c
  src/scr_media.c
  src/scr_notifs.c
  src/video.c
  src/third_party/monocypher/monocypher.c
  src/third_party/quirc/quirc.c
  src/third_party/quirc/decode.c
  src/third_party/quirc/identify.c
  src/third_party/quirc/version_db.c
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  ScePgf_stub
  ScePvf_stub
  SceNet_stub
  SceNetCtl_stub
  SceProcessmgr_stub
  ScePower_stub
  SceAppMgr_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCamera_stub
  SceTouch_stub
  SceRtc_stub
  SceAvPlayer_stub
  SceNotificationUtil_stub
  curl
  ssl
  crypto
  zstd
  freetype
  png
  jpeg
  bz2
  z
  m
  c
  pthread
  SceSysmem_stub
  SceKernelThreadMgr_stub
  SceIofilemgr_stub
  SceLibKernel_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE res/fonts/MPLUS1p-Regular.ttf res/fonts/MPLUS1p-Regular.ttf
  FILE res/fonts/MPLUS1p-Bold.ttf res/fonts/MPLUS1p-Bold.ttf
  FILE res/fonts/MPLUS1p-ExtraBold.ttf res/fonts/MPLUS1p-ExtraBold.ttf
  FILE res/fonts/OFL-MPLUS1p.txt res/fonts/OFL-MPLUS1p.txt
)
