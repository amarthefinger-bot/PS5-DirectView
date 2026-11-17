// Static replacement for chiaki-ng lib/config.h.in (CMake-configured upstream).
// DirectView never decodes Remote Play audio/video: all decoders are off.
#ifndef CHIAKI_CONFIG_H
#define CHIAKI_CONFIG_H
#define CHIAKI_LIB_ENABLE_OPUS 0
#define CHIAKI_LIB_ENABLE_PI_DECODER 0
#define CHIAKI_LIB_ENABLE_FFMPEG_DECODER 0
#endif
