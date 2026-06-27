#ifndef __A44_H__
#define __A44_H__

#include <stdint.h>

// チャンネルモード
#define A44_MONO     (0)
#define A44_STEREO   (-1)

// LUTサイズ
#define A44_ENCODE_LUT_SIZE (5680)
#define A44_DECODE_LUT_SIZE (2048 * (68 + 1)) // 141312 バイト

// ハンドル構造体
typedef struct {
    int16_t stereo;     // 0: mono, -1: stereo
    int16_t pad0;
    uintptr_t cnva_add;  // テーブルバッファのアドレス (未使用)
    uintptr_t pcma_add;  // PCM格納アドレス
    uintptr_t ada_add;   // ADPCM格納アドレス
    int32_t x, y;
    int32_t rx, ry;
    int32_t lx, ly;
    uintptr_t x1, rx1, lx1;
    uintptr_t ra, la;
    int32_t rback, lback, back;
} A44_HANDLE;

// --- PCM -> ADPCM ---
void a44_ptoa_make_buffer(A44_HANDLE* handle);
void a44_ptoa_init(A44_HANDLE* handle, int16_t mode); // 0: mono, 以外: stereo
void a44_ptoa_exec(A44_HANDLE* handle, const uint8_t* pcm_addr, uint32_t pcm_bytes, uint8_t* adpcm_addr);

// --- ADPCM -> PCM ---
void a44_atop_make_buffer(A44_HANDLE* handle);
void a44_atop_init(A44_HANDLE* handle, int16_t mode); // 0: monaural, 以外: stereo
void a44_atop_exec(A44_HANDLE* handle, const uint8_t* adpcm_addr, uint32_t adpcm_bytes, uint8_t* pcm_addr);

// --- 追加機能 ---
void a44_atop_mem(A44_HANDLE* handle, uint8_t* save_addr);  // mono: 8bytes, stereo: 16bytes 確保された領域
void a44_atop_set(A44_HANDLE* handle, const uint8_t* load_addr);
void a44_atop_null_exec(A44_HANDLE* handle, const uint8_t* adpcm_addr, uint32_t adpcm_bytes);
void a44_ad_set_panpot(A44_HANDLE* handle, int16_t mode);

#endif