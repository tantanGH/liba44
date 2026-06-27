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
  uintptr_t buffer_base = (uintptr_t)lut;

  for (int16_t d0 = 0; d0 <= 69; d0++) { // CMP.B #bufx+2, D0
    EnclutElement* slot = &lut[d0];
    
    // table3 からのワード読み出し（d0を2倍する）
    int16_t d2 = *(int16_t*)((uintptr_t)table3 + (d0 * 2));

    // --- 1. MAKE_BAI ---
    int16_t d1 = 1;
    int a4_idx = 0;
    int a3_idx = 0;
    int a5_idx = 0;

    do {

      uint32_t d3_u = (uint32_t)(uint16_t)d2 * (uint32_t)(uint16_t)d1;

      slot->search_origin[a3_idx++] = (int16_t)(d3_u >> 3);

      int32_t d3_s = -(int32_t)d3_u;
      slot->minus_scale[a5_idx++] = (int16_t)(d3_s >> 3);

      d1++; // ADDQ.W #1
      
      d3_u = (uint32_t)(uint16_t)d2 * (uint32_t)(uint16_t)d1; // mulu
      slot->plus_scale[a4_idx++] = (int16_t)(d3_u >> 3);

      d1++; // ADDQ.W #1
    } while (d1 < 17);

    // MOVE.W D2, -(A4)
    slot->plus_scale[7] = d2;

    // --- 2. MAKE_CODE ---
    for (int16_t i = 0; i < 8; i++) {
      slot->codes[i] = i;
    }

    // --- 3. SET_NEXTADR ---
    for (int16_t i = 0; i < 8; i++) {
      int16_t next_idx = table4W[i];
      next_idx = get_val(next_idx, d0); // BSR GET_VAL

      uintptr_t next_slot_addr = buffer_base + (next_idx * 80);
      uintptr_t current_a3_addr = (uintptr_t)&(slot->next_offset[i]);

      int32_t rel_addr = (int32_t)(next_slot_addr - current_a3_addr);
      rel_addr += 6;

      slot->next_offset[i] = (int16_t)rel_addr;
    }
  }
}

//
//  buffer_making: デコードテーブル（141,312バイト）の生成
//
static void a44_buffer_making_internal(A44_HANDLE* handle) {

  uint16_t* a0 = (uint16_t*)decode_lut;

  for (int16_t d7 = 0; d7 <= 68; d7++) { 
    for (int16_t d1 = 0; d1 <= 255; d1++) { 
            
      // --- 前半4bitの処理 ---
      int32_t d0 = d7;
      int16_t d2 = (d1 & 0x70) >> 3; 
      int16_t d3 = *(int16_t*)((uintptr_t)table4 + d2); 
      d2 += 1;
            
      int16_t d6 = *(int16_t*)((uintptr_t)table3 + d0*2); // wordテーブルからそのままの16bitとしてキャスト
      
      // 【重要】MC68000の mulu は16bit符号なし乗算
      // レジスタの上位に符号拡張させないため、一度 uint32_t で受けて計算します
      uint32_t d2_32 = (uint32_t)(uint16_t)d2 * (uint32_t)(uint16_t)d6;
      
      if (d1 & 0x80) { 
        // neg.l d2 (32bitとしての2の補数表現に変換)
        d2_32 = (uint32_t)(-(int32_t)d2_32);
      }
      
      // asr.l #3, d2 (符号付きとして算術右シフトを行う)
      int16_t diff1 = (int16_t)((int32_t)d2_32 >> 3);

      *a0++ = (uint16_t)diff1;

      // 段数d0の更新
      d0 += (int8_t)d3; 
      if (d0 < 0)  d0 = 0;
      if (d0 > 68) d0 = 68;

      // --- 後半4bitの処理 ---
      d2 = (d1 & 0x07) << 1; 
      d3 = *(int16_t*)((uintptr_t)table4 + d2); 
      d2 += 1;

      d6 = *(int16_t*)((uintptr_t)table3 + d0*2);
      
      // 後半も同様に完全な符号なし16bit乗算を再現
      uint32_t d2_32_late = (uint32_t)(uint16_t)d2 * (uint32_t)(uint16_t)d6;
      
      if (d1 & 0x08) { 
        d2_32_late = (uint32_t)(-(int32_t)d2_32_late);
      }
      int16_t diff2 = (int16_t)((int32_t)d2_32_late >> 3);

      *a0++ = (uint16_t)diff2;

      d0 += (int8_t)d3;
      if (d0 < 0)  d0 = 0;
      if (d0 > 68) d0 = 68;

      // --- 次のテーブルへの相対アドレス計算 ---
      uintptr_t next_table_addr = (uintptr_t)decode_lut + (d0 * 2048);
      uintptr_t current_a0_addr = (uintptr_t)a0; 
            
      int32_t next_offset = (int32_t)(next_table_addr - current_a0_addr);

      *a0++ = (uint16_t)((next_offset >> 16) & 0xFFFF);
      *a0++ = (uint16_t)(next_offset & 0xFFFF);
    }
  }

  handle->x1 = (uintptr_t)decode_lut;
  handle->lx1 = (uintptr_t)decode_lut;
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
//  onef マクロ (d2,a3,d6) for R
//
static uint8_t onef_r(A44_HANDLE* a44) {

  // 戻すべき 4bit ADPCMコード
  uint8_t reg_d6_val = 0;

  // 16bitPCMデータの読み出し
  // move.w	(a1)+,d5
  int16_t pcm_value = *(int16_t*)(a44->pcma_add);
  a44->pcma_add += 2;

  // 予測値との差分
  // sub.w d2, d5    
  int16_t d5v = (int16_t)pcm_value - (int16_t)a44->ry; 

  // 絶対値化と符号ビットのセット    
	//	bpl.w	@f
  if (d5v < 0) {
    d5v = -d5v;          // neg.w d5
    reg_d6_val |= 0x08;  // or.b #$8, d6
  }

  // 二分探索1回目
  // @@:
  //   cmp.w	(a3),d5		* 4/8
	//	 bcc.w	1f
	//	 subq.w	#4,a3
	//	 bra	@f
	// 1:	
  //   addq.w	#4,a3
	// @@:
  int16_t a3v_1 = *((int16_t*)a44->ra);
  if (d5v >= a3v_1) {
    // addq.w	#4,a3
    a44->ra += 4;
  } else {
    // subq.w	#4,a3
    a44->ra -= 4;
  }

  // 二分探索2回目
	// @@:	
  //  cmp.w	(a3)+,d5
	//  bcc.w	@f
	//	subq.w	#4,a3
	// @@:    
  int16_t a3v_2 = *(int16_t*)(a44->ra);
  a44->ra += 2;
  if (d5v >= a3v_2) {
    // bcc.w @f
  } else {
    // subq.w #4, a3
    a44->ra -= 4; 
  }

  // 二分探索3回目
  // @@:	
  //  cmp.w	(a3),d5
	//	bcs	@f
	//	addq.w	#2,a3
  // @@:
  int16_t a3v_3 = *(int16_t*)(a44->ra);
  if (d5v >= a3v_3) {
    // addq.w #2, a3
    a44->ra += 2; 
  } else {
    // bcs @f
  }

  // ベースアドレス確定
  // @@:
  //  addq.w	#8,a3
	//	addq.w	#8,a3
  a44->ra += 16;

  //  btst	#3,d6
  if ((reg_d6_val & 0x08) == 0) {
        
    // プラス側予測値更新

    //	add.w (a3),d2
    int32_t next_ry = (int32_t)a44->ry + (*(int16_t*)(a44->ra));

    //  bvc 3f
    if (next_ry >= -32768 && next_ry <= 32767) {
      // bvc 3f 成立
      a44->ry = (int16_t)next_ry;
      // bvc 3f
      a44->ra += 16;
    } else {

      // プラス側オーバーフロー補正

      // sub.w	(a3),d2
      //next_ry -= delta;         // a44->ryを直接いじったわけではないので、これは不要

      // move.w	32(a3),d5
      int16_t d5v = *(int16_t*)(a44->ra + 32);

      // beq @f
      if (d5v != 0) {
        // beq @f 不成立
        // add.w -(a3), d2
        a44->ra -= 2;                 
        a44->ry += *(int16_t*)(a44->ra);
        // bra 3f
        a44->ra += 16;
      } else {
        // beq @f 成立
        // or.b #$8, d2
        a44->ry |= 0x08;
        // add.w 16(a3), d2
        a44->ry += *(int16_t*)(a44->ra + 16);
        // bra 3f
        a44->ra += 16;
      }
    }
  } else {

    // マイナス側予測値更新

    //  addq.w #8, a3
    //  addq.w #8, a3
    a44->ra += 16;

    //	add.w (a3),d2
    int32_t next_ry = (int32_t)a44->ry + (*(int16_t*)(a44->ra));

    //  bvc 1f
    if (next_ry >= -32768 && next_ry <= 32767) {
      // bvc 1f 成立
      a44->ry = (int16_t)next_ry;
    } else {

      // マイナス側オーバーフロー補正

      // move.w 16(a3), d5
      int16_t d5v = *(int16_t*)(a44->ra + 16);

      // beq @f
      if (d5v != 0) {
        // beq @f 不成立
        // add.w -(a3), d2
        a44->ra -= 2;
        a44->ry += *(int16_t*)(a44->ra);
      } else {
        // beq @f 成立
        // and.b #$F7, d6 (符号ビットを落とす)
        reg_d6_val &= 0xF7;

        // add.w -16(a3), d2
        a44->ry += *(int16_t*)(a44->ra - 16);
      }
    }
  }

	// 1:
	//	addq.w	#8,a3
	//	addq.w	#8,a3
  a44->ra += 16;

  // ADPCMコードの確定
  //  or.w (a3), d6
  reg_d6_val |= *((uint16_t*)(a44->ra));

	//	addq.w	#8,a3
  //	addq.w	#8,a3
  a44->ra += 16;

  // 次回のためのポインタ更新 
	//	add.w	(a3),a3
  a44->ra += *((int16_t*)(a44->ra));

  return reg_d6_val;
}

//
//  onef マクロ (d1,a2,d7) for L
//
static uint8_t onef_l(A44_HANDLE* a44) {

  // 戻すべき 4bit ADPCMコード
  uint8_t reg_d7_val = 0;

  // 16bitPCMデータの読み出し
  // move.w	(a1)+,d5
  int16_t pcm_value = *(int16_t*)(a44->pcma_add);
  a44->pcma_add += 2;

  // 予測値との差分
  // sub.w d1, d5    
  int16_t d5v = (int16_t)pcm_value - (int16_t)a44->ly; 

  // 絶対値化と符号ビットのセット
	//	bpl.w	@f
  if (d5v < 0) {
    d5v = -d5v;          // neg.w d5
    reg_d7_val |= 0x08;  // or.b #$8, d7
  }

  // 二分探索1回目
  // @@:
  //   cmp.w	(a2),d5		* 4/8
	//	 bcc.w	1f
	//	 subq.w	#4,a2
	//	 bra	@f
	// 1:	
  //   addq.w	#4,a2
	// @@:
  int16_t a2v_1 = *((int16_t*)a44->la);
  if (d5v >= a2v_1) {
    // addq.w	#4,a2
    a44->la += 4;
  } else {
    // subq.w	#4,a2
    a44->la -= 4;
  }

  // 二分探索2回目
	// @@:	
  //  cmp.w	(a2)+,d5
	//  bcc.w	@f
	//	subq.w	#4,a2
	// @@:    
  int16_t a2v_2 = *(int16_t*)(a44->la);
  a44->la += 2;
  if (d5v >= a2v_2) {
    // bcc.w @f
  } else {
    // subq.w #4, a2
    a44->la -= 4; 
  }

  // 二分探索3回目
  // @@:	
  //  cmp.w	(a2),d5
	//	bcs	@f
	//	addq.w	#2,a2
  // @@:
  int16_t a2v_3 = *(int16_t*)(a44->la);
  if (d5v >= a2v_3) {
    // addq.w #2, a2
    a44->la += 2; 
  } else {
    // bcs @f
  }

  // ベースアドレス確定
  // @@:
  //  addq.w	#8,a2
	//	addq.w	#8,a2
  a44->la += 16;

  //  btst	#3,d7
  if ((reg_d7_val & 0x08) == 0) {
        
    // プラス側予測値更新

    //	add.w	(a2),d1
    int32_t next_ly = (int32_t)a44->ly + (*(int16_t*)(a44->la));

    //  bvc 3f
    if (next_ly >= -32768 && next_ly <= 32767) {
      // bvc 3f 成立
      // オーバーフローしていない
      a44->ly = (int16_t)next_ly;
      // bvc 3f
      a44->la += 16;
    } else {

      // プラス側オーバーフロー補正

      // sub.w	(a2),d1
      //next_ly -= delta;         // a44->lyを直接いじったわけではないので、これは不要

      // move.w	32(a2),d5
      int16_t d5v = *(int16_t*)(a44->la + 32);

      // beq @f
      if (d5v != 0) {
        // beq @f 不成立
        // add.w -(a2), d1
        a44->la -= 2;                 
        a44->ly += *(int16_t*)(a44->la);
        // bra 3f
        a44->la += 16;
      } else {
        // beq @f 成立
        // or.b #$8, d1
        a44->ly |= 0x08;
        // add.w 16(a2), d1
        a44->ly += *(int16_t*)(a44->la + 16);
        // bra 3f
        a44->la += 16;
      }
    }

  } else {

    // マイナス側予測値更新
        
    // addq.w #8, a2
    // addq.w #8, a2
    a44->la += 16;

    // add.w	(a2),d1
    int32_t next_ly = (int32_t)a44->ly + (*(int16_t*)(a44->la));

    // bvc 1f
    if (next_ly >= -32768 && next_ly <= 32767) {
      // bvc 1f 成立
      a44->ly = (int16_t)next_ly;
    } else {

      // マイナス側オーバーフロー補正

      // move.w 16(a2), d5
      int16_t d5v = *(int16_t*)(a44->la + 16);

      // beq @f
      if (d5v != 0) {
        // beq @f 不成立
        // add.w -(a2), d1
        a44->la -= 2;
        a44->ly += *(int16_t*)(a44->la);
      } else {
        // beq @f 成立
        // and.b #$F7, d7 (符号ビットを落とす)
        reg_d7_val &= 0xF7;

        // add.w -16(a2), d1
        a44->ly += *(int16_t*)(a44->la - 16);
      }
    }
  }

	// 1:
	//	addq.w	#8,a2
	//	addq.w	#8,a2
  a44->la += 16;

  //  or.w (a2), d7
  reg_d7_val |= *((uint16_t*)(a44->la));

	//	addq.w	#8,a2
	//	addq.w	#8,a2
  a44->la += 16;

	//	add.w	(a2),a2
  a44->la += *((int16_t*)(a44->la));

  return reg_d7_val;
}

//
//  a44_ptoa_make_buffer: エンコード環境の全初期化とテーブル作成
//
void a44_ptoa_make_buffer(A44_HANDLE* handle) {

  handle->stereo = 0;
  handle->pad0   = 0;

  // cnva_add, pcma_add, ada_add は init では触らない（アセンブラ準拠）

  handle->x   = 0;
  handle->y   = 0;
  handle->rx  = 0;
  handle->ry  = 0;
  handle->lx  = 0;
  handle->ly  = 0;
  handle->x1  = 0;
  handle->rx1 = 0;
  handle->lx1 = 0;

  // bsr MAKE_BUFFER
  a44_make_buffer_internal(handle);

  // #BUFFER+6
  handle->ra = (uintptr_t)encode_lut + 6;
  handle->la = (uintptr_t)encode_lut + 6;

  // 念の為
  handle->back = 0;
  handle->rback = 0;
  handle->lback = 0;
}

//
//  a44_ptoa_init: 新しいPCMデータを変換する前のコンテキスト初期化
//
void a44_ptoa_init(A44_HANDLE* handle, int16_t mode) {

  handle->stereo = mode;
  handle->pad0   = 0;

  // cnva_add, pcma_add, ada_add は init では触らない（アセンブラ準拠）

  handle->x   = 0;
  handle->y   = 0;
  handle->rx  = 0;
  handle->ry  = 0;
  handle->lx  = 0;
  handle->ly  = 0;
  handle->x1  = 0;
  handle->rx1 = 0;
  handle->lx1 = 0;

  // #BUFFER+6
  handle->ra = (uintptr_t)encode_lut + 6;
  handle->la = (uintptr_t)encode_lut + 6;

  // 念の為
  handle->back = 0;
  handle->rback = 0;
  handle->lback = 0;
}

//
//  a44_ptoa_exec: PCM -> ADPCM 変換の実行
//
void a44_ptoa_exec(A44_HANDLE* handle, const uint8_t* pcm_addr, uint32_t pcm_bytes, uint8_t* adpcm_addr) {

  handle->pcma_add = (uintptr_t)pcm_addr;
  handle->ada_add  = (uintptr_t)adpcm_addr;

  if (handle->stereo != 0) {

    int32_t remain_bytes = pcm_bytes;

    while (remain_bytes > 0) {

      uint32_t adpcm_value_l = onef_l(handle);
      adpcm_value_l <<= 4;

      uint32_t adpcm_value_r = onef_r(handle);
      adpcm_value_r <<= 4;

      adpcm_value_l |= onef_l(handle);
      adpcm_value_r |= onef_r(handle);

      *((uint8_t*)handle->ada_add) = (uint8_t)adpcm_value_l;
      handle->ada_add++;

      *((uint8_t*)handle->ada_add) = (uint8_t)adpcm_value_r;
      handle->ada_add++;

      remain_bytes -= 8;

    }

  } else {

    int32_t remain_bytes = pcm_bytes;

    while (remain_bytes > 0) {

      uint32_t adpcm_value = onef_l(handle);
      adpcm_value <<= 4;

      adpcm_value |= onef_l(handle);

      *((uint8_t*)handle->ada_add) = (uint8_t)adpcm_value;
      handle->ada_add++;

      remain_bytes -= 4;
    }
  }
}

//
//  a44_atop_make_buffer: デコード用テーブルの生成と初期化
//
void a44_atop_make_buffer(A44_HANDLE* handle) {

  // 初期値の設定（初期値はモノラル）
  handle->stereo = 0; // clr.w stereo(a6)

  // テーブルの先頭アドレスを各ポインタ（インデックス）の初期値にする
  handle->x1  = (uintptr_t)decode_lut; // handle->cnva_add; // move.l cnva_add(a6), x1(a6)
  handle->lx1 = (uintptr_t)decode_lut; //handle->cnva_add; // move.l cnva_add(a6), lx1(a6)
  handle->rx1 = (uintptr_t)decode_lut; //handle->cnva_add; // move.l cnva_add(a6), rx1(a6)

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
  handle->x1  = (uintptr_t)decode_lut; //handle->cnva_add; // move.l cnva_add(a6), x1(a6)
  handle->lx1 = (uintptr_t)decode_lut; //handle->cnva_add; // move.l cnva_add(a6), lx1(a6)
  handle->rx1 = (uintptr_t)decode_lut; //handle->cnva_add; // move.l cnva_add(a6), rx1(a6)

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
    uint32_t offset_x1 = (uint32_t)(handle->x1 - (uintptr_t)decode_lut);
    *dst++ = offset_x1;
  } else {
    // ステレオ
    *dst++ = (uint32_t)handle->rback; // 波形予測値（4バイト）
    *dst++ = (uint32_t)handle->lback; // 波形予測値（4バイト）
    
    // それぞれテーブル先頭からの「相対距離」に変換して4バイトで保存
    uint32_t offset_rx1 = (uint32_t)(handle->rx1 - (uintptr_t)decode_lut);
    uint32_t offset_lx1 = (uint32_t)(handle->lx1 - (uintptr_t)decode_lut);
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
    handle->x1 = (uintptr_t)decode_lut + offset_x1;
  } else {
    // ステレオ
    handle->rback = (int32_t)*src++;
    handle->lback = (int32_t)*src++;
    
    uint32_t offset_rx1 = *src++;
    uint32_t offset_lx1 = *src++;
    
    // 現在の環境のベースアドレス（32bit or 64bit）にそれぞれの相対距離を足して復元
    handle->rx1 = (uintptr_t)decode_lut + offset_rx1;
    handle->lx1 = (uintptr_t)decode_lut + offset_lx1;
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
