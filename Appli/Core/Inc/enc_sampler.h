#ifndef INC_ENC_SAMPLER_H_
#define INC_ENC_SAMPLER_H_

/**
 * @file    enc_sampler.h
 * @brief   Encoder sampler: ISR-driven event capture and fixed-length windowing.
 *
 * @details
 *   - ISR path pushes compact AB/Z events into a lock-free SPSC ring.
 *   - Main path consumes events, builds SEQ_LEN×FEAT_DIM float windows,
 *     stamps Z with SSZ accuracy, and publishes per-window flags/metadata.
 *   - Public API exposes FIFO status, diagnostics, and a linear copy function.
 */

#include <stdint.h>
#include <stdbool.h>
#include "model_meta.h"   /**< Provides MODEL_SEQ_LEN / MODEL_FEAT_DIM. */

#ifdef __cplusplus
extern "C" {
#endif

/* ========= Public constants ========= */

/**
 * @brief Number of rows per window (time steps).
 * @see MODEL_SEQ_LEN (generated in model_meta.h)
 */
#define SEQ_LEN   (MODEL_SEQ_LEN)

/**
 * @brief Number of features per row.
 * @see MODEL_FEAT_DIM (generated in model_meta.h)
 */
#define FEAT_DIM  (MODEL_FEAT_DIM)

/* ---------- Attribute helpers (safe in headers) ---------- */

/**
 * @brief Place an object in a named linker section with 32-byte alignment.
 * @param name Section name (string literal).
 */
#ifndef ENC_SECTION
#define ENC_SECTION(name) __attribute__((section(name), aligned(32)))
#endif

/**
 * @brief Mark a symbol as used to prevent dead-stripping.
 */
#ifndef ENC_USED
#define ENC_USED __attribute__((used))
#endif

/**
 * @brief Enable per-window AB/SRC/Z trace capture.
 * @details When 1, the implementation preserves a “last-window” trace
 *          retrievable via @ref EncSampler_DebugGetLastTrace. When 0, the
 *          API is a no-op but remains link-compatible.
 */
#ifndef ENABLE_TRACE_WIN
#define ENABLE_TRACE_WIN 1
#endif

/**
 * @brief Window FIFO capacity (number of windows retained).
 * @note Must be >= 1. Overflows overwrite the oldest window.
 */
#ifndef WINFIFO
#define WINFIFO 3800u
#endif

/**
 * @brief Size of the ISR event ring buffer (number of events).
 * @note Tune according to edge rates and main-loop service latency.
 */
#ifndef EVENT_BUFFER_SIZE
#define EVENT_BUFFER_SIZE 80000u   /* exact static number you want */
#endif

/**
 * @brief Rule flag: window was captured during a “trial” interval.
 * @see EncSampler_OnTrialArm()
 */
#define RF_TRIAL (1u << 0)

/* ========= Public state (read-only to app) ========= */

/**
 * @brief Rolling/live flags for the window currently being packed.
 * @details Bitfield; meanings are implementation-defined (e.g., bounce, double-Z).
 */
extern volatile uint32_t enc_rule_flags;

/**
 * @brief Flags associated with the last dequeued window.
 * @details Stable snapshot corresponding to the most recent call to
 *          @ref EncSampler_CopyWindowLinear.
 */
extern volatile uint32_t win_rule_flags;

/* ---------- FIFO flow control ---------- */

/** @brief Number of windows currently available to copy (0..WINFIFO). */
extern volatile uint32_t win_ready;
/** @brief Total number of windows produced since init/flush. */
extern volatile uint32_t win_total;
/** @brief Count of ISR event drops (overcapture / ring overflow). */
extern volatile uint32_t event_drops;

/* ---------- Z-path diagnostics consumed by the app logger ---------- */

/** @brief ISR: total Z pulses observed (IDXF seen). */
extern volatile uint32_t z_diag_isr_seen;
/** @brief ISR: total Z pulses accepted post-gating/alignment. */
extern volatile uint32_t z_diag_isr_accepted;
/** @brief ISR: AB edges since last accepted Z (running). */
extern volatile uint32_t z_diag_ab_since_isr;
/** @brief ISR: steps×4 since last accepted Z (alias of ab_since). */
extern volatile uint32_t z_diag_steps_x4;
/** @brief Coarse ms timestamp of last accepted Z. */
extern volatile uint32_t z_diag_last_z_ms;

/* ---------- Window stamps exported to app_x-cube-ai ---------- */

/**
 * @brief AB×4 start position for the last copied window (1-based, modulo rev).
 */
extern volatile uint32_t win_stamp_ab_start_x4;
/**
 * @brief AB×4 end position for the last copied window (1-based, modulo rev).
 */
extern volatile uint32_t win_stamp_ab_end_x4;
/**
 * @brief AB×4 span (end - start) modulo revolution for the last copied window.
 */
extern volatile uint32_t win_stamp_ab_delta_x4;
/**
 * @brief Z epoch mirrored into the last copied window (0 if none in window).
 */
extern volatile uint32_t win_stamp_z_epoch;
/**
 * @brief First Z row in the last copied window (-1 if none).
 */
extern volatile int16_t  win_stamp_z_row;
/**
 * @brief Monotonic Z sequence number at the end of the last copied window.
 */
extern volatile uint32_t win_stamp_z_seq_end;

/* ========= API ========= */

/**
 * @brief Initialize sampler state and configure IRQs.
 * @details Call once at boot. Buffers are cold-initialized on first call.
 */
void EncSampler_Init(void);

/**
 * @brief Stop TIM1 encoder hardware and disable its interrupts.
 * @details Safe to call multiple times.
 */
void EncSampler_StopHardware(void);

/**
 * @brief Flush windows and events; reset counters and diagnostics.
 * @details Keeps buffers allocated; does not reconfigure hardware.
 */
void EncSampler_FlushWindows(void);

/**
 * @brief Atomically enable or disable AB (and IDX if present) interrupts.
 * @param on 1 to enable, 0 to disable.
 */
void EncSampler_SetABIrqs(uint8_t on);

/**
 * @brief Drain TIM1 encoder/index interrupts into the event ring.
 * @details Call from the TIM1 CC and/or TRG/COM ISR contexts.
 */
void EncSampler_EncoderIRQ_Drain(void);

/**
 * @brief Consume events, build windows, stamp Z, and publish to FIFO.
 * @details Call from the main loop (non-ISR).
 */
void EncSampler_Process(void);

/* ---------- Direction helpers ---------- */

/**
 * @brief Get the last latched direction from edge transitions.
 * @return +1 forward, -1 backward, 0 unknown/neutral.
 */
int8_t EncSampler_GetLatchedDir(void);

/**
 * @brief Get the direction summary for the last published window.
 * @return +1 forward, -1 backward, 0 neutral.
 */
int8_t EncSampler_GetLastWindowDir(void);

/**
 * @brief Copy the oldest ready window into @p dst and pop it from the FIFO.
 *
 * @param[out] dst       Buffer for SEQ_LEN*FEAT_DIM floats (row-major).
 * @param[out] out_flags Optional; receives per-window rule flags.
 * @retval 0  Success, a window was copied and stamps were exported.
 * @retval -1 No window available (non-blocking).
 *
 * @post On success, @ref win_rule_flags and the win_stamp_* exports correspond
 *       to the dequeued window.
 */
int EncSampler_CopyWindowLinear(float *dst, uint32_t *out_flags);

/**
 * @brief Retrieve last-window AB/SRC/Z traces.
 * @param[out] ab  Gray codes per row (0..3), length SEQ_LEN (cleared if tracing off).
 * @param[out] src Source per row (0=A,1=A,2=B), length SEQ_LEN.
 * @param[out] z   Z mask per row (0/1), length SEQ_LEN.
 * @note When @ref ENABLE_TRACE_WIN == 0, this function zero-fills the outputs.
 */
void EncSampler_DebugGetLastTrace(uint8_t *ab, uint8_t *src, uint8_t *z);

/* ---------- Utility ---------- */

/**
 * @brief Whether there is pending work (events buffered or partial window).
 * @return 1 if pending, 0 otherwise.
 */
uint8_t EncSampler_HasPending(void);

/** @brief Peak occupancy of the event ring since last reset. */
uint32_t EncSampler_GetEventRingHWM(void);
/** @brief Peak occupancy of the window FIFO since last reset. */
uint32_t EncSampler_GetWinFifoHWM(void);

/**
 * @brief Arm a 1-second capture “trial” interval.
 * @details Starts accepting edges and resets SSZ domain. Windows produced
 *          during the interval are flagged with @ref RF_TRIAL.
 */
void EncSampler_OnTrialArm(void);

/**
 * @brief Whether the 1-second capture is currently active.
 * @return 1 if active, 0 otherwise.
 */
uint8_t EncSampler_IsCaptureActive(void);

#ifdef __cplusplus
}
#endif
#endif /* INC_ENC_SAMPLER_H_ */
