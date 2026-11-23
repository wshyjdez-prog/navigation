# 1 号：LinuxVoiceAdapter（语音 PAL）。
# 产物 libp1_voice.so 只导出 include/contracts/VoiceFfi.h 的 C ABI，核心层经 FFI 调用并接收回调。
# 后端按 DEC-20261123-1-voice-backend-01 最终选定：TTS = Piper（常驻子进程），ASR = Vosk（C API）。
# 试验期的 espeak-ng、speech-dispatcher、whisper.cpp 后端及其依赖已删除。
enable_language(C)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(P1_PULSE REQUIRED IMPORTED_TARGET libpulse-simple libpulse)

set(P1_VOICE_THIRD_PARTY "${PROJECT_SOURCE_DIR}/third_party/voice" CACHE PATH
    "由 tools/fetch-voice-deps.sh 准备的语音依赖目录（Vosk 库与离线模型/音色）")
set(P1_VOICE_VOSK_DIR "${P1_VOICE_THIRD_PARTY}/vosk/vosk-linux-x86_64-0.3.45" CACHE PATH "Vosk 0.3.45 预编译库目录")
set(P1_VOICE_RESOURCE_SOURCE "${P1_VOICE_THIRD_PARTY}/resources" CACHE PATH "语音离线资源（模型/音色）")

add_library(p1_voice SHARED
    "${PROJECT_SOURCE_DIR}/include/contracts/VoiceFfi.h"
    LinuxVoiceAdapter.cpp
    EngineFactory.cpp
    AudioIo.cpp
    AudioIo.h
    VoiceEngine.h
    VoiceText.h)
set_target_properties(p1_voice PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    INSTALL_RPATH "$ORIGIN")
target_include_directories(p1_voice
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(p1_voice PRIVATE PkgConfig::P1_PULSE Threads::Threads)
# FFI 表面只保留 p1_voice_*：隐藏 C++ 符号，静态库符号也不导出。
target_link_options(p1_voice PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined")
target_compile_definitions(p1_voice PRIVATE P1_VOICE_DEV_RESOURCE_DIR="${P1_VOICE_RESOURCE_SOURCE}")
target_compile_options(p1_voice PRIVATE "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>")

# Piper 是常驻子进程，编译期无依赖；运行期需要资源目录中的 piper/ 与中文音色。
target_sources(p1_voice PRIVATE backends/PiperTts.cpp backends/VoskAsr.cpp)
if(NOT EXISTS "${P1_VOICE_VOSK_DIR}/vosk_api.h" OR NOT EXISTS "${P1_VOICE_VOSK_DIR}/libvosk.so")
    message(FATAL_ERROR "缺少 Vosk 0.3.45（${P1_VOICE_VOSK_DIR}）：先运行 tools/fetch-voice-deps.sh")
endif()
add_library(p1_vosk SHARED IMPORTED GLOBAL)
set_target_properties(p1_vosk PROPERTIES
    IMPORTED_LOCATION "${P1_VOICE_VOSK_DIR}/libvosk.so"
    IMPORTED_NO_SONAME ON
    INTERFACE_INCLUDE_DIRECTORIES "${P1_VOICE_VOSK_DIR}")
target_link_libraries(p1_voice PRIVATE p1_vosk)
install(FILES "${P1_VOICE_VOSK_DIR}/libvosk.so" DESTINATION lib)
if(NOT EXISTS "${P1_VOICE_RESOURCE_SOURCE}/piper/piper" OR NOT EXISTS "${P1_VOICE_RESOURCE_SOURCE}/vosk-model-small-cn-0.22")
    message(WARNING "语音离线资源不完整（${P1_VOICE_RESOURCE_SOURCE}）：运行 tools/fetch-voice-deps.sh，否则播报/识别会报告 MODEL_MISSING")
endif()

install(TARGETS p1_voice LIBRARY DESTINATION lib)
install(FILES "${PROJECT_SOURCE_DIR}/include/contracts/VoiceFfi.h" DESTINATION include/p1-navigation)
# 离线语音资源随应用分发（share/p1-navigation/voice）；适配器按 <exe>/../share/p1-navigation/voice 查找。
if(EXISTS "${P1_VOICE_RESOURCE_SOURCE}")
    install(DIRECTORY "${P1_VOICE_RESOURCE_SOURCE}/" DESTINATION share/p1-navigation/voice USE_SOURCE_PERMISSIONS)
endif()

# 基准程序：经 C ABI 测量播报/识别（DEC-20261123-1-voice-backend-01、NAVIT-20261123-1-voice-01）。
# 适配器只导出 C ABI；测试与基准直接编译 AudioIo.cpp 以复用 WAV 读写。
add_executable(p1-voice-bench tools/VoiceBench.cpp AudioIo.cpp)
target_link_libraries(p1-voice-bench PRIVATE p1_voice PkgConfig::P1_PULSE)
target_include_directories(p1-voice-bench PRIVATE "${PROJECT_SOURCE_DIR}/src")

if(BUILD_TESTING)
    # 纯 C 程序经 FFI 调用，证明边界不依赖 C++/Qt。
    add_executable(p1-voice-ffi-c-smoke tests/FfiSmoke.c)
    target_link_libraries(p1-voice-ffi-c-smoke PRIVATE p1_voice)
    set_target_properties(p1-voice-ffi-c-smoke PROPERTIES C_STANDARD 99)
    add_test(NAME voice-ffi-c-smoke COMMAND p1-voice-ffi-c-smoke)

    add_executable(p1-voice-tests tests/VoiceAdapterTests.cpp AudioIo.cpp)
    target_link_libraries(p1-voice-tests PRIVATE p1_voice PkgConfig::P1_PULSE Threads::Threads)
    target_include_directories(p1-voice-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(p1-voice-tests PRIVATE P1_VOICE_TEST_RESOURCES="${P1_VOICE_RESOURCE_SOURCE}")
    target_compile_options(p1-voice-tests PRIVATE "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>")
    add_test(NAME voice-adapter COMMAND p1-voice-tests)
    set_tests_properties(voice-adapter PROPERTIES TIMEOUT 300)
endif()
