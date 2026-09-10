#ifndef LGNOME_NATIVE_RDP_CALLBACK_PARTS_H
#define LGNOME_NATIVE_RDP_CALLBACK_PARTS_H

#include "rdp_ffi.h"

void native_rdp_install_audio_callbacks(RdpCallbacks *callbacks);
void native_rdp_install_video_callbacks(RdpCallbacks *callbacks);

#endif
