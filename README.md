qsv-ffmpeg-codec
================

Quick sync video as a encode codec in ffmpeg.

## Requirements
   * Intel Media SDK 2014 (https://software.intel.com/en-us/vcsource/tools/media-sdk-server)
   * ubuntu12.04 x64  

## Build
   * ./configure --extra-libs="-lmfx -lsupc++ -lstdc++ -ldl -lva -lva-drm" --extra-ldflags="-L/opt/intel/mediasdk/lib" --extra-cflags=
"-I/opt/intel/mediasdk/include" --prefix="/opt/intel/mediasdk"

## Support
   * Intel Media SDK API : v1.1
   * Codec : H.264

## Codec name
   * H.264 : `h264_qsv`

## Example
   * ffmpeg -i in.mp4 -an -vcodec h264_qsv -b 2000k -f mp4 -y out.mp4
