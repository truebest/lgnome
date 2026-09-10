#ifndef LGNOME_MEDIA_BACKEND_H
#define LGNOME_MEDIA_BACKEND_H

typedef struct NativeMedia NativeMedia;

/* Returns NULL when DirectMedia is unavailable. */
NativeMedia *native_media_open(const char *webos_sdk_version);
void native_media_close(NativeMedia *media);
/* Hands the DirectMedia pipeline back to the platform when the process is dying
 * without reaching native_media_close() (fatal signal, stray exit()). Safe to
 * call from a signal handler; no-op when nothing is open or NDL is not linked. */
void native_media_emergency_release(void);
#endif
