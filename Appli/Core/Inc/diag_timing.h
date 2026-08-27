#ifndef DIAG_TIMING_H
#define DIAG_TIMING_H

/**
 * @file    diag_timing.h
 * @brief   Lightweight cycle-timing, buffer health, and trial-capture utilities.
 *
 * @details
 *  - Exposes scoped timers (cycle-based) with min/avg/max aggregation.
 *  - Tracks event/window buffer health (capacity, HWM, drops).
 *  - Provides a 1-second (configurable) “trial” capture aligned to next Z.
 *  - Computes simple instantaneous/averaged rotational speed from Z pulses.
 */

#include <stdint.h>
#include <stdbool.h>
#include "stm32n6xx_hal.h"
#include "main.h"   /* for INT_PIN_Pin / MODEL_PIN_Pin ports/pins */

/* ========= Scope pins (oscilloscope) ========= */
/**
 * @brief Drive the INT scope pin high.
 * @note  Intended for coarse visibility on ISR sections, etc.
 */
#define DIAG_INT_PIN_SET()    HAL_GPIO_WritePin(INT_PIN_GPIO_Port,   INT_PIN_Pin,   GPIO_PIN_SET)
/** @brief Drive the INT scope pin low. */
#define DIAG_INT_PIN_CLR()    HAL_GPIO_WritePin(INT_PIN_GPIO_Port,   INT_PIN_Pin,   GPIO_PIN_RESET)
/** @brief Drive the MODEL scope pin high (wraps model execution). */
#define DIAG_MODEL_PIN_SET()  HAL_GPIO_WritePin(MODEL_PIN_GPIO_Port, MODEL_PIN_Pin, GPIO_PIN_SET)
/** @brief Drive the MODEL scope pin low. */
#define DIAG_MODEL_PIN_CLR()  HAL_GPIO_WritePin(MODEL_PIN_GPIO_Port, MODEL_PIN_Pin, GPIO_PIN_RESET)

/* ========= Timers you can measure ========= */
/**
 * @brief IDs for cycle-timers that can be started/stopped and aggregated.
 */
typedef enum {
  DIAG_T_ISR_TOTAL = 0,     /**< Whole encoder IRQ service time. */
  DIAG_T_ISR_Z,             /**< Optional: Z branch inside IRQ. */
  DIAG_T_ISR_AB,            /**< Optional: CC1/CC2 branch inside IRQ. */
  DIAG_T_PACK_AB,           /**< AB event → features packing (main loop). */
  DIAG_T_WIN_PUBLISH,       /**< Full window publish (flags/memcpy/indices). */
  DIAG_T_MODEL,             /**< ai_run() wall time. */
  DIAG_T_E2E_WINDOW,        /**< First-row-in-window → classification ready. */
  DIAG_T__COUNT             /**< Sentinel: number of timers. */
} diag_timer_id_t;

/**
 * @brief Aggregated stats for a single timer (cycle domain).
 */
typedef struct {
  uint32_t count;       /**< Number of samples accumulated. */
  uint64_t sum_cycles;  /**< Sum of cycles over all samples. */
  uint32_t min_cycles;  /**< Minimum cycles observed (per sample). */
  uint32_t max_cycles;  /**< Maximum cycles observed (per sample). */
} diag_timer_stat_t;

/**
 * @brief Snapshot of timer statistics and buffer/system health.
 * @details Filled by @ref Diag_GetAndResetSnapshot and suitable for logging.
 */
typedef struct {
  diag_timer_stat_t timers[DIAG_T__COUNT]; /**< Per-timer aggregates. */

  /* health / saturation (copy in from your app when you dump) */
  uint32_t event_buf_cap;  /**< Event ring capacity (elements). */
  uint32_t event_buf_hwm;  /**< Event ring high-water mark since last reset. */
  uint32_t event_drops;    /**< Number of dropped events since last reset. */

  uint32_t winfifo_cap;    /**< Window FIFO capacity (windows). */
  uint32_t winfifo_hwm;    /**< Window FIFO high-water mark since last reset. */

  /* optional: model count in this reporting period */
  uint32_t inferences;     /**< Number of inferences counted in the period. */
  uint32_t win_overwrites; /**< Windows overwritten due to FIFO full. */
} diag_snapshot_t;

/* ========= 1-second trial capture (arm after next Z) ========= */
/** @brief Maximum histogram size for trial class counts. */
#ifndef DIAG_MAX_CLASSES
#define DIAG_MAX_CLASSES  8u
#endif

/**
 * @brief Trial configuration/state and counters (aligned to next accepted Z).
 */
typedef struct {
  bool     armed_after_z;   /**< Waiting for next Z to start. */
  bool     running;         /**< Trial is active. */
  bool     done;            /**< Trial finished & latched until fetch/reset. */
  uint32_t duration_ms;     /**< Target duration (e.g., 1000 ms). */
  uint32_t t_start_ms;      /**< Start time (ms, HAL_GetTick). */
  uint32_t t_end_ms;        /**< End time (ms). */

  /* counters collected while running */
  uint32_t ab_edges;        /**< AB edges observed during trial. */
  uint32_t windows;         /**< Windows published during trial. */
  uint32_t inferences;      /**< Inferences executed during trial. */
  uint32_t class_hist[DIAG_MAX_CLASSES]; /**< Top-class histogram. */
} diag_trial_t;

/**
 * @brief Simple rotational speed telemetry derived from Z pulses.
 */
typedef struct {
  volatile uint32_t last_z_ms;   /**< Timestamp of last accepted Z (ms). */
  volatile uint32_t period_ms;   /**< Most recent Z-to-Z period (ms). */
  volatile float    rps_inst;    /**< Instantaneous rev/s = 1000 / period_ms. */
  volatile float    rps_ewma;    /**< Smoothed rev/s (EWMA). */
  volatile float    rpm_inst;    /**< Instantaneous RPM. */
  volatile float    rpm_ewma;    /**< Smoothed RPM. */
} td_speed_t;


/* ========= Public API ========= */

/**
 * @brief Enable DWT cycle counter and reset all timing stats.
 * @note  Call once in main() after HAL/clock init.
 */
void DiagTiming_Init(void);

/**
 * @brief Reset all timer aggregates (does not touch the trial state).
 */
void DiagTiming_ResetTimers(void);

/**
 * @brief Start/stop a timer by ID. Not re-entrant per ID.
 * @param id Timer identifier from @ref diag_timer_id_t.
 */
void DiagTiming_Start(diag_timer_id_t id);
/** @copydoc DiagTiming_Start */
void DiagTiming_Stop(diag_timer_id_t id);

/**
 * @brief Convert CPU cycles to nanoseconds using SystemCoreClock.
 * @param cycles Cycle count.
 * @return Approximate time in nanoseconds.
 */
static inline uint32_t Diag_CyclesToNs(uint32_t cycles) {
  /* cycles / (Hz) = seconds -> *1e9 for ns */
  /* To keep it simple and fast, do (cycles * 1000) / (SystemCoreClock/1000000) for ns */
  return (uint32_t)((((uint64_t)cycles) * 1000000000ull) / (uint64_t)SystemCoreClock);
}

/**
 * @brief Convert CPU cycles to microseconds using SystemCoreClock.
 * @param cycles Cycle count.
 * @return Approximate time in microseconds.
 */
static inline uint32_t Diag_CyclesToUs(uint32_t cycles) {
  return (uint32_t)((((uint64_t)cycles) * 1000000ull) / (uint64_t)SystemCoreClock);
}

/* ----- Health / watermarks ----- */
/** @brief Set event ring capacity (for reporting). */
void Diag_SetEventBufCaps(uint32_t capacity);
/** @brief Observe current event ring level; updates internal HWM. */
void Diag_ObserveEventBufLevel(uint32_t level);
/**
 * @brief Add to event drop counter.
 * @param drops Delta to add. If you maintain an absolute counter in the app,
 *              convert to delta before calling.
 */
void Diag_AddEventDrops(uint32_t drops);
/** @brief Set window FIFO capacity (for reporting). */
void Diag_SetWinFifoCaps(uint32_t capacity);
/** @brief Observe current window FIFO level; updates internal HWM. */
void Diag_ObserveWinFifoLevel(uint32_t level);
/** @brief Increment inference counter (call after successful ai_run). */
void Diag_IncInferenceCount(void);

/**
 * @brief Take a snapshot of timers/health and reset incremental counters.
 * @param[out] out Populated snapshot; must be non-NULL.
 */
void Diag_GetAndResetSnapshot(diag_snapshot_t *out);

/* ========= Trial control ========= */
/** @brief Clear all trial state (disarmed, not running, counters zeroed). */
void Diag_TrialReset(void);
/**
 * @brief Arm a trial to start on the next accepted Z.
 * @param duration_ms Trial duration in milliseconds (0 → 1000).
 */
void Diag_ArmTrialAfterNextZ(uint32_t duration_ms);
/**
 * @brief Notify that a Z was accepted (call from Z-accept ISR).
 * @param now_ms HAL_GetTick() at the moment of acceptance.
 */
void Diag_NotifyZAccepted(uint32_t now_ms);
/** @brief Notify once per published window during the trial window. */
void Diag_NotifyWindowPublished(void);
/**
 * @brief Notify the top class after each inference (for trial histogram).
 * @param top_class_index Index of the top class (must be < DIAG_MAX_CLASSES).
 */
void Diag_NotifyInferenceTop(uint32_t top_class_index);
/**
 * @brief Compute rev/s during/after a trial using AB edges and duration.
 * @param[out] rps_inst Instantaneous rps (same as average here).
 * @param[out] rps_avg  Averaged rps over the trial.
 */
void Diag_GetRps(float *rps_inst, float *rps_avg);

/**
 * @brief Notify that one AB edge occurred at a specific time.
 * @param event_ms Timestamp (ms) to use for trial stop-checks.
 */
void Diag_NotifyABEdgeAt(uint32_t event_ms);
/**
 * @brief Convenience: notify AB edge using HAL_GetTick() now.
 */
static inline void Diag_NotifyABEdge(void) { Diag_NotifyABEdgeAt(HAL_GetTick()); }

/**
 * @brief Peek at the current/last trial state (no reset).
 * @return Pointer to internal trial struct; valid until next API call.
 * @warning Read-only; call @ref Diag_TrialReset to clear after consuming.
 */
const diag_trial_t* Diag_TrialPeek(void);

/**
 * @brief Pretty-print a previously captured snapshot using printf().
 * @param snap Snapshot previously returned by @ref Diag_GetAndResetSnapshot.
 */
void Diag_PrintSnapshot(const diag_snapshot_t* snap);

/* ----- Z-speed helpers (free-running, independent of trial) ----- */
/** @brief Reset the Z-speed estimator state. */
void TD_Speed_Reset(void);
/**
 * @brief Update Z-speed estimator on each accepted Z.
 * @param now_ms HAL_GetTick() when Z was accepted.
 */
void TD_Speed_OnZ(uint32_t now_ms);
/**
 * @brief Read instantaneous and EWMA speeds (rps/rpm).
 * @param[out] rps_inst Instantaneous rps.
 * @param[out] rps_avg  Smoothed rps.
 * @param[out] rpm_inst Instantaneous rpm.
 * @param[out] rpm_avg  Smoothed rpm.
 */
void TD_Speed_Get(float *rps_inst, float *rps_avg,
                  float *rpm_inst, float *rpm_avg);

/** @brief Add to the window-overwrite counter (FIFO full events). */
void Diag_AddWinOverwrites(uint32_t n);

/**
 * @brief Whether a timestamp lies within the current trial [start, end].
 * @param t_ms Timestamp to test (ms).
 * @return true if covered; false otherwise.
 */
bool Diag_IsTrialCovering(uint32_t t_ms);

#endif /* DIAG_TIMING_H */
