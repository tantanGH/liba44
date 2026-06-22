#include <stdint.h>
#include "a44.h"

typedef struct {
  int16_t plus_scale[8];    // +0  (A4) 
  int16_t search_origin[8]; // +16 (A3) 二分探索のスタート基準点（実質的な倍数テーブル2）
  int16_t minus_scale[8];   // +32 (A5) マイナス方向の倍数テーブル
  int16_t codes[8];         // +48 (A4) 0〜7のコード値
  int16_t next_offset[8];   // +64 (A3) 次の段への相対オフセット
} EnclutElement;

typedef struct {
  int16_t diff1;            // 1つ目の4bitサンプルによるPCM変化量
  int16_t diff2;            // 2つ目の4bitサンプルによるPCM変化量
  int32_t next_offset;      // 次のインデックス（loop1の段数）を指すためのバイトオフセット
} DeclutElement;

// エンコード・デコード用ルックアップテーブル領域
static uint8_t encode_lut[ A44_ENCODE_LUT_SIZE ];
static uint8_t decode_lut[ A44_DECODE_LUT_SIZE ];

// インデックス変動値テーブル
static const int16_t table4[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t table4W[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

// YM2608互換 ステップサイズ基準値テーブル
static const int16_t table3[70] = {
  16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
  118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
  544, 598, 658, 724, 796, 875, 963, 1060, 1166, 1282, 1411, 1552,
  1707, 1877, 2065, 2272, 2499, 2749, 3023, 3325, 3657, 4022,
  4424, 4866, 5352, 5887, 6475,
  7122, 7834, 8617, 9478, 10425,
  32 // 末尾の調整値
};

//
//  GET_VAL サブルーチン
//
static int16_t get_val(int16_t d2, int16_t d0) {
  d2 += d0; // ADD.W D0, D2
  if (d2 < 0) {
    return 0; // 2f: MOVE.w D7, D2 (D7は0固定)
  }
  if (d2 >= (68 + 1)) { // bufx = 68
    return 68; // 1f: MOVE.W #bufx, D2
  }
  return d2;
}

//
//  MAKE_BUFFER サブルーチン
//
static void a44_make_buffer_internal(A44_HANDLE* handle) {

  EnclutElement* lut = (EnclutElement*)encode_lut;
    
  // アセンブラの BUFFER_ADR の代わり（lutの先頭アドレス）
  uintptr_t buffer_base = (uintptr_t)lut;

  for (int16_t d0 = 0; d0 < (68 + 2); d0++) { // CMP.B #bufx+2, D0
    EnclutElement* slot = &lut[d0];
    int16_t d2 = table3[d0];

    // --- 1. MAKE_BAI ---
    int16_t d1 = 1;
    int a4_idx = 0;
    int a3_idx = 0;
    int a5_idx = 0;

    do {
      // 前半部 (A3, A5)
      int32_t d3 = (int32_t)d2 * d1;
      slot->search_origin[a3_idx++] = (int16_t)(d3 < 0 ? (d3 - 7) / 8 : d3 / 8); // LSR.L #3

      d3 = (int32_t)d2 * d1;
      d3 = -d3;
      // 符号付き右シフト (ASR.L #3)
      slot->minus_scale[a5_idx++] = (int16_t)(d3 < 0 ? (d3 - 7) / 8 : d3 / 8); 

      // 後半部 (A4)
      d1++; // ADDQ.W #1
      d3 = (int32_t)d2 * d1;
      slot->plus_scale[a4_idx++] = (int16_t)(d3 < 0 ? (d3 - 7) / 8 : d3 / 8);

      d1++; // ADDQ.W #1
    } while (d1 < 17);

    // MOVE.W D2, -(A4) の再現（plus_scaleの最後の要素を上書きしている）
    slot->plus_scale[7] = d2;

    // --- 2. MAKE_CODE ---
    for (int16_t i = 0; i < 8; i++) {
      slot->codes[i] = i;
    }

    // --- 3. SET_NEXTADR ---
    for (int16_t i = 0; i < 8; i++) {
      int16_t next_idx = table4W[i];
      next_idx = get_val(next_idx, d0); // BSR GET_VAL

      // 次のインデックスのスロットへの物理アドレスを計算
      uintptr_t next_slot_addr = buffer_base + (next_idx * 80);
            
      // アセンブラの現在の書き込み位置 A3 (next_offset[i] のアドレス)
      uintptr_t current_a3_addr = (uintptr_t)&(slot->next_offset[i]);

      // SUB.L A3, D2  -> (次アドレス - 現在のアドレス)
      int32_t rel_addr = (int32_t)(next_slot_addr - current_a3_addr);
            
      // ADD.L #6, d2
      rel_addr += 6;

      slot->next_offset[i] = (int16_t)rel_addr;
    }
  }
}

//
//  buffer_making: デコードテーブル（141,312バイト）の生成
//
static void a44_buffer_making_internal(A44_HANDLE* handle) {

  DeclutElement* lut = (DeclutElement*)decode_lut;

  int32_t a0_idx = 0; // 要素単位のインデックス

  for (int16_t d7 = 0; d7 <= 68; d7++) { // loop1: 0 〜 bufx(68)
    for (int16_t d1 = 0; d1 <= 255; d1++) { // loop2: 0 〜 255 (bcc loop2 で1周)
            
      // --- 前半4bitの処理 ---
      int32_t d0 = d7;
      int16_t d2 = (d1 & 0x70) >> 3; // and.w #$70, d2 / lsr.b #3
      int16_t d3 = table4[d2];
      d2 += 1;
            
      int32_t d6 = table3[d0]; // wordテーブルなのでそのままキャスト
      int32_t d2_mulu = d6 * d2;
      if (d1 & 0x80) { // btst #7, d1
        d2_mulu = -d2_mulu;
      }
      int16_t diff1 = (int16_t)((d2_mulu < 0 ? (d2_mulu - 7) : d2_mulu) / 8);

      // インデックスの更新と境界チェック
      d0 += (int8_t)d3; // add.b d3, d0
      if (d0 < 0) d0 = 0;
      if (d0 > 68) d0 = 68;

      // --- 後半4bitの処理 ---
      d2 = (d1 & 0x07) << 1; // and.w #$7, d2 / lsl.b #1
      d3 = table4[d2];
      d2 += 1;

      d6 = table3[d0];
      d2_mulu = d6 * d2;
      if (d1 & 0x08) { // btst #3, d1
        d2_mulu = -d2_mulu;
      }
      int16_t diff2 = (int16_t)((d2_mulu < 0 ? (d2_mulu - 7) : d2_mulu) / 8);

      d0 += (int8_t)d3;
      if (d0 < 0) d0 = 0;
      if (d0 > 68) d0 = 68;

      // --- 次のテーブルへの相対アドレス計算 ---
      // アセンブラ: lsl.l #3, d0 / lsl.l #8, d0 は 「d0 * 2048」を意味する
      //  = 次の段（d0）の先頭（256要素 * 8バイト = 2048バイト）へのアドレス計算
      uintptr_t next_table_addr = (uintptr_t)lut + (d0 * 256 * sizeof(DeclutElement));
      uintptr_t current_a0_addr = (uintptr_t)&lut[a0_idx].next_offset;
            
      int32_t next_offset = (int32_t)(next_table_addr - current_a0_addr);

      // テーブルへ書き込み
      lut[a0_idx].diff1 = diff1;
      lut[a0_idx].diff2 = diff2;
      lut[a0_idx].next_offset = next_offset;
            
      a0_idx++;
    }
  }

  // 状態のリセット
  handle->x1 = handle->cnva_add;
  handle->lx1 = handle->cnva_add;
  handle->back = 0;
  handle->lback = 0;
}

//
//  conv_mono: モノラルデコード本体
//
static void a44_conv_mono(A44_HANDLE* handle, uint32_t adpcm_bytes) {

  if (adpcm_bytes == 0) return;

  // 現在のテーブルポインタを復元
  uint8_t* a0 = (uint8_t*)handle->x1;
  const uint8_t* a1 = (const uint8_t*)handle->ada_add;
  int16_t* a2 = (int16_t*)handle->pcma_add;
  int32_t d1 = handle->back;

  for (uint32_t i = 0; i < adpcm_bytes; i++) {
    uint8_t d3 = *a1++; // move.b (a1)+, d3
      
    // 1要素8バイトなので、d3 * 8 バイト分ポインタを進める
    DeclutElement* slot = (DeclutElement*)(a0 + (d3 << 3));

    // 1サンプル目出力
    d1 += slot->diff1;
    *a2++ = (int16_t)d1;

    // 2サンプル目出力
    d1 += slot->diff2;
    *a2++ = (int16_t)d1;

    // 次のインデックス段の先頭へテーブルポインタをジャンプさせる
    // a0 = (現在のnext_offsetのアドレス) + 相対オフセット値
    a0 = (uint8_t*)&slot->next_offset + slot->next_offset;
  }

  // 状態をハンドルに保存
  handle->back = d1;
  handle->x1 = (uintptr_t)a0;
  handle->ada_add = (uintptr_t)a1;
  handle->pcma_add = (uintptr_t)a2;
}

//
//  conv_stereo: ステレオデコード本体
//
static void a44_conv_stereo(A44_HANDLE* handle, uint32_t adpcm_bytes) {

  // L/Rが1バイトずつインターリーブで並んでいるためループ回数は バイト数/2
  // (subq.l #2, d0 のbccループに対応)
  uint32_t loops = adpcm_bytes / 2;
  if (loops == 0) return;

  uint8_t* a0 = (uint8_t*)handle->lx1;
  uint8_t* a1 = (uint8_t*)handle->rx1;
  const uint8_t* a2 = (const uint8_t*)handle->ada_add;
  int16_t* a3 = (int16_t*)handle->pcma_add;
  int32_t d1 = handle->lback;
  int32_t d2 = handle->rback;

  for (uint32_t i = 0; i < loops; i++) {

    // --- L チャンネル ---
    uint8_t d3_l = *a2++;
    DeclutElement* slot_l = (DeclutElement*)(a0 + (d3_l << 3));

    // --- R チャンネル ---
    uint8_t d3_r = *a2++;
    DeclutElement* slot_r = (DeclutElement*)(a1 + (d3_r << 3));

    // 1サンプル目出力 (L/R交互にPCMバッファに書き込む)
    d1 += slot_l->diff1;
    d2 += slot_r->diff1;
    *a3++ = (int16_t)d1; // L
    *a3++ = (int16_t)d2; // R

    // 2サンプル目出力
    d1 += slot_l->diff2;
    d2 += slot_r->diff2;
    *a3++ = (int16_t)d1; // L
    *a3++ = (int16_t)d2; // R

    // 次の段へ遷移
    a0 = (uint8_t*)((uintptr_t)&slot_l->next_offset + slot_l->next_offset);
    a1 = (uint8_t*)((uintptr_t)&slot_r->next_offset + slot_r->next_offset);
  }

  handle->lback = d1;
  handle->rback = d2;
  handle->lx1 = (uintptr_t)a0;
  handle->rx1 = (uintptr_t)a1;
  handle->ada_add = (uintptr_t)a2;
  handle->pcma_add = (uintptr_t)a3;
}

//
//  conv_monon: モノラルダミーデコード（状態更新のみ）
//
static void a44_conv_monon(A44_HANDLE* handle, uint32_t adpcm_bytes) {

  if (adpcm_bytes == 0) return;

  uint8_t* a0 = (uint8_t*)handle->x1;
  const uint8_t* a1 = (const uint8_t*)handle->ada_add;
  int32_t d1 = handle->back;

  for (uint32_t i = 0; i < adpcm_bytes; i++) {
    uint8_t d3 = *a1++; // move.b (a1)+, d3
        
    // 1要素8バイトなので、d3 * 8 バイト進める
    DeclutElement* slot = (DeclutElement*)(a0 + (d3 << 3));

    // 内部状態（予測値d1）の更新のみ行う（PCMへの書き出しはしない）
    d1 += slot->diff1; // add.w (a0)+, d1
    d1 += slot->diff2; // add.w (a0)+, d1

    // 次のインデックス段の先頭へジャンプ
    a0 = (uint8_t*)&slot->next_offset + slot->next_offset;
  }

  // 状態をハンドルに保存
  handle->back = d1;
  handle->x1 = (uintptr_t)a0;
  handle->ada_add = (uintptr_t)a1;
}

//
//  conv_stereon: ステレオダミーデコード（状態更新のみ）
//
static void a44_conv_stereon(A44_HANDLE* handle, uint32_t adpcm_bytes) {

  uint32_t loops = adpcm_bytes / 2;
  if (loops == 0) return;

  uint8_t* a0 = (uint8_t*)handle->lx1;
  uint8_t* a1 = (uint8_t*)handle->rx1;
  const uint8_t* a2 = (const uint8_t*)handle->ada_add;
  int32_t d1 = handle->lback;
  int32_t d2 = handle->rback;

  for (uint32_t i = 0; i < loops; i++) {

    // --- L チャンネル ---
    uint8_t d3_l = *a2++;
    DeclutElement* slot_l = (DeclutElement*)(a0 + (d3_l << 3));

    // --- R チャンネル ---
    uint8_t d3_r = *a2++;
    DeclutElement* slot_r = (DeclutElement*)(a1 + (d3_r << 3));

    // 1サンプル目の状態更新
    d1 += slot_l->diff1;
    d2 += slot_r->diff1;

    // 2サンプル目の状態更新
    d1 += slot_l->diff2;
    d2 += slot_r->diff2;

    // 次の段へ遷移
    a0 = (uint8_t*)&slot_l->next_offset + slot_l->next_offset;
    a1 = (uint8_t*)&slot_r->next_offset + slot_r->next_offset;
  }

  handle->lback = d1;
  handle->rback = d2;
  handle->lx1 = (uintptr_t)a0;
  handle->rx1 = (uintptr_t)a1;
  handle->ada_add = (uintptr_t)a2;
}

//
//  onef マクロ
//  引数のポインタを経由して、現在の予測値(RD)とテーブルポインタ(RA)を更新し、戻り値でコードを返す
//
static uint8_t a44_onef_core(int16_t pcm_sample, int32_t* p_rd, EnclutElement** p_ra) {

  EnclutElement* ra = *p_ra;
  int32_t rd = *p_rd;
  uint8_t code = 0;

  // 1. 差分の計算 (move.w (a1)+, d5 / sub.w RD, d5)
  int32_t d5 = pcm_sample - (int16_t)rd;

  // 2. 符号ビットの決定
  if (d5 < 0) {
    d5 = -d5;
    code |= 0x08; // 符号ビット(Bit3)をセット
  }

  // 3. ポインタ依存を排除した二分探索
  int idx = 0;
  if (d5 >= ra->plus_scale[2]) {
    // 1回目のBCC: RAは plus_scale[4] の位置へ移動している
    if (d5 >= ra->plus_scale[4]) {
      // 2回目のBCC: (RA)+ により、判定直後にRAは実質 plus_scale[5] へ進む
      // 3回目の判定
      if (d5 >= ra->plus_scale[5]) {
        idx = (d5 >= ra->plus_scale[6]) ? 7 : 6;
      } else {
        idx = 5;
      }
    } else {
      // 2回目のBCS: subq #4 により、RAは実質 plus_scale[3] へ戻る
      // 3回目の判定
      idx = (d5 >= ra->plus_scale[3]) ? 4 : 3;
    }
  } else {
    // 1回目のBCS: RAは plus_scale[0] の位置へ移動している
    if (d5 >= ra->plus_scale[0]) {
      // 2回目のBCC: (RA)+ により、判定直後にRAは実質 plus_scale[1] へ進む
      // 3回目の判定
      idx = (d5 >= ra->plus_scale[1]) ? 2 : 1;
    } else {
      // 2回目のBCS: subq #4 により、RAは実質 plus_scale[0] よりさらに手前へ戻る
      // 3回目の判定
      idx = 0;
    }
  }

  // 下位3ビットに決定したインデックス(0〜7)をマージ
  code |= (idx & 0x07);

  // 4. 予測値(RD)の更新とクリッピング
  int32_t diff = (code & 0x08) ? ra->minus_scale[idx] : ra->plus_scale[idx];
  int32_t next_rd = rd + diff;

  if (next_rd > 32767)  next_rd = 32767;
  if (next_rd < -32768) next_rd = -32768;
  rd = next_rd;

  // 5. 次のインデックス段へのポインタ更新
  uintptr_t current_offset_addr = (uintptr_t)&(ra->next_offset[idx]);
  int16_t rel_offset = ra->next_offset[idx];
  uintptr_t next_ra_addr = current_offset_addr + rel_offset;

  // 6. 状態を戻す
  *p_rd = rd;
  *p_ra = (EnclutElement*)next_ra_addr;

  return code;
}

//
//  conv_stereob: ステレオエンコード本体
//
static void a44_conv_stereob(A44_HANDLE* handle, uint32_t pcm_bytes) {

  // 1サンプルあたりL/Rで4バイト
  uint32_t loops = pcm_bytes / 4; 
  if (loops == 0) return;

  const int16_t* a1 = (const int16_t*)handle->pcma_add;
  EnclutElement* a2 = (EnclutElement*)handle->ra;
  EnclutElement* a3 = (EnclutElement*)handle->la;
  uint8_t* a4 = (uint8_t*)handle->ada_add;

  int32_t d2 = handle->ly;
  int32_t d1 = handle->ry;

  for (uint32_t i = 0; i < loops; i++) {
    uint8_t d6 = 0;
    uint8_t d7 = 0;

    // 1バイト目（Lのサンプル1 ＋ Lのサンプル2）
    uint8_t code_l1 = a44_onef_core(*a1++, &d2, &a3);
    d6 = (code_l1 << 4); // lsl.w #4, d6
    uint8_t code_l2 = a44_onef_core(*a1++, &d2, &a3);
    d6 |= (code_l2 & 0x0F);
    *a4++ = d6;

    // 2バイト目（Rのサンプル1 ＋ Rのサンプル2）
    uint8_t code_r1 = a44_onef_core(*a1++, &d1, &a2);
    d7 = (code_r1 << 4); // lsl.w #4, d7
    uint8_t code_r2 = a44_onef_core(*a1++, &d1, &a2);
    d7 |= (code_r2 & 0x0F);
    *a4++ = d7;
  }

  // 状態の保存
  handle->ry = d1;
  handle->ra = (uintptr_t)a2;
  handle->ly = d2;
  handle->la = (uintptr_t)a3;
  handle->pcma_add = (uintptr_t)a1;
  handle->ada_add  = (uintptr_t)a4;
}

//
//  conv_monob: モノラルエンコード本体
//
static void a44_conv_monob(A44_HANDLE* handle, uint32_t pcm_bytes) {

  uint32_t loops = pcm_bytes / 4; // 1回で2サンプル（計4バイト）処理
  if (loops == 0) return;

  const int16_t* a1 = (const int16_t*)handle->pcma_add;
  EnclutElement* a3 = (EnclutElement*)handle->ra;
  uint8_t* a4 = (uint8_t*)handle->ada_add;

  int32_t d2 = handle->y;

  for (uint32_t i = 0; i < loops; i++) {
    uint8_t d6 = 0;

    uint8_t code1 = a44_onef_core(*a1++, &d2, &a3);
    d6 = (code1 << 4);
    uint8_t code2 = a44_onef_core(*a1++, &d2, &a3);
    d6 |= (code2 & 0x0F);
    *a4++ = d6;
  }

  handle->y = d2;
  handle->ra = (uintptr_t)a3;
  handle->pcma_add = (uintptr_t)a1;
  handle->ada_add  = (uintptr_t)a4;
}

//
//  a44_ptoa_make_buffer: エンコード環境の全初期化とテーブル作成
//
void a44_ptoa_make_buffer(A44_HANDLE* handle) {

  // 1. 各変数の初期化（ptoa_init と共通の処理）
  handle->stereo = 0; // 初期値はモノラル (clr.w stereo(a6))
  handle->x  = 0;     // clr.l x(a6)
  handle->y  = 0;
  handle->rx = 0;
  handle->ry = 0;
  handle->lx = 0;
  handle->ly = 0;

  // 変換バッファ（テーブル）の生成
  // bsr MAKE_BUFFER
  a44_make_buffer_internal(handle);

  // ra, la の初期化
  handle->ra = handle->cnva_add + 6; 
  handle->la = handle->cnva_add + 6;
}

//
//  a44_ptoa_init: 新しいPCMデータを変換する前のコンテキスト初期化
//
void a44_ptoa_init(A44_HANDLE* handle, int16_t mode) {

  handle->stereo = (int32_t)mode;

  handle->x  = 0;
  handle->y  = 0;
  handle->rx = 0;
  handle->ry = 0;
  handle->lx = 0;
  handle->ly = 0;

  // 初期位置をベースアドレス+6にリセット
  handle->ra = handle->cnva_add + 6;
  handle->la = handle->cnva_add + 6;
}

//
//  a44_ptoa_exec: PCM -> ADPCM 変換の実行
//
void a44_ptoa_exec(A44_HANDLE* handle, const uint8_t* pcm_addr, uint32_t pcm_bytes, uint8_t* adpcm_addr) {

  handle->pcma_add = (uintptr_t)pcm_addr;
  handle->ada_add  = (uintptr_t)adpcm_addr;

  if (handle->stereo == 0) {
    // bsr conv_monob
    a44_conv_monob(handle, pcm_bytes);
  } else {
    // bsr conv_stereob
    a44_conv_stereob(handle, pcm_bytes);
  }
}

//
//  a44_atop_make_buffer: デコード用テーブルの生成と初期化
//
void a44_atop_make_buffer(A44_HANDLE* handle) {

  // 初期値の設定（初期値はモノラル）
  handle->stereo = 0; // clr.w stereo(a6)

  // テーブルの先頭アドレスを各ポインタ（インデックス）の初期値にする
  handle->x1  = handle->cnva_add; // move.l cnva_add(a6), x1(a6)
  handle->lx1 = handle->cnva_add; // move.l cnva_add(a6), lx1(a6)
  handle->rx1 = handle->cnva_add; // move.l cnva_add(a6), rx1(a6)

  // バックアップ用の値をクリア
  handle->back  = 0; // clr.l back(a6)
  handle->lback = 0;
  handle->rback = 0;

  // 141,312バイトのデコード用ルックアップテーブルを生成
  // bsr buffer_making
  a44_buffer_making_internal(handle);
}

//
//  a44_atop_init: 新しいADPCMデータをデコードする前の初期化
//
void a44_atop_init(A44_HANDLE* handle, int16_t mode) {
  // move.w d0, stereo(a6)
  handle->stereo = (int32_t)mode;

  // ポインタ（インデックス）をテーブルの先頭にリセット
  handle->x1  = handle->cnva_add; // move.l cnva_add(a6), x1(a6)
  handle->lx1 = handle->cnva_add; // move.l cnva_add(a6), lx1(a6)
  handle->rx1 = handle->cnva_add; // move.l cnva_add(a6), rx1(a6)

  // バックアップ状態のリセット
  handle->back  = 0; // clr.l back(a6)
  handle->lback = 0;
  handle->rback = 0;
}

//
//  a44_atop_exec: ADPCM -> PCM 変換の実行
//
void a44_atop_exec(A44_HANDLE* handle, const uint8_t* adpcm_addr, uint32_t adpcm_bytes, uint8_t* pcm_addr) {
  // move.l a0, ada_add(a6)
  // move.l a1, pcma_add(a6)
  handle->ada_add  = (uintptr_t)adpcm_addr;
  handle->pcma_add = (uintptr_t)pcm_addr;

  // tst.w stereo(a6) / bne @f
  if (handle->stereo == 0) {
    // bsr conv_mono
    a44_conv_mono(handle, adpcm_bytes);
  } else {
    // bsr conv_stereo
    a44_conv_stereo(handle, adpcm_bytes);
  }
}

//
//  atop_mem: 現在のADPCMレジスタの状態を外部バッファに保存
//
void a44_atop_mem(A44_HANDLE* handle, uint8_t* save_addr) {

  // 外部バッファへは、実機互換の固定4バイト単位（uint32_t）で書き出す
  uint32_t* dst = (uint32_t*)save_addr;

  if (handle->stereo == 0) {
    // モノラル
    *dst++ = (uint32_t)handle->back;  // 波形予測値（4バイト）
    
    // 絶対アドレス（uintptr_t）を、テーブル先頭からの「相対距離」に変換して4バイトで保存
    uint32_t offset_x1 = (uint32_t)(handle->x1 - handle->cnva_add);
    *dst++ = offset_x1;
  } else {
    // ステレオ
    *dst++ = (uint32_t)handle->rback; // 波形予測値（4バイト）
    *dst++ = (uint32_t)handle->lback; // 波形予測値（4バイト）
    
    // それぞれテーブル先頭からの「相対距離」に変換して4バイトで保存
    uint32_t offset_rx1 = (uint32_t)(handle->rx1 - handle->cnva_add);
    uint32_t offset_lx1 = (uint32_t)(handle->lx1 - handle->cnva_add);
    *dst++ = offset_rx1;
    *dst++ = offset_lx1;
  }
}

//
//  atop_set: 外部バッファからADPCMレジスタの状態を復元
//
void a44_atop_set(A44_HANDLE* handle, const uint8_t* load_addr) {

  // 外部バッファからは、固定4バイト単位（uint32_t）で読み込む
  const uint32_t* src = (const uint32_t*)load_addr;

  if (handle->stereo == 0) {
    // モノラル
    handle->back = (int32_t)*src++; // 4バイトをそのまま符号付きで復元
    
    // 4バイトの「相対距離」として読み込む（符号拡張の罠を避けるため一度uint32_tで受ける）
    uint32_t offset_x1 = *src++;
    // 現在の環境のベースアドレス（32bit or 64bit）に相対距離を足して、ポインタとして復元
    handle->x1 = handle->cnva_add + offset_x1;
  } else {
    // ステレオ
    handle->rback = (int32_t)*src++;
    handle->lback = (int32_t)*src++;
    
    uint32_t offset_rx1 = *src++;
    uint32_t offset_lx1 = *src++;
    
    // 現在の環境のベースアドレス（32bit or 64bit）にそれぞれの相対距離を足して復元
    handle->rx1 = handle->cnva_add + offset_rx1;
    handle->lx1 = handle->cnva_add + offset_lx1;
  }
}

//
//  atop_null_exec: PCMデータを書き出さずにデコード処理（内部状態の更新）だけを行う
//
void a44_atop_null_exec(A44_HANDLE* handle, const uint8_t* adpcm_addr, uint32_t adpcm_bytes) {

  // move.l a0, ada_add(a6)
  handle->ada_add = (uintptr_t)adpcm_addr;

  if (handle->stereo == 0) {
    // bsr conv_monon
    a44_conv_monon(handle, adpcm_bytes);
  } else {
    // bsr conv_stereon
    a44_conv_stereon(handle, adpcm_bytes);
  }
}

//
//  ad_set_panpot: パンポット（モノラル/ステレオ）の設定
//
void a44_ad_set_panpot(A44_HANDLE* handle, int16_t mode) {
  // move.w d0, stereo(a6)
  handle->stereo = (int32_t)mode; 
}
