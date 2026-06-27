#ifndef __A44_H__
#define __A44_H__

#include <stdint.h>

/* Channel Modes */
#define A44_MONO     (0)
#define A44_STEREO   (-1)

/* LUT (Look-Up Table) Sizes */
#define A44_ENCODE_LUT_SIZE (5680)
#define A44_DECODE_LUT_SIZE (2048 * (68 + 1)) // 141,312 bytes

/* A44 Handle Structure */
typedef struct {
  int32_t stereo;      // Channel mode (0: Mono, -1: Stereo)
  uintptr_t cnva_add;  // Address of table buffer (Unused)
  uintptr_t pcma_add;  // Address of PCM buffer
  uintptr_t ada_add;   // Address of ADPCM buffer
  int32_t x, y;        // Predictor/Work variables for Mono
  int32_t rx, ry;      // Predictor/Work variables for Right channel
  int32_t lx, ly;      // Predictor/Work variables for Left channel
  uintptr_t x1, rx1, lx1;
  uintptr_t ra, la;    // Table pointers for Right and Left channels
  int32_t rback, lback, back; // Unused (Legacy context backup areas)
} A44_HANDLE;

/* --- PCM -> ADPCM (Encoder) --- */
void a44_ptoa_make_buffer(A44_HANDLE* handle);
void a44_ptoa_init(A44_HANDLE* handle, int16_t mode); // mode (0: Mono, Others: Stereo)
void a44_ptoa_exec(A44_HANDLE* handle, const uint8_t* pcm_addr, uint32_t pcm_bytes, uint8_t* adpcm_addr);

/* --- ADPCM -> PCM (Decoder) --- */
void a44_atop_make_buffer(A44_HANDLE* handle);
void a44_atop_init(A44_HANDLE* handle, int16_t mode); // mode (0: Mono, Others: Stereo)
void a44_atop_exec(A44_HANDLE* handle, const uint8_t* adpcm_addr, uint32_t adpcm_bytes, uint8_t* pcm_addr);

/* --- Extension / Utility Functions --- */
void a44_atop_mem(A44_HANDLE* handle, uint8_t* save_addr); // save_addr requires 8 bytes for Mono, 16 bytes for Stereo
void a44_atop_set(A44_HANDLE* handle, const uint8_t* load_addr);
void a44_atop_null_exec(A44_HANDLE* handle, const uint8_t* adpcm_addr, uint32_t adpcm_bytes);
void a44_ad_set_panpot(A44_HANDLE* handle, int16_t mode);

#endif