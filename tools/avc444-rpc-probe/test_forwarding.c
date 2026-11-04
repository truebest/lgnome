#include <assert.h>
#include <stdint.h>

#define ARGS uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h
#ifdef PROBE_STUB
uint32_t vrpc_inband_pts2(ARGS) { return a + 3*b + 5*c + 7*d + 11*e + 13*f + 17*g + 19*h; }
uint32_t DirectVoUpdateWriteIndex(ARGS) { return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h; }
#else
uint32_t vrpc_inband_pts2(ARGS);
uint32_t DirectVoUpdateWriteIndex(ARGS);
int main(void) {
    assert(vrpc_inband_pts2(1,2,3,4,5,6,7,8) == 454);
    assert(DirectVoUpdateWriteIndex(1,2,4,8,16,32,64,128) == 255);
    return 0;
}
#endif
