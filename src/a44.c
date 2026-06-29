#include <stdint.h>
#include "a44.h"

/* Structure of an element in the Encode Look-Up Table */
typedef struct {
  int16_t plus_scale[8];    // Offset +0:  Scale factor for positive delta
  int16_t search_origin[8]; // Offset +16: Reference base for binary search (Scale table 2)
  int16_t minus_scale[8];   // Offset +32: Scale factor for negative delta
  int16_t codes[8];         // Offset +48: Resolved 4-bit ADPCM code values (0-7)
  int16_t next_offset[8];   // Offset +64: Relative byte offset to the next index state
} ENCODE_LUT_ELEM;

/* Structure of an element in the Decode Look-Up Table */
typedef struct {
  int16_t diff1;            // Delta for the 1st 4-bit sample
  int16_t diff2;            // Delta for the 2nd 4-bit sample
  int32_t next_offset;      // Byte offset pointing to the next index state
} DECODE_LUT_ELEM;

/* Encoding/decoding lookup tables */
static uint8_t encode_lut[ A44_ENCODE_LUT_SIZE ];
static uint8_t decode_lut[ A44_DECODE_LUT_SIZE ];

/* Index modifier tables (Quantizer scale adaptation) */
static const int16_t table4[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t table4W[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* Step size table */
static const int16_t table3[70] = {
  16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
  118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
  544, 598, 658, 724, 796, 875, 963, 1060, 1166, 1282, 1411, 1552,
  1707, 1877, 2065, 2272, 2499, 2749, 3023, 3325, 3657, 4022,
  4424, 4866, 5352, 5887, 6475,
  7122, 7834, 8617, 9478, 10425,
  32 // Padding / Alignment value at the end
};

//
//  get_val
//
static int16_t get_val(int16_t d2, int16_t d0) {
  d2 += d0; // ADD.W D0, D2
  if (d2 < 0) {
    return 0; // 2f: MOVE.W D7, D2 (D7 is fixed to 0)
  }
  if (d2 >= (68 + 1)) { // bufx = 68
    return 68; // 1f: MOVE.W #bufx, D2
  }
  return d2;
}

//
//  make_encoding_lut (Replaces legacy MAKE_BUFFER routine)
//
static void make_encoding_lut(A44_HANDLE* handle) {

  ENCODE_LUT_ELEM* lut = (ENCODE_LUT_ELEM*)encode_lut;
  uintptr_t buffer_base = (uintptr_t)lut;

  for (int16_t d0 = 0; d0 <= 69; d0++) { // CMP.B #bufx+2, D0
    ENCODE_LUT_ELEM* slot = &lut[d0];
    
    // Read step size from table3 (scaled by index d0)
    int16_t d2 = *(int16_t*)((uintptr_t)table3 + (d0 * 2));

    // --- 1. Generate Scale Factors (MAKE_BAI) ---
    int16_t d1 = 1;
    int a4_idx = 0;
    int a3_idx = 0;
    int a5_idx = 0;

    do {
      // Unsigned multiplication for scaling
      uint32_t d3_u = (uint32_t)(uint16_t)d2 * (uint32_t)(uint16_t)d1;

      // Base value for binary search
      slot->search_origin[a3_idx++] = (int16_t)(d3_u >> 3);

      // Scale value for negative delta direction
      int32_t d3_s = -(int32_t)d3_u;
      slot->minus_scale[a5_idx++] = (int16_t)(d3_s >> 3);

      d1++; // ADDQ.W #1
      
      // Calculate next positive scale factor
      d3_u = (uint32_t)(uint16_t)d2 * (uint32_t)(uint16_t)d1;
      slot->plus_scale[a4_idx++] = (int16_t)(d3_u >> 3);

      d1++; // ADDQ.W #1
    } while (d1 < 17);

    // Final adjustment to the scale table (MOVE.W D2, -(A4))
    slot->plus_scale[7] = d2;

    // --- 2. Initialize ADPCM Codes (MAKE_CODE) ---
    for (int16_t i = 0; i < 8; i++) {
      slot->codes[i] = i;
    }

    // --- 3. Compute Relative Branch Offsets (SET_NEXTADR) ---
    for (int16_t i = 0; i < 8; i++) {
      int16_t next_idx = table4W[i];
      next_idx = get_val(next_idx, d0); // BSR GET_VAL

      // Calculate relative byte displacement to the next LUT state slot
      // 80 bytes per ENCODE_LUT_ELEM structure
      uintptr_t next_slot_addr = buffer_base + (next_idx * 80);
      uintptr_t current_a3_addr = (uintptr_t)&(slot->next_offset[i]);

      int32_t rel_addr = (int32_t)(next_slot_addr - current_a3_addr);
      rel_addr += 6;

      slot->next_offset[i] = (int16_t)rel_addr;
    }
  }
}

//
//  make_decoding_lut (Replaces legacy buffer_making routine)
//
static void make_decoding_lut(A44_HANDLE* handle) {

  uint16_t* a0 = (uint16_t*)decode_lut;

  for (int16_t d7 = 0; d7 <= 68; d7++) { 
    for (int16_t d1 = 0; d1 <= 255; d1++) { 
            
      // --- Process 1st 4-bit ADPCM Sample (Upper Nibble) ---
      int32_t d0 = d7;
      int16_t d2 = (d1 & 0x70) >> 3; 
      int16_t d3 = *(int16_t*)((uintptr_t)table4 + d2); 
      d2 += 1;
            
      int16_t d6 = *(int16_t*)((uintptr_t)table3 + d0*2);

      uint32_t d2_32 = (uint32_t)(uint16_t)d2 * (uint32_t)(uint16_t)d6;
      
      if (d1 & 0x80) { // Check sign bit for the 1st sample
        d2_32 = (uint32_t)(-(int32_t)d2_32);
      }
      
      // Perform fixed-point scaling (asr.l #3, d2)
      int16_t diff1 = (int16_t)((int32_t)d2_32 >> 3);

      *a0++ = (uint16_t)diff1;

      // Update quantizer index d0 for the next step
      d0 += (int8_t)d3; 
      if (d0 < 0)  d0 = 0;
      if (d0 > 68) d0 = 68;

      // --- Process 2nd 4-bit ADPCM Sample (Lower Nibble) ---
      d2 = (d1 & 0x07) << 1; 
      d3 = *(int16_t*)((uintptr_t)table4 + d2); 
      d2 += 1;

      d6 = *(int16_t*)((uintptr_t)table3 + d0*2);
      
      uint32_t d2_32_late = (uint32_t)(uint16_t)d2 * (uint32_t)(uint16_t)d6;
      
      if (d1 & 0x08) { // Check sign bit for the 2nd sample
        d2_32_late = (uint32_t)(-(int32_t)d2_32_late);
      }
      int16_t diff2 = (int16_t)((int32_t)d2_32_late >> 3);

      *a0++ = (uint16_t)diff2;

      // Update quantizer index d0 again
      d0 += (int8_t)d3;
      if (d0 < 0)  d0 = 0;
      if (d0 > 68) d0 = 68;

      // --- Calculate relative address offset to the next index state ---
      // Each index block spans 2048 bytes (256 entries * 8 bytes per entry)
      uintptr_t next_table_addr = (uintptr_t)decode_lut + (d0 * 2048);
      uintptr_t current_a0_addr = (uintptr_t)a0; 
            
      int32_t next_offset = (int32_t)(next_table_addr - current_a0_addr);

      // Store the 32-bit offset as two 16-bit words
      *a0++ = (uint16_t)((next_offset >> 16) & 0xFFFF);
      *a0++ = (uint16_t)(next_offset & 0xFFFF);
    }
  }

  // Note: The following initializations from the original assembly 
  // are deliberately bypassed as they are no longer required.
  // handle->x1 = (uintptr_t)decode_lut;
  // handle->lx1 = (uintptr_t)decode_lut;
  // handle->back = 0;
  // handle->lback = 0;
}

//
//  conv_mono: Monaural decoding stream core
//
static void conv_mono(A44_HANDLE* handle, uint32_t adpcm_bytes) {

  if (adpcm_bytes == 0) return;

  // Restore current runtime context and table pointers from the handle
  uint8_t* a0 = (uint8_t*)handle->x1;
  const uint8_t* a1 = (const uint8_t*)handle->ada_add;
  int16_t* a2 = (int16_t*)handle->pcma_add;
  int32_t d1 = handle->back;

  for (uint32_t i = 0; i < adpcm_bytes; i++) {
    uint8_t d3 = *a1++; // Fetch 1 byte containing two 4-bit samples (move.b (a1)+, d3)
      
    // Each DECODE_LUT_ELEM spans 8 bytes.
    // Advance the pointer to the targeted table entry via (d3 * 8).
    DECODE_LUT_ELEM* slot = (DECODE_LUT_ELEM*)(a0 + (d3 << 3));

    // Decode and output the 1st PCM sample
    d1 += slot->diff1;
    *a2++ = (int16_t)d1;

    // Decode and output the 2nd PCM sample
    d1 += slot->diff2;
    *a2++ = (int16_t)d1;

    // Dynamically jump the LUT base pointer to the next quantizer index state block.
    // New base address = (Address of current next_offset member) + Relative byte displacement
    a0 = (uint8_t*)&slot->next_offset + slot->next_offset;
  }

  // Save the updated runtime context back to the handle structure
  handle->back = d1;
  handle->x1 = (uintptr_t)a0;
  handle->ada_add = (uintptr_t)a1;
  handle->pcma_add = (uintptr_t)a2;
}

//
//  conv_stereo: Stereo decoding stream core
//
static void conv_stereo(A44_HANDLE* handle, uint32_t adpcm_bytes) {

  // Since L and R channels are interleaved byte by byte,
  // the total loop count is calculated as (total_bytes / 2).
  // This mirrors the behavior of the legacy 'subq.l #2, d0' assembly branch loop.
  uint32_t loops = adpcm_bytes / 2;
  if (loops == 0) return;

  // Restore channel-specific contexts and pointers from the handle
  uint8_t* a0 = (uint8_t*)handle->lx1;        // Left channel table pointer
  uint8_t* a1 = (uint8_t*)handle->rx1;        // Right channel table pointer
  const uint8_t* a2 = (const uint8_t*)handle->ada_add; // Input ADPCM stream pointer
  int16_t* a3 = (int16_t*)handle->pcma_add;   // Output PCM stream pointer
  int32_t d1 = handle->lback;                 // Left channel predictor accumulator
  int32_t d2 = handle->rback;                 // Right channel predictor accumulator

  for (uint32_t i = 0; i < loops; i++) {

    // --- Process Left Channel ---
    uint8_t d3_l = *a2++;
    DECODE_LUT_ELEM* slot_l = (DECODE_LUT_ELEM*)(a0 + (d3_l << 3));

    // --- Process Right Channel ---
    uint8_t d3_r = *a2++;
    DECODE_LUT_ELEM* slot_r = (DECODE_LUT_ELEM*)(a1 + (d3_r << 3));

    // Output 1st PCM Sample set (L/R interleaved)
    d1 += slot_l->diff1;
    d2 += slot_r->diff1;
    *a3++ = (int16_t)d1; // Left 1
    *a3++ = (int16_t)d2; // Right 1

    // Output 2nd PCM Sample set (L/R interleaved)
    d1 += slot_l->diff2;
    d2 += slot_r->diff2;
    *a3++ = (int16_t)d1; // Left 2
    *a3++ = (int16_t)d2; // Right 2

    // Dynamically jump both channel table pointers to their next index state blocks
    a0 = (uint8_t*)((uintptr_t)&slot_l->next_offset + slot_l->next_offset);
    a1 = (uint8_t*)((uintptr_t)&slot_r->next_offset + slot_r->next_offset);
  }

  // Save the updated runtime context back to the handle structure
  handle->lback = d1;
  handle->rback = d2;
  handle->lx1 = (uintptr_t)a0;
  handle->rx1 = (uintptr_t)a1;
  handle->ada_add = (uintptr_t)a2;
  handle->pcma_add = (uintptr_t)a3;
}

//
//  conv_monon: Monaural dummy decoding (State/Context updates only)
//
static void conv_monon(A44_HANDLE* handle, uint32_t adpcm_bytes) {

  if (adpcm_bytes == 0) return;

  // Restore current runtime context and table pointers from the handle
  uint8_t* a0 = (uint8_t*)handle->x1;
  const uint8_t* a1 = (const uint8_t*)handle->ada_add;
  int32_t d1 = handle->back;

  for (uint32_t i = 0; i < adpcm_bytes; i++) {
    uint8_t d3 = *a1++; // Fetch 1 byte containing two 4-bit samples (move.b (a1)+, d3)
        
    // Each DECODE_LUT_ELEM spans 8 bytes.
    // Advance the pointer to the targeted table entry via (d3 * 8).
    DECODE_LUT_ELEM* slot = (DECODE_LUT_ELEM*)(a0 + (d3 << 3));

    // Update internal predictor status (accumulator d1) only.
    // Deliberately skips writing the decoded samples to the output PCM buffer.
    d1 += slot->diff1; // add.w (a0)+, d1
    d1 += slot->diff2; // add.w (a0)+, d1

    // Dynamically jump the LUT base pointer to the next quantizer index state block.
    a0 = (uint8_t*)&slot->next_offset + slot->next_offset;
  }

  // Save the updated runtime context back to the handle structure
  handle->back = d1;
  handle->x1 = (uintptr_t)a0;
  handle->ada_add = (uintptr_t)a1;
}

//
//  conv_stereon: Stereo dummy decoding (State/Context updates only)
//
static void conv_stereon(A44_HANDLE* handle, uint32_t adpcm_bytes) {

  // Since L and R channels are interleaved byte by byte,
  // the total loop count is calculated as (total_bytes / 2).
  uint32_t loops = adpcm_bytes / 2;
  if (loops == 0) return;

  // Restore channel-specific contexts and pointers from the handle
  uint8_t* a0 = (uint8_t*)handle->lx1;        // Left channel table pointer
  uint8_t* a1 = (uint8_t*)handle->rx1;        // Right channel table pointer
  const uint8_t* a2 = (const uint8_t*)handle->ada_add; // Input ADPCM stream pointer
  int32_t d1 = handle->lback;                 // Left channel predictor accumulator
  int32_t d2 = handle->rback;                 // Right channel predictor accumulator

  for (uint32_t i = 0; i < loops; i++) {

    // --- Process Left Channel ---
    uint8_t d3_l = *a2++;
    DECODE_LUT_ELEM* slot_l = (DECODE_LUT_ELEM*)(a0 + (d3_l << 3));

    // --- Process Right Channel ---
    uint8_t d3_r = *a2++;
    DECODE_LUT_ELEM* slot_r = (DECODE_LUT_ELEM*)(a1 + (d3_r << 3));

    // Update internal predictor status for the 1st sample set (L/R)
    d1 += slot_l->diff1;
    d2 += slot_r->diff1;

    // Update internal predictor status for the 2nd sample set (L/R)
    d1 += slot_l->diff2;
    d2 += slot_r->diff2;

    // Dynamically jump both channel table pointers to their next index state blocks
    a0 = (uint8_t*)&slot_l->next_offset + slot_l->next_offset;
    a1 = (uint8_t*)&slot_r->next_offset + slot_r->next_offset;
  }

  // Save the updated runtime context back to the handle structure
  handle->lback = d1;
  handle->rback = d2;
  handle->lx1 = (uintptr_t)a0;
  handle->rx1 = (uintptr_t)a1;
  handle->ada_add = (uintptr_t)a2;
}

//
//  onef_r: Replaces the core encoder macro for the Right channel
//
static uint8_t onef_r(A44_HANDLE* handle) {

  // 4-bit ADPCM code to be returned
  uint8_t reg_d6_val = 0;

  // Read 16-bit linear PCM input sample
  // move.w (a1)+,d5
  int16_t pcm_value = *(int16_t*)(handle->pcma_add);
  handle->pcma_add += 2;

  // Calculate delta from the current predictor value
  // sub.w d2, d5    
  int16_t d5v = (int16_t)pcm_value - (int16_t)handle->ry; 

  // Compute absolute delta and set the ADPCM sign bit if negative
  // bpl.w @f
  if (d5v < 0) {
    d5v = -d5v;          // neg.w d5
    reg_d6_val |= 0x08;  // or.b #$8, d6 (Set sign bit)
  }

  // --- Binary Search Stage 1 ---
  // cmp.w  (a3),d5   * 4/8
  // bcc.w  1f
  // subq.w #4,a3
  // bra  @f
  // 1: 
  // addq.w #4,a3
  int16_t a3v_1 = *((int16_t*)handle->ra);
  if (d5v >= a3v_1) {
    handle->ra += 4;
  } else {
    handle->ra -= 4;
  }

  // --- Binary Search Stage 2 ---
  // cmp.w (a3)+,d5
  // bcc.w @f
  // subq.w  #4,a3
  int16_t a3v_2 = *(int16_t*)(handle->ra);
  handle->ra += 2;
  if (d5v >= a3v_2) {
    // bcc.w @f
  } else {
    handle->ra -= 4; 
  }

  // --- Binary Search Stage 3 ---
  // cmp.w (a3),d5
  // bcs @f
  // addq.w  #2,a3
  int16_t a3v_3 = *(int16_t*)(handle->ra);
  if (d5v >= a3v_3) {
    handle->ra += 2; 
  } else {
    // bcs @f
  }

  // Finalize the base address offset within the LUT slot
  // addq.w  #8,a3
  // addq.w  #8,a3
  handle->ra += 16;

  // Evaluate the processed sign bit to update the predictor
  // btst  #3,d6
  if ((reg_d6_val & 0x08) == 0) {
        
    // --- Positive Delta Predictor Update ---

    // add.w (a3),d2
    int32_t next_ry = (int32_t)handle->ry + (*(int16_t*)(handle->ra));

    // Check for 16-bit signed overflow/underflow (bvc 3f)
    if (next_ry >= -32768 && next_ry <= 32767) {
      // No overflow: Accept the calculated predictor and jump forward
      handle->ry = (int16_t)next_ry;
      handle->ra += 16;
    } else {

      // --- Positive Overflow Mitigation Branch ---
      // Re-read and fallback to safe scale values
      int16_t d5v = *(int16_t*)(handle->ra + 32);

      // beq @f
      if (d5v != 0) {
        // Fallback option A: Pre-decrement and accumulate
        handle->ra -= 2;                 
        handle->ry += *(int16_t*)(handle->ra);
        handle->ra += 16;
      } else {
        // Fallback option B: Inject saturation flags and step adjustment
        handle->ry |= 0x08;
        handle->ry += *(int16_t*)(handle->ra + 16);
        handle->ra += 16;
      }
    }
  } else {

    // --- Negative Delta Predictor Update ---

    // addq.w #8, a3
    // addq.w #8, a3
    handle->ra += 16;

    // add.w (a3),d2
    int32_t next_ry = (int32_t)handle->ry + (*(int16_t*)(handle->ra));

    // Check for 16-bit signed overflow/underflow (bvc 1f)
    if (next_ry >= -32768 && next_ry <= 32767) {
      // No overflow: Accept the calculated predictor
      handle->ry = (int16_t)next_ry;
    } else {

      // --- Negative Overflow Mitigation Branch ---
      int16_t d5v = *(int16_t*)(handle->ra + 16);

      // beq @f
      if (d5v != 0) {
        // Fallback option A: Pre-decrement and accumulate
        handle->ra -= 2;
        handle->ry += *(int16_t*)(handle->ra);
      } else {
        // Fallback option B: Clear sign bit due to extreme floor saturation
        reg_d6_val &= 0xF7;
        handle->ry += *(int16_t*)(handle->ra - 16);
      }
    }
  }

  // Synchronize base offset for resolving final ADPCM code
  // 1:
  // addq.w  #8,a3
  // addq.w  #8,a3
  handle->ra += 16;

  // Resolve and merge final 4-bit ADPCM code value
  // or.w (a3), d6
  reg_d6_val |= *((uint16_t*)(handle->ra));

  // Advance pointer to the structural jump-table vector
  // addq.w  #8,a3
  // addq.w  #8,a3
  handle->ra += 16;

  // Perform dynamic state transition to the next lookup table index row
  // add.w (a3),a3
  handle->ra += *((int16_t*)(handle->ra));

  return reg_d6_val;
}

//
//  onef_l: Replaces the core encoder macro for the Left channel
//
static uint8_t onef_l(A44_HANDLE* handle) {

  // 4-bit ADPCM code to be returned
  uint8_t reg_d7_val = 0;

  // Read 16-bit linear PCM input sample
  // move.w (a1)+,d5
  int16_t pcm_value = *(int16_t*)(handle->pcma_add);
  handle->pcma_add += 2;

  // Calculate delta from the current predictor value
  // sub.w d1, d5    
  int16_t d5v = (int16_t)pcm_value - (int16_t)handle->ly; 

  // Compute absolute delta and set the ADPCM sign bit if negative
  // bpl.w @f
  if (d5v < 0) {
    d5v = -d5v;          // neg.w d5
    reg_d7_val |= 0x08;  // or.b #$8, d7 (Set sign bit)
  }

  // --- Binary Search Stage 1 ---
  // cmp.w  (a2),d5   * 4/8
  // bcc.w  1f
  // subq.w #4,a2
  // bra  @f
  // 1: 
  // addq.w #4,a2
  int16_t a2v_1 = *((int16_t*)handle->la);
  if (d5v >= a2v_1) {
    handle->la += 4;
  } else {
    handle->la -= 4;
  }

  // --- Binary Search Stage 2 ---
  // cmp.w (a2)+,d5
  // bcc.w @f
  // subq.w  #4,a2
  int16_t a2v_2 = *(int16_t*)(handle->la);
  handle->la += 2;
  if (d5v >= a2v_2) {
    // bcc.w @f
  } else {
    handle->la -= 4; 
  }

  // --- Binary Search Stage 3 ---
  // cmp.w (a2),d5
  // bcs @f
  // addq.w  #2,a2
  int16_t a2v_3 = *(int16_t*)(handle->la);
  if (d5v >= a2v_3) {
    handle->la += 2; 
  } else {
    // bcs @f
  }

  // Finalize the base address offset within the LUT slot
  // addq.w  #8,a2
  // addq.w  #8,a2
  handle->la += 16;

  // Evaluate the processed sign bit to update the predictor
  // btst  #3,d7
  if ((reg_d7_val & 0x08) == 0) {
        
    // --- Positive Delta Predictor Update ---

    // add.w (a2),d1
    int32_t next_ly = (int32_t)handle->ly + (*(int16_t*)(handle->la));

    // Check for 16-bit signed overflow/underflow (bvc 3f)
    if (next_ly >= -32768 && next_ly <= 32767) {
      // No overflow: Accept the calculated predictor and jump forward
      handle->ly = (int16_t)next_ly;
      handle->la += 16;
    } else {

      // --- Positive Overflow Mitigation Branch ---
      // Re-read and fallback to safe scale values
      int16_t d5v = *(int16_t*)(handle->la + 32);

      // beq @f
      if (d5v != 0) {
        // Fallback option A: Pre-decrement and accumulate
        handle->la -= 2;                 
        handle->ly += *(int16_t*)(handle->la);
        handle->la += 16;
      } else {
        // Fallback option B: Inject saturation flags and step adjustment
        handle->ly |= 0x08;
        handle->ly += *(int16_t*)(handle->la + 16);
        handle->la += 16;
      }
    }
  } else {

    // --- Negative Delta Predictor Update ---

    // addq.w #8, a2
    // addq.w #8, a2
    handle->la += 16;

    // add.w  (a2),d1
    int32_t next_ly = (int32_t)handle->ly + (*(int16_t*)(handle->la));

    // Check for 16-bit signed overflow/underflow (bvc 1f)
    if (next_ly >= -32768 && next_ly <= 32767) {
      // No overflow: Accept the calculated predictor
      handle->ly = (int16_t)next_ly;
    } else {

      // --- Negative Overflow Mitigation Branch ---
      int16_t d5v = *(int16_t*)(handle->la + 16);

      // beq @f
      if (d5v != 0) {
        // Fallback option A: Pre-decrement and accumulate
        handle->la -= 2;
        handle->ly += *(int16_t*)(handle->la);
      } else {
        // Fallback option B: Clear sign bit due to extreme floor saturation
        reg_d7_val &= 0xF7;
        handle->ly += *(int16_t*)(handle->la - 16);
      }
    }
  }

  // Synchronize base offset for resolving final ADPCM code
  // 1:
  // addq.w  #8,a2
  // addq.w  #8,a2
  handle->la += 16;

  // Resolve and merge final 4-bit ADPCM code value
  // or.w (a2), d7
  reg_d7_val |= *((uint16_t*)(handle->la));

  // Advance pointer to the structural jump-table vector
  // addq.w  #8,a2
  // addq.w  #8,a2
  handle->la += 16;

  // Perform dynamic state transition to the next lookup table index row
  // add.w (a2),a2
  handle->la += *((int16_t*)(handle->la));

  return reg_d7_val;
}

//
//  a44_ptoa_make_buffer: Full initialization of encoding environment and LUT generation
//
void a44_ptoa_make_buffer(A44_HANDLE* handle) {

  handle->stereo = 0;

  // Note: cnva_add, pcma_add, and ada_add are preserved untouched here
  // to maintain strict compatibility with the legacy assembly specification.

  handle->x   = 0;
  handle->y   = 0;
  handle->rx  = 0;
  handle->ry  = 0;
  handle->lx  = 0;
  handle->ly  = 0;
  handle->x1  = 0;
  handle->rx1 = 0;
  handle->lx1 = 0;

  // Generate lookup table entries (Replaces 'bsr MAKE_BUFFER')
  make_encoding_lut(handle);

  // Initialize table pointers to structural base offset (Replaces '#BUFFER+6')
  handle->ra = (uintptr_t)encode_lut + 6;
  handle->la = (uintptr_t)encode_lut + 6;

  // Explicit safety resets for legacy context backup fields
  handle->back = 0;
  handle->rback = 0;
  handle->lback = 0;
}

//
//  a44_ptoa_init: Context reset prior to processing a new linear PCM stream
//
void a44_ptoa_init(A44_HANDLE* handle, int16_t mode) {

  handle->stereo = mode;

  // Note: cnva_add, pcma_add, and ada_add are preserved untouched here
  // to maintain strict compatibility with the legacy assembly specification.

  handle->x   = 0;
  handle->y   = 0;
  handle->rx  = 0;
  handle->ry  = 0;
  handle->lx  = 0;
  handle->ly  = 0;
  handle->x1  = 0;
  handle->rx1 = 0;
  handle->lx1 = 0;

  // Initialize table pointers to structural base offset (Replaces '#BUFFER+6')
  handle->ra = (uintptr_t)encode_lut + 6;
  handle->la = (uintptr_t)encode_lut + 6;

  // Explicit safety resets for legacy context backup fields
  handle->back = 0;
  handle->rback = 0;
  handle->lback = 0;
}

//
//  a44_ptoa_exec: Executes PCM to ADPCM encoding stream
//
void a44_ptoa_exec(A44_HANDLE* handle, const uint8_t* pcm_addr, uint32_t pcm_bytes, uint8_t* adpcm_addr) {

  handle->pcma_add = (uintptr_t)pcm_addr;
  handle->ada_add  = (uintptr_t)adpcm_addr;

  if (handle->stereo != 0) {

    // --- Stereo Encoding Mode ---
    int32_t remain_bytes = pcm_bytes;

    while (remain_bytes > 0) {

      // Pack 1st and 2nd Left channel samples into upper/lower nibbles
      uint32_t adpcm_value_l = onef_l(handle);
      adpcm_value_l <<= 4;

      // Pack 1st and 2nd Right channel samples into upper/lower nibbles
      uint32_t adpcm_value_r = onef_r(handle);
      adpcm_value_r <<= 4;

      adpcm_value_l |= onef_l(handle);
      adpcm_value_r |= onef_r(handle);

      // Write Left channel packed byte to the output ADPCM stream
      *((uint8_t*)handle->ada_add) = (uint8_t)adpcm_value_l;
      handle->ada_add++;

      // Write Right channel packed byte to the output ADPCM stream
      *((uint8_t*)handle->ada_add) = (uint8_t)adpcm_value_r;
      handle->ada_add++;

      // 4 samples total (2 Left, 2 Right) processed per loop.
      // 4 samples * 2 bytes per 16-bit PCM sample = 8 bytes.
      remain_bytes -= 8;

    }

  } else {

    // --- Monaural Encoding Mode ---
    int32_t remain_bytes = pcm_bytes;

    while (remain_bytes > 0) {

      // Pack 1st and 2nd monaural samples into a single byte
      uint32_t adpcm_value = onef_l(handle);
      adpcm_value <<= 4;

      adpcm_value |= onef_l(handle);

      *((uint8_t*)handle->ada_add) = (uint8_t)adpcm_value;
      handle->ada_add++;

      // 2 samples processed per loop.
      // 2 samples * 2 bytes per 16-bit PCM sample = 4 bytes.
      remain_bytes -= 4;
    }
  }
}

//
//  a44_atop_make_buffer: Generates the decoding LUT and initializes default context
//
void a44_atop_make_buffer(A44_HANDLE* handle) {

  // Set default initial mode to monaural
  handle->stereo = 0; // clr.w stereo(a6)

  // Generate the 141,312-byte decoding look-up table (Replaces 'bsr buffer_making')
  make_decoding_lut(handle);

  // Bind the base address of the generated LUT to each channel's runtime pointer
  handle->x1  = (uintptr_t)decode_lut;
  handle->lx1 = (uintptr_t)decode_lut;
  handle->rx1 = (uintptr_t)decode_lut;

  // Reset predictor accumulator backups
  handle->back  = 0; // clr.l back(a6)
  handle->lback = 0;
  handle->rback = 0;
}

//
//  a44_atop_init: Resets decoder runtime context prior to handling a new ADPCM stream
//
void a44_atop_init(A44_HANDLE* handle, int16_t mode) {
  // Set target channel mode (move.w d0, stereo(a6))
  handle->stereo = (int32_t)mode;

  // Reset channel table pointers back to the decoding LUT base
  handle->x1  = (uintptr_t)decode_lut;
  handle->lx1 = (uintptr_t)decode_lut;
  handle->rx1 = (uintptr_t)decode_lut;

  // Reset predictor accumulator backups
  handle->back  = 0; // clr.l back(a6)
  handle->lback = 0;
  handle->rback = 0;
}

//
//  a44_atop_exec: Executes ADPCM to PCM decoding stream
//
void a44_atop_exec(A44_HANDLE* handle, const uint8_t* adpcm_addr, uint32_t adpcm_bytes, uint8_t* pcm_addr) {

  handle->ada_add  = (uintptr_t)adpcm_addr;
  handle->pcma_add = (uintptr_t)pcm_addr;

  // Dispatch to the corresponding decoding engine based on the channel mode
  // tst.w stereo(a6) / bne @f
  if (handle->stereo == 0) {
    // Replaces 'bsr conv_mono'
    conv_mono(handle, adpcm_bytes);
  } else {
    // Replaces 'bsr conv_stereo'
    conv_stereo(handle, adpcm_bytes);
  }
}

//
//  a44_atop_mem: Serialization helper to backup the current ADPCM register/predictor context
//
void a44_atop_mem(A44_HANDLE* handle, uint8_t* save_addr) {

  // Serializes state data using explicit 4-byte boundaries (uint32_t) for hardware compatibility
  uint32_t* dst = (uint32_t*)save_addr;

  if (handle->stereo == 0) {
    // --- Monaural Mode ---
    *dst++ = (uint32_t)handle->back;  // Predictor accumulator (4 bytes)
    
    // Convert absolute pointer address to a relative 4-byte byte offset
    // from the LUT base. This ensures platform-independent data layouts (32-bit vs 64-bit).
    uint32_t offset_x1 = (uint32_t)(handle->x1 - (uintptr_t)decode_lut);
    *dst++ = offset_x1;
  } else {
    // --- Stereo Mode ---
    *dst++ = (uint32_t)handle->rback; // Right channel predictor accumulator (4 bytes)
    *dst++ = (uint32_t)handle->lback; // Left channel predictor accumulator (4 bytes)
    
    // Convert absolute pointers to relative byte offsets from the LUT base
    uint32_t offset_rx1 = (uint32_t)(handle->rx1 - (uintptr_t)decode_lut);
    uint32_t offset_lx1 = (uint32_t)(handle->lx1 - (uintptr_t)decode_lut);
    *dst++ = offset_rx1;
    *dst++ = offset_lx1;
  }
}

//
//  a44_atop_set: Deserialization helper to restore the ADPCM register/predictor context
//
void a44_atop_set(A44_HANDLE* handle, const uint8_t* load_addr) {

  // Deserializes state data using explicit 4-byte boundaries (uint32_t)
  const uint32_t* src = (const uint32_t*)load_addr;

  if (handle->stereo == 0) {
    // --- Monaural Mode ---
    handle->back = (int32_t)*src++; // Restore 4-byte signed predictor
    
    // Read the stored relative 4-byte offset
    uint32_t offset_x1 = *src++;
    // Reconstruct valid absolute runtime pointer based on current system architecture (32-bit/64-bit)
    handle->x1 = (uintptr_t)decode_lut + offset_x1;
  } else {
    // --- Stereo Mode ---
    handle->rback = (int32_t)*src++;
    handle->lback = (int32_t)*src++;
    
    uint32_t offset_rx1 = *src++;
    uint32_t offset_lx1 = *src++;
    
    // Reconstruct valid absolute runtime pointers based on current system architecture
    handle->rx1 = (uintptr_t)decode_lut + offset_rx1;
    handle->lx1 = (uintptr_t)decode_lut + offset_lx1;
  }
}

//
//  a44_atop_null_exec: Performs dummy decoding without writing PCM data (Updates context only)
//
void a44_atop_null_exec(A44_HANDLE* handle, const uint8_t* adpcm_addr, uint32_t adpcm_bytes) {

  // move.l a0, ada_add(a6)
  handle->ada_add = (uintptr_t)adpcm_addr;

  // Check channel mode to dispatch to the corresponding dummy decoding engine
  if (handle->stereo == 0) {
    // Replaces 'bsr conv_monon'
    conv_monon(handle, adpcm_bytes);
  } else {
    // Replaces 'bsr conv_stereon'
    conv_stereon(handle, adpcm_bytes);
  }
}

//
//  a44_ad_set_panpot: Sets the panning / channel mode configuration
//
void a44_ad_set_panpot(A44_HANDLE* handle, int16_t mode) {
  // Configures the stereo state flag (move.w d0, stereo(a6))
  handle->stereo = (int32_t)mode; 
}