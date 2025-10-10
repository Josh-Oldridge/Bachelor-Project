/**
 * @file    enc_sampler.c
 * @brief   Encoder event ingestion and fixed-length window builder for AB+Z quadrature.
 *
 * @details
 * - ISR path (TIM1 CC/IDX) writes compact events into a single-producer/single-consumer ring.
 * - Main path consumes events, packs features into SEQ_LEN×FEAT_DIM windows, and stamps Z with
 *   SSZ (since-stamped-Z) accuracy. Per-window metadata is exported for telemetry.
 * - Provides flow control, minimal diagnostics, and optional ASCII traces for debugging.
 */

/* ============================== Includes =============================== */
#include "enc_sampler.h"
#include "model_meta.h"
#include "main.h"
#include "diag_timing.h"
#include "stm32n6xx_hal.h"

#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>

/**
 * @def ALWAYS_INLINE
 * @brief Force-inline for small helpers on GCC/Clang.
 */
#ifndef ALWAYS_INLINE
# define ALWAYS_INLINE __attribute__((always_inline)) static inline
#endif

/* ---- Ring index helpers (work for any WINFIFO, not just 2^N) ---- */
/**
 * @brief Next index in a modulo-WINFIFO ring.
 * @param i Current index.
 * @return i+1 wrapped to 0 at WINFIFO.
 */
ALWAYS_INLINE uint16_t idx_next(uint16_t i) {
  ++i;
  return (i == WINFIFO) ? 0u : i;
}
/**
 * @brief Previous index in a modulo-WINFIFO ring.
 * @param i Current index.
 * @return i-1 wrapped to WINFIFO-1 at 0.
 */
ALWAYS_INLINE uint16_t idx_prev(uint16_t i) {
  return (i == 0u) ? (uint16_t)(WINFIFO - 1u) : (uint16_t)(i - 1u);
}

/**
 * @brief Next index in the event ring buffer.
 */
ALWAYS_INLINE uint32_t er_next(uint32_t i) {
  ++i;
  return (i == EVENT_BUFFER_SIZE) ? 0u : i;
}
/**
 * @brief Current fill level of the event ring buffer.
 * @param w Writer index.
 * @param r Reader index.
 * @return Number of queued events.
 */
ALWAYS_INLINE uint32_t er_level(uint32_t w, uint32_t r) {
  return (w >= r) ? (w - r) : (EVENT_BUFFER_SIZE - r + w);
}

/** @brief HAL millisecond tick (read directly in ISR to avoid call overhead). */
extern volatile uint32_t uwTick; /* provided by HAL */

/* Predictive branch hints (non-binding but often helpful) */
#ifndef LIKELY
# define LIKELY(x)   __builtin_expect(!!(x), 1)
# define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* --------------------------- Geometry / domains --------------------------- */
/** @brief Cycles per revolution (electrical Gray digits). */
#ifndef CPR
#define CPR 500u
#endif
/** @brief AB both-edge count per mechanical revolution. */
#ifndef QUAD_EDGES_PER_REV
#define QUAD_EDGES_PER_REV (4u * CPR)
#endif

/* --------------------------- Z index / hygiene ---------------------------- */
/** @brief Build with Z index handling. */
#ifndef HAVE_Z_INDEX
#define HAVE_Z_INDEX 1
#endif
/** @brief Software debounce for Z (ms) between accepted Z pulses. */
#ifndef Z_SOFT_DEBOUNCE_MS
#define Z_SOFT_DEBOUNCE_MS 0u
#endif
/** @brief Minimum AB edges since last Z before a new Z can arm. */
#ifndef Z_MIN_AB_EDGES
#define Z_MIN_AB_EDGES 0u
#endif
/** @brief Minimum AB-edge gap to classify double-Z within one revolution. */
#ifndef Z_MIN_GAP_EDGES
#define Z_MIN_GAP_EDGES 0u
#endif
/** @brief Threshold in edges to flag a missed-Z condition. */
#ifndef Z_MISS_EDGE_LIMIT
#define Z_MISS_EDGE_LIMIT (QUAD_EDGES_PER_REV + QUAD_EDGES_PER_REV/8u)
#endif

/** @brief Compile-time toggle for detailed Z diagnostics. */
#ifndef ENC_DEBUG_Z
#define ENC_DEBUG_Z 0   /* set 0 to compile diagnostics out */
#endif

#if ENC_DEBUG_Z
#include <stdio.h>
#define ZDBG(fmt, ...) printf("[Z] " fmt "\n", ##__VA_ARGS__)
#else
#define ZDBG(fmt, ...) do{}while(0)
#endif

#if ENC_DEBUG_Z
/** @brief Count of Z stamps consumed in the current window. */
static volatile uint32_t z_stamps_consumed = 0;
/** @brief Number of Z stamps found in the current window. */
static volatile uint32_t z_stamps_in_window = 0;
/** @brief Last published Z sequence number (continuity check). */
static volatile uint32_t z_seq_last_pub = 0;
/** @brief SSZ (edges since Z) at the time of stamping for diagnostics. */
static volatile uint32_t ab_since_z_at_consume = 0;
#endif

/** @brief ISR observed Z pulses (IDXF seen). */
static volatile uint32_t z_isr_seen_cnt = 0;
/** @brief ISR accepted Z pulses (post-gating). */
static volatile uint32_t z_isr_accepted_cnt = 0;

/* ============================ Linker Sections =========================== */
/** @brief Place window FIFO in a dedicated section for locality. */
#define WIN_RING_ATTR __attribute__((section(".winring"), aligned(32)))
#undef  EVENTBUF_ATTR
/** @brief Align event buffer for efficient DMA/cachelines (if any). */
#define EVENTBUF_ATTR   __attribute__((aligned(32)))

/* ========================== Exposed / Globals =========================== */

extern TIM_HandleTypeDef htim1;

/**
 * @name Per-window flags (written to flags_ring / exported in win_rule_flags)
 * @{
 */
enum {
  WR_SKIP2  = 1u << 0, /**< Two or more "same source" edges in window (possible bounce). */
  WR_ZMISS  = 1u << 1, /**< Edges since last Z exceeded @ref Z_MISS_EDGE_LIMIT. */
  WR_ZDOUBLE= 1u << 2, /**< Z gap less than @ref Z_MIN_GAP_EDGES (double index). */
  WR_ZINWIN = 1u << 3, /**< This window contains at least one Z row. */
};
/** @} */

/** @brief Rolling/live flags for the window currently being packed. */
volatile uint32_t enc_rule_flags = 0;
/** @brief Flags associated with the last dequeued window (stable). */
volatile uint32_t win_rule_flags = 0;

/**
 * @name FIFO flow control (windows)
 * @{
 */
volatile uint32_t win_ready = 0;   /**< Number of windows available for copy. */
volatile uint32_t win_total = 0;   /**< Total produced windows since boot/arm. */
volatile uint32_t event_drops = 0; /**< Number of dropped ISR events (overflow). */
/** @} */

#if ENC_DEBUG_Z
ENC_USED static uint8_t WIN_RING_ATTR win_z_count[WINFIFO];
#endif

/* Feature sizes (from model_meta.h) */
/** @brief Model sequence length (number of rows per window). */
#define SEQ_LEN   (MODEL_SEQ_LEN)
/** @brief Per-row feature dimension. */
#define FEAT_DIM  (MODEL_FEAT_DIM)

/* ===================== SPSC window ring (zero-copy) ==================== */
/** @brief Fixed-size FIFO of feature windows (SPSC). */
ENC_USED static float    WIN_RING_ATTR win_ring[WINFIFO][SEQ_LEN * FEAT_DIM];
/** @brief Per-window rule flags mirror. */
ENC_USED static uint32_t WIN_RING_ATTR flags_ring[WINFIFO];
/** @brief Bit mask per window: bit i set if row i contains a Z stamp. */
ENC_USED static uint32_t WIN_RING_ATTR win_z_mask[WINFIFO]; // bit i => Z at row i
/** @brief Accumulator mask for the currently building window. */
static uint32_t z_rows_mask_this_window = 0;
/** @brief Window FIFO write/read indices and occupancy. */
static uint16_t wf_w = 0, wf_r = 0, wf_count = 0;
/** @brief Current row index in the active window. */
static uint8_t win_row = 0;

/* ====================== Optional per-window trace ====================== */
#if ENABLE_TRACE_WIN
/** @brief Trace buffers for last-built window (AB code, source, Z). */
static uint8_t trace_ab[SEQ_LEN];
static uint8_t trace_src[SEQ_LEN];
static uint8_t trace_z[SEQ_LEN];

/** @brief Retained copy of previous window traces for UI/debug dump. */
static uint8_t last_trace_ab[SEQ_LEN];
static uint8_t last_trace_src[SEQ_LEN];
static uint8_t last_trace_z[SEQ_LEN];
#endif

/* ============================ Encoder state ============================ */
/**
 * @brief Minimal encoder state for feature engineering.
 */
typedef struct {
  uint8_t last_AB_state;       /**< Gray code at previous event (bit1=A, bit0=B). */
  float   speed_change_rate;   /**< |(dt / EWMA_dt) - 1|, normalized speed jitter. */
  float   baseline_interval;   /**< EWMA of inter-edge interval in milliseconds. */
} encoder_state_t;

static encoder_state_t enc_state = { 0 };

/* ------------------------------ Direction ------------------------------ */
/**
 * @brief Last latched direction from step transitions (+1 fwd, -1 bwd, 0 unknown).
 * @return Signed direction.
 */
static volatile int8_t enc_latched_dir = 0;
int8_t EncSampler_GetLatchedDir(void) { return enc_latched_dir; }

/**
 * @brief Direction summary for the last published window (+1/-1/0).
 * @return Signed direction.
 */
static volatile int8_t last_window_dir = +1;
int8_t EncSampler_GetLastWindowDir(void) { return last_window_dir; }

/* ------------------------- Z timing / counters ------------------------- */
/** @brief Millisecond timestamp of last accepted Z (ISR domain). */
static volatile uint32_t z_last_ms_isr = 0;
/** @brief AB edges since last accepted Z (ISR domain). */
static volatile uint32_t ab_edges_since_z_isr = 0;
/** @brief Gap (in edges) between the previous accepted Z and the arm point. */
static volatile uint32_t z_last_gap_edges_isr = 0;

/** @brief Epoch increments by 1 per accepted Z (visible to main). */
volatile uint32_t z_epoch_isr = 0;
/** @brief Last seen epoch in main (staleness/diagnostic). */
static uint32_t z_epoch_seen_main = 0;

/* -------------------------- Public diagnostics ------------------------- */
volatile uint32_t z_diag_isr_seen     = 0;
volatile uint32_t z_diag_isr_accepted = 0;
volatile uint32_t z_diag_ab_since_isr = 0;
volatile uint32_t z_diag_steps_x4     = 0;
volatile uint32_t z_diag_last_z_ms    = 0;

/* ----------------------- Main-side Z serialization --------------------- */
/** @brief Monotonic sequence number mirrored to windows that contain Z. */
static uint32_t z_seq_main = 0;

/* -------------------------- SSZ-accurate placement --------------------- */
/** @brief ISR: 1-based SSZ target row at which Z should be stamped. */
static volatile uint32_t z_target_ssz_isr  = 0;
/** @brief Main: copy of @ref z_target_ssz_isr used for stamping. */
static          uint32_t z_target_ssz_main = 0;
/** @brief ISR: pending align-to-AB==00 acceptance latch. */
static volatile uint8_t  z_align_pending_isr = 0;

/** @brief Main: whether a Z stamp is due when SSZ matches the target. */
static uint8_t z_pending_main = 0u;

/* ------------------------------- Misc ---------------------------------- */
/** @brief Seed the first model Gray with 0 after an arm if needed. */
static volatile uint8_t seed_from_zero_next_ab = 0;
/** @brief Require Z pin to go LOW at least once after arming. */
static volatile uint8_t z_need_low_since_arm = 0u;

/**
 * @brief Sample Z pin (I channel) directly from GPIO (PE7).
 * @return 1 if high, 0 if low.
 */
ALWAYS_INLINE uint8_t z_pin_now(void) {
  return (GPIOE->IDR & ENC_Z_Pin) ? 1u : 0u; /* PE7 */
}

/* ---- lightweight capture/gating shim (no big cap buffer) ---- */
/** @brief Trial capture state. */
static volatile uint8_t  cap_armed = 0, cap_active = 0, cap_done = 0;
/** @brief Trial capture end time (ms) and duration (ms). */
static volatile uint32_t cap_end_ms = 0, cap_duration_ms = 0;

/* ---------------------------- Win-stamp metadata ----------------------- */
/** @brief 1-based ABx4 start index for each window modulo revolution. */
ENC_USED static uint32_t WIN_RING_ATTR win_ab_start_x4[WINFIFO];
/** @brief 1-based ABx4 end index for each window modulo revolution. */
ENC_USED static uint32_t WIN_RING_ATTR win_ab_end_x4[WINFIFO];
/** @brief First Z row in window (0..SEQ_LEN-1) or 0xFFFF if none. */
ENC_USED static uint16_t WIN_RING_ATTR win_z_row[WINFIFO];
/** @brief Z epoch mirrored into the window that contains the first Z. */
ENC_USED static uint32_t WIN_RING_ATTR win_z_epoch[WINFIFO];
/** @brief Z sequence value after finishing this window (monotonic). */
ENC_USED static uint32_t WIN_RING_ATTR win_z_seq_end[WINFIFO];

/* ---- exported "last copied window" stamps (read by app_x-cube-ai) ---- */
/** @brief Exported: ABx4 start (1-based, modulo rev) for last copied window. */
volatile uint32_t win_stamp_ab_start_x4 = 0;
/** @brief Exported: ABx4 end (1-based, modulo rev) for last copied window. */
volatile uint32_t win_stamp_ab_end_x4   = 0;
/** @brief Exported: ABx4 delta (end-start modulo rev) for last copied window. */
volatile uint32_t win_stamp_ab_delta_x4 = 0;
/** @brief Exported: Z epoch for last copied window (0 if none). */
volatile uint32_t win_stamp_z_epoch     = 0;
/** @brief Exported: Z row for last copied window (-1 if none). */
volatile int16_t  win_stamp_z_row       = -1;
/** @brief Exported: Last Z sequence observed at end of this window. */
volatile uint32_t win_stamp_z_seq_end   = 0;
/** @brief Exported: Bit mask of Z rows for last copied window. */
volatile uint32_t win_stamp_z_mask   = 0;

/**
 * @brief Per-window stamp accumulator (main thread).
 */
typedef struct {
  uint32_t ab_since_z_soft;      /**< SSZ counter in AB edges (x4), 1-based when used for f[7]. */
  uint16_t z_row_this_window;    /**< First stamped Z row, or 0xFFFF if none. */
  uint32_t z_epoch_this_window;  /**< Z sequence for the stamped row, 0 if none. */
} winstamp_t;

static winstamp_t g_ws = { 0, 0xFFFFu, 0 };

/** @brief Gate publishing until the first Z after arming/init has occurred. */
static volatile uint8_t suppress_until_first_Z = 1u;
/** @brief High-water marks for telemetry. */
static uint32_t event_ring_hwm = 0, winfifo_hwm = 0;

/** @brief Previous Z pin level sampled in ISR (coarse). */
static volatile uint8_t z_pin_prev_isr = 0;

/**
 * @brief Compute modular delta with wrap-around.
 * @param cur Current value.
 * @param prev Previous value.
 * @param mod Modulus.
 * @return (cur - prev) modulo mod.
 */
ALWAYS_INLINE uint32_t mod_delta_u32(uint32_t cur, uint32_t prev, uint32_t mod) {
  return (cur >= prev) ? (cur - prev) : (mod - prev + cur);
}

/* ============================ Helpers ================================== */
/**
 * @brief Derive source lanes (A/B) from a Gray transition.
 * @param prev_gray Previous Gray code (bit1=A, bit0=B).
 * @param curr_gray Current Gray code.
 * @param srcA [out] 1 if A toggled, else 0.
 * @param srcB [out] 1 if B toggled, else 0.
 */
ALWAYS_INLINE void gray_step_to_src(uint8_t prev_gray, uint8_t curr_gray,
                                    uint8_t *srcA, uint8_t *srcB) {
  const uint8_t toggled = (uint8_t)((prev_gray ^ curr_gray) & 0x3u);
  *srcA = (toggled & 0x2u) ? 1u : 0u;
  *srcB = (toggled & 0x1u) ? 1u : 0u;
}

/**
 * @brief Apply per-feature min/scale normalization and clamp to [0,1].
 * @param j Feature index.
 * @param x Raw value.
 * @return Scaled value in [0,1].
 */
ALWAYS_INLINE float scale_feat(int j, float x) {
  float y = (x - MODEL_FEAT_MIN[j]) * MODEL_FEAT_SCALE[j];
  if (y < 0.f) y = 0.f;
  if (y > 1.f) y = 1.f;
  return y;
}

/**
 * @brief Read AB Gray code directly from GPIOE (A=PE9, B=PE11).
 * @return 0..3 Gray value (bit1=A, bit0=B).
 */
ALWAYS_INLINE uint8_t enc_read_AB(void) {
  const uint32_t idr = GPIOE->IDR; /* A=PE9, B=PE11 */
  return (uint8_t)(((idr & ENC_A_Pin) ? 2u : 0u) | ((idr & ENC_B_Pin) ? 1u : 0u));
}

/* =============================== Events ================================ */
/**
 * @brief Compact ISR event record.
 * @note  Packed to minimize ring bandwidth and cache misses.
 */
typedef struct __attribute__((packed)) {
  uint8_t  src;       /**< 0=Z, 1=A(CC1), 2=B(CC2) */
  uint8_t  ab_state;  /**< Gray at capture (bit1=A, bit0=B) */
  uint16_t dms;       /**< Delta ms since previous event (uwTick), clamped */
} encoder_event_t;

/**
 * @brief Quadrature direction from Gray transition.
 * @return +1 forward, -1 backward, 0 if invalid or no change.
 */
ALWAYS_INLINE int8_t quad_dir_from_transition(uint8_t prev_g, uint8_t curr_g) {
	prev_g &= 3u;
	curr_g &= 3u;
	if (prev_g == curr_g)
		return 0; /* no change */
	const uint8_t diff = (uint8_t) ((prev_g ^ curr_g) & 0x3u);
	const uint8_t adjacent = (diff == 1u) || (diff == 2u);
	if (!adjacent)
		return 0; /* glitch: both bits -> reject */

	if ((prev_g == 0 && curr_g == 1) || (prev_g == 1 && curr_g == 3)
			|| (prev_g == 3 && curr_g == 2) || (prev_g == 2 && curr_g == 0))
		return +1;

	return -1;
}

/** @brief ISR->main SPSC event ring. */
ENC_USED static encoder_event_t EVENTBUF_ATTR event_buffer[EVENT_BUFFER_SIZE];
static volatile uint32_t event_write = 0;
static volatile uint32_t event_read  = 0;
/** @brief Last uwTick captured in ISR for dms computation. */
static volatile uint32_t last_tick_isr = 0; /* for delta computation */

/* ============== Event ring push (ISR) ============== */
/**
 * @brief Push a capture event into the ISR ring.
 * @param src        0=Z, 1=A(CC1), 2=B(CC2)
 * @param ccr_sample Capture register (unused; reserved).
 * @param ab_state   Gray code after the edge.
 */
ALWAYS_INLINE void push_event_isr(uint8_t src, uint16_t ccr_sample, uint8_t ab_state) {
  (void)ccr_sample; /* not stored */

  const uint32_t next = er_next(event_write);
  if (UNLIKELY(next == event_read)) {
    event_drops++;
    Diag_AddEventDrops(1);
    return;
  }

  const uint32_t now = uwTick; /* fast tick */
  uint32_t d = now - last_tick_isr; /* unsigned wrap ok */
  if (d > 0xFFFFu) d = 0xFFFFu;
  last_tick_isr = now;

  encoder_event_t *e = &event_buffer[event_write];
  e->src      = src; /* 0=Z, 1=A, 2=B */
  e->ab_state = (uint8_t)(ab_state & 0x3u);
  e->dms      = (uint16_t)d;

  event_write = next;
}

/**
 * @brief Handle an accepted Z (after phase alignment) in ISR context.
 * @param now_ms  Current uwTick (ms).
 * @param ab_post Gray code sampled after the accepting AB edge.
 * @note  Arms SSZ target for main thread and pushes a Z event.
 */
ALWAYS_INLINE void Gate_OnAcceptedZ(uint32_t now_ms, uint8_t ab_post) {
  /* require Z pin LOW once before next accept */
  if (cap_armed && !cap_active) {
    cap_active = 1u;
    cap_armed  = 0u;
    cap_done   = 0u;
    cap_end_ms = now_ms + cap_duration_ms;
  }

  seed_from_zero_next_ab = 0u;

  Diag_NotifyZAccepted(now_ms);
  push_event_isr(0u, 0u, ab_post);

  /* SSZ seen BEFORE this aligned AB edge; target stamp is the NEXT SSZ (1-based) */
  z_target_ssz_isr = z_last_gap_edges_isr;
}

/**
 * @brief Push one AB edge while capture is active; auto-stops at @ref cap_end_ms.
 * @param src_code 1=A(CC1) or 2=B(CC2)
 * @param ccr      CCR value (unused).
 * @param ab_post  Gray code after edge.
 * @param tms      uwTick (ms).
 */
ALWAYS_INLINE void Gate_PushAB(uint8_t src_code, uint16_t ccr, uint8_t ab_post, uint32_t tms) {
  if (!cap_active) return;

  push_event_isr(src_code, ccr, ab_post);
  Diag_NotifyABEdgeAt(tms);

  if ((int32_t)(tms - cap_end_ms) >= 0) {
    cap_active = 0u;
    cap_done   = 1u;
    __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_CC1 | TIM_IT_CC2 | TIM_IT_IDX);
  }
}

/* ============================ Win-stamp API ============================ */
/**
 * @brief Initialize all per-window stamp arrays and exported stamps.
 */
ALWAYS_INLINE void WinStamp_InitAll(void) {
  memset(win_ab_start_x4, 0, sizeof(win_ab_start_x4));
  memset(win_ab_end_x4,   0, sizeof(win_ab_end_x4));
  for (uint32_t i = 0; i < WINFIFO; ++i) win_z_row[i] = 0xFFFFu;
  memset(win_z_epoch,     0, sizeof(win_z_epoch));
  memset(win_z_seq_end,   0, sizeof(win_z_seq_end));
  memset(win_z_mask, 0, sizeof(win_z_mask));
#if ENC_DEBUG_Z
  memset(win_z_count,     0, sizeof(win_z_count));
#endif

  win_stamp_ab_start_x4 = 0;
  win_stamp_ab_end_x4   = 0;
  win_stamp_ab_delta_x4 = 0;
  win_stamp_z_epoch     = 0;
  win_stamp_z_row       = -1;
  win_stamp_z_seq_end   = 0;

  g_ws.ab_since_z_soft    = 0; /* x4 edges */
  g_ws.z_row_this_window  = 0xFFFFu;
  g_ws.z_epoch_this_window= 0;
  z_seq_main              = 0;
  win_stamp_z_mask = 0;
}

/** @brief One-shot latch to force the next window start to 0 after a Z. */
static volatile uint8_t reset_start_on_next_begin = 0u;

/**
 * @brief Begin building a new window; compute its ABx4 start and clear Z meta.
 * @param wf_w_local Current write index in the window ring.
 */
ALWAYS_INLINE void WinStamp_BeginWindow(uint16_t wf_w_local) {
  const uint16_t prev = idx_prev(wf_w_local);

  if (reset_start_on_next_begin) {
    /* After any accepted/stamped Z, force the next window to export start==0 */
    win_ab_start_x4[wf_w_local] = 0u;
    reset_start_on_next_begin   = 0u;    // one-shot
  } else if (wf_w_local == 0 && wf_count == 0) {
    /* very first window after arming */
    win_ab_start_x4[wf_w_local] = g_ws.ab_since_z_soft;
  } else {
    uint32_t start_raw = win_ab_end_x4[prev];
    if (win_z_row[prev] == (uint16_t)(SEQ_LEN - 1u)) start_raw = 0u;
    win_ab_start_x4[wf_w_local] = start_raw;
  }

  g_ws.z_row_this_window    = 0xFFFFu;
  g_ws.z_epoch_this_window  = 0u;

  z_rows_mask_this_window = 0u;
}

/**
 * @brief Advance SSZ domain by 1 AB edge (done before packing a row).
 */
ALWAYS_INLINE void WinStamp_OnEdge(void) {
  /* bump SSZ so f[7] is 1-based */
  g_ws.ab_since_z_soft++;
}

/**
 * @brief Finalize per-window stamps (ABx4 end, Z row/epoch, mask, Z seq).
 * @param wf_w_local Current write index in the window ring.
 */
ALWAYS_INLINE void WinStamp_PublishWindow(uint16_t wf_w_local) {
  /* end position is always start + SEQ_LEN in x4 edges */
  win_ab_end_x4[wf_w_local] = win_ab_start_x4[wf_w_local] + (uint32_t)SEQ_LEN;

  /* Keep whatever Process stamped (or none) — no last-row forcing */
  win_z_row[wf_w_local]   = g_ws.z_row_this_window;     /* 0xFFFF if none */
  win_z_epoch[wf_w_local] = g_ws.z_epoch_this_window;   /* 0 if none   */

  win_z_mask[wf_w_local]  = z_rows_mask_this_window;

  if (g_ws.z_row_this_window != 0xFFFFu) {
    win_z_seq_end[wf_w_local] = z_seq_main;
  } else {
    if (wf_count > 0) {
      const uint16_t prev = idx_prev(wf_w_local);
      win_z_seq_end[wf_w_local] = win_z_seq_end[prev];
    } else {
      win_z_seq_end[wf_w_local] = 0u;
    }
#if ENC_DEBUG_Z
    win_z_count[wf_w_local] = (uint8_t)z_stamps_in_window;
#endif
  }

}

/**
 * @brief Copy per-window stamps to exported variables for UI/telemetry.
 * @param r Window ring index being dequeued by @ref EncSampler_CopyWindowLinear.
 */
ALWAYS_INLINE void WinStamp_CopyOut(uint16_t r) {
  /* Export 1-based to UI */
  const uint32_t s_raw = win_ab_start_x4[r] % QUAD_EDGES_PER_REV;
  const uint32_t e_raw = win_ab_end_x4[r]   % QUAD_EDGES_PER_REV;

  win_stamp_ab_start_x4 = s_raw + 1u;
  win_stamp_ab_end_x4   = e_raw + 1u;

  win_stamp_ab_delta_x4 = mod_delta_u32(win_ab_end_x4[r], win_ab_start_x4[r], QUAD_EDGES_PER_REV);
  win_stamp_z_epoch     = win_z_epoch[r];
  win_stamp_z_row       = (win_z_row[r] == 0xFFFFu) ? -1 : (int16_t)win_z_row[r];
  win_stamp_z_seq_end   = win_z_seq_end[r];
  win_stamp_z_mask      = win_z_mask[r];

#if ENC_DEBUG_Z
  /* seq continuity: don't jump > #stamps in this slot */
  uint32_t prev_seq = z_seq_last_pub;
  uint32_t this_seq = win_z_seq_end[r];
  uint32_t dz = (this_seq >= prev_seq) ? (this_seq - prev_seq) : 0u;
  if (dz > 1 && prev_seq != 0u) { /* we stamp at most once per window in this scheme */
    ZDBG("WARN: Zseq jump dz=%lu exceeds expected (prev_seq=%lu, this_seq=%lu)",
         (unsigned long)dz, (unsigned long)prev_seq, (unsigned long)this_seq);
  }
  z_seq_last_pub = this_seq;
#endif
}

/* ======================= IRQ enable helpers ============================ */
/** @brief Mirror of AB IRQ enable state. */
static volatile uint8_t ab_irqs_on = 1; /* mirror state for AB (CC1/CC2) */

/**
 * @brief Configure TIM1 interrupts for AB edges and (optionally) IDX.
 * @note  Disables unrelated UPDATE/TRIGGER/COM interrupts.
 */
ALWAYS_INLINE void EncSampler_ConfigIRQs(void) {
  __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_CC1 | TIM_IT_CC2);
#if HAVE_Z_INDEX
  __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_IDX);
#ifdef TIM_SR_IDXF
  __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_IDXF);
#endif
#endif
  __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_UPDATE | TIM_IT_TRIGGER | TIM_IT_COM);
  __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_UIF | TIM_SR_TIF | TIM_SR_COMIF);
}

/**
 * @brief Enable/disable AB (and IDX) interrupts atomically.
 * @param on 1 to enable; 0 to disable.
 */
void EncSampler_SetABIrqs(uint8_t on) {
  __disable_irq();
  if (on) {
    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_CC1 | TIM_IT_CC2);
#if HAVE_Z_INDEX
    __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_IDX);
#ifdef TIM_SR_IDXF
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_IDXF);
#endif
#endif
  } else {
    __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_CC1 | TIM_IT_CC2 | TIM_IT_IDX);
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_CC1IF | TIM_SR_CC1OF | TIM_SR_CC2IF | TIM_SR_CC2OF);
#ifdef TIM_SR_IDXF
    __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_IDXF);
#endif
  }
  ab_irqs_on = on ? 1u : 0u;
  __enable_irq();
}

/**
 * @brief Drain TIM1 encoder/index interrupts into the event ring.
 * @details
 *  Order of handling within a spin:
 *   1) Clear overcaptures and account drops.
 *   2) On IDXF: "arm" potential Z; acceptance happens only on AB CC when AB==00,
 *      honoring soft-debounce and min-edge constraints.
 *   3) Service exactly one CC (CC1 or CC2) per pass, sampling AB and uwTick.
 *   4) If Z was armed and AB==00, accept Z, push Z event, reset SSZ counters.
 *   5) Update diagnostics and high-water marks.
 */
void EncSampler_EncoderIRQ_Drain(void) {
  HAL_GPIO_WritePin(INT_PIN_GPIO_Port, INT_PIN_Pin, GPIO_PIN_SET);
  DiagTiming_Start(DIAG_T_ISR_TOTAL);

  for (int spins = 0;; ++spins) {

    /* clear the LOW-once requirement as soon as Z is actually LOW */
    if (z_need_low_since_arm && (z_pin_now() == 0u)) {
      z_need_low_since_arm = 0u;
    }

    uint32_t sr = TIM1->SR;
    uint32_t pending = sr & (TIM_SR_CC1IF | TIM_SR_CC2IF | TIM_SR_CC1OF | TIM_SR_CC2OF);
#if HAVE_Z_INDEX && defined(TIM_SR_IDXF)
    pending |= (sr & TIM_SR_IDXF);
#endif
    if (!pending) break;
    if (spins > 512) break; /* safety valve */

    /* allow only one Z acceptance per loop pass */
    uint8_t z_accepted_this_pass = 0;

    /* --- Handle CC overcapture --- */
    if (sr & (TIM_SR_CC1OF | TIM_SR_CC2OF)) {
      if (sr & TIM_SR_CC1OF) { event_drops++; Diag_AddEventDrops(1); }
      if (sr & TIM_SR_CC2OF) { event_drops++; Diag_AddEventDrops(1); }
      __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_CC1OF | TIM_SR_CC2OF);
      /* fall through */
    }

    /* --- IDXF: ARM ONLY (do NOT accept here) --- */
#if HAVE_Z_INDEX && defined(TIM_SR_IDXF)
    if (sr & TIM_SR_IDXF) {
      __HAL_TIM_CLEAR_IT(&htim1, TIM_IT_IDX);
# ifdef TIM_SR_IDXF
      __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_IDXF);
# endif
      z_diag_isr_seen++;
      z_isr_seen_cnt++;

      if ((cap_active || cap_armed)) {
        if (z_need_low_since_arm && (z_pin_now() == 0u))
          z_need_low_since_arm = 0u;

        if (!z_need_low_since_arm) {
          const uint32_t now_ms = uwTick;
          const int32_t  dt_ms  = (int32_t)(now_ms - (int32_t)z_last_ms_isr);
          if (dt_ms >= (int32_t)Z_SOFT_DEBOUNCE_MS &&
              ab_edges_since_z_isr >= Z_MIN_AB_EDGES) {
            /* Arm: CC path will accept on next AB edge when AB==00 */
            z_align_pending_isr = 1u;
          }
        }
      }
      /* fall through */
    }
#endif

    /* Refresh SR after possible Z branch to observe fresh CCIF */
    sr = TIM1->SR;

    /* --- Unified CC handler (service one channel per pass) --- */
    const uint32_t have_cc1 = (sr & TIM_SR_CC1IF);
    const uint32_t have_cc2 = (sr & TIM_SR_CC2IF);

    if (have_cc1 | have_cc2) {
      const uint8_t  src_code = have_cc1 ? 1u : 2u;
      const uint16_t ccr      = have_cc1 ? (uint16_t)TIM1->CCR1 : (uint16_t)TIM1->CCR2;

      __HAL_TIM_CLEAR_IT(&htim1, have_cc1 ? TIM_IT_CC1 : TIM_IT_CC2);

      /* Sample AB & time for both acceptance and event push */
      const uint8_t  ab  = enc_read_AB() & 0x3u;
      const uint32_t tms = uwTick;

      /* Accept pending IDXF only when phase-aligned: AB==00 */
      if (!z_accepted_this_pass && z_align_pending_isr) {
        z_last_gap_edges_isr = ab_edges_since_z_isr; /* SSZ before including this AB edge */
        ab_edges_since_z_isr = 0u;
        z_diag_ab_since_isr  = 0u;
        z_diag_steps_x4      = 0u;

        z_last_ms_isr        = tms;
        z_diag_last_z_ms     = tms;
        z_diag_isr_accepted++;
        z_isr_accepted_cnt++;
        z_epoch_isr++;

        /* require Z to go LOW before the next arm/accept */
        z_need_low_since_arm = 1u;

        Gate_OnAcceptedZ(tms, ab); /* pushes evt.src==0 and sets z_target_ssz_isr */
        z_align_pending_isr = 0u;  /* disarm */
        z_accepted_this_pass = 1u; /* prevent CC1+CC2 double-accept */
      }

      /* Always push the AB event (needed for rows & SSZ progression) */
      Gate_PushAB(src_code, ccr, ab, tms);

      /* keep diag counters aligned to “edges since last accepted Z” */
      ab_edges_since_z_isr++;
      z_diag_ab_since_isr = ab_edges_since_z_isr;
      z_diag_steps_x4     = ab_edges_since_z_isr;

      continue;
    }

#ifdef TIM_SR_TERRF
    if (sr & TIM_SR_TERRF) { __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_TERRF); }
#endif
#ifdef TIM_SR_IERRF
    if (sr & TIM_SR_IERRF) { __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_IERRF); }
#endif
  } /* for(spins) */

  /* health (cheap sampling) */
  {
    const uint32_t level = er_level(event_write, event_read);
    if (level > event_ring_hwm) event_ring_hwm = level;
    Diag_ObserveEventBufLevel(level);
  }

  DiagTiming_Stop(DIAG_T_ISR_TOTAL);
  HAL_GPIO_WritePin(INT_PIN_GPIO_Port, INT_PIN_Pin, GPIO_PIN_RESET);
  __DSB();
}

/* ============================== Init & main ============================= */
/**
 * @brief Initialize sampler state and buffers. Call once at boot.
 */
void EncSampler_Init(void) {
  /* Only cold-init the big buffers once to save time / bus bandwidth */
  static uint8_t cold_init_done = 0u;

  memset(&enc_state, 0, sizeof(enc_state));
  enc_state.baseline_interval = 0.0f;

  WinStamp_InitAll();

  /* Fast-path clears that don't touch the big buffers */
  z_align_pending_isr   = 0u;
  z_epoch_seen_main     = z_epoch_isr;
  z_last_gap_edges_isr  = 0u;
  suppress_until_first_Z= 1u;

  seed_from_zero_next_ab= 0u;
  z_pending_main        = 0u;
  z_target_ssz_isr      = 0u;
  z_target_ssz_main     = 0u;

  __disable_irq();
  wf_w = wf_r = 0;
  wf_count = 0;
  win_row = 0;
  win_ready = 0;
  win_total = 0;
  event_read = event_write = 0;
  event_drops = 0;
  last_tick_isr = uwTick;
  z_pin_prev_isr = z_pin_now();
  __enable_irq();

  if (!cold_init_done) {
    memset((void*)win_ring,   0, sizeof(win_ring));
    memset((void*)flags_ring, 0, sizeof(flags_ring));
    memset((void*)event_buffer, 0, sizeof(event_buffer));
    cold_init_done = 1u;
  }

  EncSampler_ConfigIRQs();
  __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_TIF | TIM_SR_COMIF);
}

/**
 * @brief Main processing pump: consume events, build windows, stamp Z, publish.
 * @details
 *  - Applies canonical window start policy (direction-aware) to align rows.
 *  - Packs features in the exact order expected by the model.
 *  - Performs SSZ-accurate Z stamping (f[6]) with first-Z row capture.
 *  - Maintains per-window flags and direction summary.
 *  - Publishes to the window FIFO with overwrite-on-full semantics.
 */
void EncSampler_Process(void) {
  /* LIVE processing with SSZ-accurate Z stamping */

  /* Per-run statics stay in .bss and keep state across calls */
  static uint32_t gray_skip2 = 0;
  static uint8_t  last_src   = 0;
  static int32_t  dir_sum    = 0;
  static uint32_t rule_flags_live = 0;
  static uint8_t  had_z_in_win    = 0;

  /* adjacency computed on the same gray the model sees */
  static uint8_t last_model_gray = 0;
  static uint8_t have_last_model_gray = 0;

  /* Precompute scaled 0/1 once for boolean-like features */
  static uint8_t s01_init = 0;
  static float S0[FEAT_DIM], S1[FEAT_DIM];
  if (!s01_init) {
    for (int j = 0; j < FEAT_DIM; ++j) {
      S0[j] = scale_feat(j, 0.f);
      S1[j] = scale_feat(j, 1.f);
    }
    s01_init = 1;
  }

  while (event_read != event_write) {
    /* Pull one event from the SPSC ring */
    encoder_event_t evt = event_buffer[event_read];
    event_read = er_next(event_read);

    if (evt.src == 0u) {
      /* Z event (from Gate_OnAcceptedZ) */
      z_diag_last_z_ms = z_last_ms_isr;

      if (suppress_until_first_Z) {
        z_pending_main           = 0u;
        g_ws.ab_since_z_soft     = 0u; /* first-Z hygiene only */
        win_row                  = 0;
        had_z_in_win             = 0;
        dir_sum                  = 0;
        have_last_model_gray     = 0;
#if ENC_DEBUG_Z
        z_stamps_in_window       = 0;
#endif
        suppress_until_first_Z   = 0u;
        reset_start_on_next_begin= 1u; /* next window exports start==0 */
        continue;
      }

      z_epoch_seen_main = z_epoch_isr;
      z_seq_main++;

      if (z_last_gap_edges_isr < Z_MIN_GAP_EDGES)           rule_flags_live |= WR_ZDOUBLE;
      if (g_ws.ab_since_z_soft > Z_MISS_EDGE_LIMIT)         rule_flags_live |= WR_ZMISS;

      /* Arm SSZ-accurate stamping; do NOT reset SSZ here. */
      z_target_ssz_main = z_target_ssz_isr; /* 1-based target within SSZ domain */
      z_pending_main    = 1u;

//      RESET_START_ON_NEXT_BEGIN = 1U;
      continue;
    }

    /* keep public Z time fresh for UI (coarse) */
    z_diag_last_z_ms = z_last_ms_isr;

#if ENABLE_TRACE_WIN
    if (win_row == 0) memset(trace_z, 0, SEQ_LEN);
#endif
    const uint8_t current_gray = (uint8_t)(evt.ab_state & 0x3u);

//    /* First AB of a window: stamp begin metadata now */
    if (win_row == 0) {
      if (suppress_until_first_Z) {
        continue; /* ignore edges until that first Z clears suppression */
      }
#if ENC_DEBUG_Z
      z_stamps_in_window = 0;
#endif
      const uint8_t allow_immediate = (!suppress_until_first_Z) && (z_pending_main > 0u);
      if (!allow_immediate) {
        /* Direction-aware canonical start */
        const int8_t dir = enc_latched_dir;  // +1=fwd, -1=bwd, 0=unknown
        uint8_t canonical_start;

        if (dir < 0) {
          /* Backward: falling variants */
          canonical_start =
              ((evt.src == 2u) && !(current_gray & 1u)) ||  /* B edge, B==0 */
              ((evt.src == 1u) && !(current_gray & 2u));    /* A edge, A==0 */
        } else if (dir > 0) {
          /* Forward: rising variants */
          canonical_start =
              ((evt.src == 2u) &&  (current_gray & 1u)) ||  /* B edge, B==1 */
              ((evt.src == 1u) &&  (current_gray & 2u));    /* A edge, A==1 */
        } else {
          /* Unknown: be permissive (any first edge) */
          canonical_start = 1u;
        }

        if (!canonical_start) {
          have_last_model_gray = 0;
          continue;                 /* wait until a canonical start edge arrives */
        }
      }


    	    had_z_in_win = 0;
    	    DiagTiming_Start(DIAG_T_E2E_WINDOW);
    	    WinStamp_BeginWindow(wf_w);
    	  }
    /* --- timing features (EWMA) --- */
    int32_t dms = (int32_t)evt.dms;
    if (dms <= 0) dms = 1;
    const float dt_ms = (float)dms;
    if (enc_state.baseline_interval < 1.0f) enc_state.baseline_interval = dt_ms;
    const float ratio = dt_ms / enc_state.baseline_interval;
    enc_state.speed_change_rate  = fabsf(ratio - 1.0f);
    enc_state.baseline_interval  = 0.9f * enc_state.baseline_interval + 0.1f * dt_ms;

    /* Each AB edge advances SSZ BEFORE we pack features, so f[7] is 1-based */
    if (!suppress_until_first_Z) {
      WinStamp_OnEdge();
    }

    DiagTiming_Start(DIAG_T_PACK_AB);
    float *f = &win_ring[wf_w][((uint32_t)win_row * FEAT_DIM)];

    /* 1) Z lane: turn on iff we have a pending Z AND SSZ matches target */
    const uint8_t should_stamp_z =
      (uint8_t)(z_pending_main && (g_ws.ab_since_z_soft >= z_target_ssz_main));
    f[6] = should_stamp_z ? S1[6] : S0[6];
    if (should_stamp_z) {
      z_pending_main = 0u;
      g_ws.ab_since_z_soft = 0u;   // reset SSZ domain at the exact stamp
      z_target_ssz_main    = 0u;
      reset_start_on_next_begin = 1u;

      /* record per-window Z metadata (first Z row + current/last seq) */
      if (g_ws.z_row_this_window == 0xFFFFu) {
        g_ws.z_row_this_window   = win_row;
        g_ws.z_epoch_this_window = z_seq_main;
        win_z_row[wf_w]          = win_row;
        win_z_epoch[wf_w]        = z_seq_main;
      }
      z_rows_mask_this_window |= (1u << win_row);
      win_z_seq_end[wf_w] = z_seq_main;
      flags_ring[wf_w]   |= WR_ZINWIN;
      had_z_in_win        = 1;
#if ENABLE_TRACE_WIN
      trace_z[win_row] = 1u;
#endif
    } else {
#if ENABLE_TRACE_WIN
      trace_z[win_row] = 0u;
#endif
    }

    /* 2) model lanes based on current Gray and previous Gray */
    uint8_t model_gray = (uint8_t)(evt.ab_state & 0x3u);
    uint8_t src_A_model = 0, src_B_model = 0;
    uint8_t adjacent = 1u;
    int8_t  step_dir = 0;

    if (seed_from_zero_next_ab && win_row == 0 && g_ws.ab_since_z_soft == 1u) {
      have_last_model_gray = 1u;
      last_model_gray = 0u;
      seed_from_zero_next_ab = 0u;
    }

    if (have_last_model_gray) {
      const uint8_t diff = (uint8_t)((last_model_gray ^ model_gray) & 0x3u);
      adjacent = (uint8_t)((diff == 1u) || (diff == 2u));
      gray_step_to_src(last_model_gray, model_gray, &src_A_model, &src_B_model);
      step_dir = quad_dir_from_transition(last_model_gray, model_gray);
      if (!adjacent) { src_A_model = 0; src_B_model = 0; step_dir = 0; }
    } else {
      src_A_model = (evt.src == 1u);
      src_B_model = (evt.src == 2u);
      have_last_model_gray = 1u;
      step_dir = 0;
    }
    last_model_gray = model_gray;

    if (step_dir != 0) enc_latched_dir = step_dir;
    if (step_dir > 0)      dir_sum++;
    else if (step_dir < 0) dir_sum--;

    const uint8_t  dir_fwd      = (step_dir > 0);
    const uint8_t  dir_bwd      = (step_dir < 0);
    const uint32_t ab_for_model = g_ws.ab_since_z_soft;

    const uint8_t src_code_model = src_A_model ? 1u : (src_B_model ? 2u : 0u);
    const uint8_t is_same_src    = (uint8_t)(last_src && (src_code_model == last_src));

    /* 3) write remaining features (never touch f[6] again) */
    f[0] = scale_feat(0, (float)model_gray);
    f[1] = src_A_model ? S1[1] : S0[1];
    f[2] = src_B_model ? S1[2] : S0[2];
    f[3] = dir_fwd     ? S1[3] : S0[3];
    f[4] = dir_bwd     ? S1[4] : S0[4];
    f[5] = scale_feat(5, enc_state.speed_change_rate);
    /* f[6] already set */
    f[7] = scale_feat(7, (float)ab_for_model);
    f[8] = is_same_src ? S1[8] : S0[8];
    f[9] = adjacent    ? S1[9] : S0[9];

#if ENABLE_TRACE_WIN
    trace_ab[win_row]  = model_gray;
    trace_src[win_row] = src_code_model ? src_code_model : evt.src;
#endif

    if (is_same_src) gray_skip2++;
    last_src = src_code_model ? src_code_model : evt.src;

    enc_state.last_AB_state = model_gray;
    DiagTiming_Stop(DIAG_T_PACK_AB);

    /* advance row, publish window if full */
    win_row++;

    if (win_row >= SEQ_LEN) {
      uint32_t flags = rule_flags_live;
      if (gray_skip2 >= 2) flags |= WR_SKIP2;
      if (had_z_in_win)    flags |= WR_ZINWIN;

      const uint32_t win_last_ms = uwTick; /* good enough for gating/printing */
      if (Diag_IsTrialCovering(win_last_ms)) {
        flags |= RF_TRIAL;
      }

      /* summarize direction */
      if      (dir_sum > 0) last_window_dir = +1;
      else if (dir_sum < 0) last_window_dir = -1;
      else                  last_window_dir = 0; /* neutral if perfectly balanced */
      enc_latched_dir = last_window_dir; /* keep getter meaningful */
      dir_sum = 0;

      DiagTiming_Stop(DIAG_T_E2E_WINDOW);
      DiagTiming_Start(DIAG_T_WIN_PUBLISH);

      if (suppress_until_first_Z && (z_seq_main == 0u)) {
        /* don’t publish this window; reset accumulators and continue */
        win_row = 0;
        gray_skip2 = 0;
        rule_flags_live = 0;
        last_src = 0;
        had_z_in_win = 0;
        DiagTiming_Stop(DIAG_T_WIN_PUBLISH);
        continue;
      }
      if (suppress_until_first_Z && (z_seq_main > 0u)) {
        suppress_until_first_Z = 0u;
      }

      /* publish stamped metadata BEFORE rotating wf_w */
      WinStamp_PublishWindow(wf_w);
      flags_ring[wf_w] = flags;

#if ENABLE_TRACE_WIN
      memcpy(last_trace_ab,  trace_ab,  SEQ_LEN);
      memcpy(last_trace_src, trace_src, SEQ_LEN);
      memcpy(last_trace_z,   trace_z,   SEQ_LEN);
#endif

      uint8_t dropped_oldest = 0;

      __disable_irq();
      wf_w = idx_next(wf_w);
      if (wf_count < WINFIFO) {
        wf_count++;
      } else {
        wf_r = idx_next(wf_r); /* overwrite oldest */
        dropped_oldest = 1;
      }
      win_ready = wf_count;
      __enable_irq();

      if (wf_count > winfifo_hwm) winfifo_hwm = wf_count;
      if (dropped_oldest) Diag_AddWinOverwrites(1);

      Diag_ObserveWinFifoLevel(wf_count);
      win_total++;

      DiagTiming_Stop(DIAG_T_WIN_PUBLISH);
      Diag_NotifyWindowPublished();

      /* reset window accumulators */
      win_row = 0;
      gray_skip2 = 0;
      rule_flags_live = 0;
      last_src = 0;
      had_z_in_win = 0;
      /* keep last_model_gray & have_last_model_gray across windows */
    }
  }
}

/* ============================== Copy & control ============================== */
/**
 * @brief Copy the oldest ready window into @p dst and pop it from the FIFO.
 * @param dst       Pointer to a buffer of size SEQ_LEN*FEAT_DIM (float).
 * @param out_flags [out] Optional pointer to receive the window flags.
 * @retval 0 on success, -1 if no window is available.
 * @post  Exports stamps for the dequeued window via win_stamp_* variables.
 */
int EncSampler_CopyWindowLinear(float *dst, uint32_t *out_flags) {
  uint16_t r;
  uint32_t flags;

  __disable_irq();
  if (wf_count == 0) {
    __enable_irq();
    return -1;
  }
  r     = wf_r;
  flags = flags_ring[r];
  wf_r  = idx_next(wf_r);
  wf_count--;
  win_ready = wf_count;
  __enable_irq();

  memcpy(dst, win_ring[r], sizeof(win_ring[0]));
  win_rule_flags = flags;
  if (out_flags) *out_flags = flags;

  /* hand off the stamps for this window */
  WinStamp_CopyOut(r);
  return 0;
}

/**
 * @brief Stop TIM1 encoder hardware and disable its interrupts.
 * @note  Safe to call multiple times.
 */
void EncSampler_StopHardware(void) {
  __disable_irq();

  if (!htim1.Instance) { __enable_irq(); return; }

  /* If already disabled, skip heavy work */
  if ((htim1.Instance->DIER & (TIM_DIER_CC1IE | TIM_DIER_CC2IE)) == 0u) {
    __enable_irq();
    return;
  }

  __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_CC1 | TIM_IT_CC2 | TIM_IT_UPDATE | TIM_IT_TRIGGER | TIM_IT_COM);
#ifdef HAVE_Z_INDEX
  __HAL_TIM_DISABLE_IT(&htim1, TIM_IT_IDX);
#endif
  __HAL_TIM_DISABLE(&htim1);

  __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_CC1IF | TIM_SR_CC1OF | TIM_SR_CC2IF | TIM_SR_CC2OF | TIM_SR_UIF | TIM_SR_TIF | TIM_SR_COMIF);
#if HAVE_Z_INDEX && defined(TIM_SR_IDXF)
  __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_IDXF);
#endif

#ifdef TIM1_CC_IRQn
  NVIC_DisableIRQ(TIM1_CC_IRQn);
#endif
#if defined(TIM1_TRG_CCU_IRQn)
  NVIC_DisableIRQ(TIM1_TRG_CCU_IRQn);
#elif defined(TIM1_TRG_COM_IRQn)
  NVIC_DisableIRQ(TIM1_TRG_COM_IRQn);
#endif

  ab_irqs_on = 0;
  __enable_irq();
}

/**
 * @brief Flush all windows and events; reset counters and diagnostics.
 * @note  Keeps buffers allocated; use @ref EncSampler_Init for cold-init.
 */
void EncSampler_FlushWindows(void) {
  __disable_irq();

  wf_w = wf_r = 0;
  wf_count = 0;
  win_ready = 0;
  win_total = 0;
  win_row = 0;

  event_read = event_write = 0;
  event_drops = 0;
  last_tick_isr = uwTick;
  z_pin_prev_isr = z_pin_now();

  z_last_ms_isr = 0;
  ab_edges_since_z_isr = 0;
  z_last_gap_edges_isr = 0;
  z_diag_isr_seen = 0;
  z_diag_isr_accepted = 0;
  z_diag_ab_since_isr = 0;
  z_diag_steps_x4 = 0;
  z_diag_last_z_ms = 0;
  z_align_pending_isr = 0u;
  z_epoch_isr = 0;
  z_epoch_seen_main = 0;

  z_pending_main        = 0u;
  z_target_ssz_isr      = 0u;
  z_target_ssz_main     = 0u;
  // EncSampler_FlushWindows()
  memset(win_z_mask, 0, sizeof(win_z_mask));
  z_rows_mask_this_window = 0;

  suppress_until_first_Z = 1u;
  event_ring_hwm = 0;
  winfifo_hwm = 0;
  cap_armed = 0u;
  cap_active = 0u;
  cap_done = 0u;
  cap_end_ms = 0u;
  cap_duration_ms = 0u;
  WinStamp_InitAll();

  enc_latched_dir = 0;
  last_window_dir = 0;
#if ENC_DEBUG_Z
  memset(win_z_count, 0, sizeof(win_z_count));
#endif

  __enable_irq();
}

/**
 * @brief Whether there is any pending work: events or a partial window in progress.
 * @return 1 if pending, 0 otherwise.
 */
uint8_t EncSampler_HasPending(void) {
  uint8_t p;
  __disable_irq();
  p = ((event_write != event_read) || ((win_row != 0) && ab_irqs_on)) ? 1u : 0u;
  __enable_irq();
  return p;
}

/**
 * @brief Arm a 1-second capture “trial”: start accepting edges and reset SSZ.
 * @note  Resets canonical start state and enforces Z-low-once gating.
 */
void EncSampler_OnTrialArm(void) {
  __disable_irq();

  cap_duration_ms = 1000u;
  cap_armed  = 0u;
  cap_active = 1u;
  cap_done   = 0u;
  cap_end_ms = uwTick + cap_duration_ms;
  enc_state.baseline_interval = 0.0f;

  seed_from_zero_next_ab = 1u;

  z_need_low_since_arm = z_pin_now();

  suppress_until_first_Z = 1u;
  z_seq_main = 0u;
  g_ws.ab_since_z_soft = 0u;
  win_row = 0;

  last_tick_isr  = uwTick;
  z_pin_prev_isr = z_pin_now();

#if HAVE_Z_INDEX && defined(TIM_SR_IDXF)
  __HAL_TIM_CLEAR_FLAG(&htim1, TIM_SR_IDXF);
#endif

  __enable_irq();
}

/**
 * @brief Stop the active capture trial (if any).
 */
void EncSampler_OnTrialStop(void) {
  __disable_irq();
  cap_armed = 0u;
  cap_active = 0u;
  cap_done = 0u;
  __enable_irq();
}

/** @brief Peak occupancy of the event ring since last reset. */
uint32_t EncSampler_GetEventRingHWM(void) { return event_ring_hwm; }
/** @brief Peak occupancy of the window FIFO since last reset. */
uint32_t EncSampler_GetWinFifoHWM(void)   { return winfifo_hwm;   }

/**
 * @brief Whether the 1-second capture is currently active.
 * @return 1 if active, 0 otherwise.
 */
uint8_t EncSampler_IsCaptureActive(void) {
  extern volatile uint8_t cap_active; // already in this TU
  return cap_active;
}

/* ============================== Trace API ============================== */
#if ENABLE_TRACE_WIN
/**
 * @brief Retrieve traces for the last published window.
 * @param ab  [out] Gray codes per row (0..3), length SEQ_LEN.
 * @param src [out] Source per row (0=A,1=A,2=B), length SEQ_LEN.
 * @param z   [out] Z mask per row (0/1), length SEQ_LEN.
 */
void EncSampler_DebugGetLastTrace(uint8_t *ab, uint8_t *src, uint8_t *z) {
  if (ab)  memcpy(ab,  last_trace_ab,  SEQ_LEN);
  if (src) memcpy(src, last_trace_src, SEQ_LEN);
  if (z)   memcpy(z,   last_trace_z,   SEQ_LEN);
}
#else
/**
 * @brief Stub when tracing is disabled; fills outputs with zeros.
 */
void EncSampler_DebugGetLastTrace(uint8_t *ab, uint8_t *src, uint8_t *z) {
  if (ab)  memset(ab,  0, SEQ_LEN);
  if (src) memset(src, 0, SEQ_LEN);
  if (z)   memset(z,   0, SEQ_LEN);
}
#endif
