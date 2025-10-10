#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file    mem_diag.h
 * @brief   Simple RAM usage diagnostics: MSP stack watermarking, heap stats,
 *          and section accounting with printable and programmatic access.
 */

/**
 * @brief Paint the MSP stack region with a watermark pattern.
 * @details Call **very early** at boot (before enabling interrupts) so the
 *          watermark reflects true high-water stack usage later.
 */
void MemDiag_Init(void);

/**
 * @brief Print a human-readable summary of RAM usage to stdout.
 * @details Safe to call at any time. Shows stack/heap usage, section sizes,
 *          and (if available) AXISRAM segments.
 */
void MemDiag_PrintSummary(void);

/**
 * @brief Programmatic snapshot of RAM usage.
 */
typedef struct {
  size_t stack_total;                   /**< Total MSP stack bytes. */
  size_t stack_used_now;                /**< Current MSP stack usage. */
  size_t stack_free_now;                /**< Current MSP stack free bytes. */
  size_t stack_hwm;                     /**< MSP stack high-water mark. */

  size_t heap_total;                    /**< Heap room up to stack (bytes). */
  size_t heap_used_now;                 /**< Bytes currently allocated. */
  size_t heap_free_now;                 /**< Bytes still available to stack. */

  size_t data_size;                     /**< Size of .data section in RAM. */
  size_t bss_size;                      /**< Size of .bss section in RAM. */

  size_t heap_free_plus_stack_free_now; /**< Quick “free now” estimator:
                                             heap_free_now + stack_free_now. */
} MemDiag_Stats;

/**
 * @brief Fill @p out with the current RAM usage snapshot.
 * @param[out] out Structure to receive values (must be non-NULL).
 */
void MemDiag_Get(MemDiag_Stats* out);

#ifdef __cplusplus
}
#endif
