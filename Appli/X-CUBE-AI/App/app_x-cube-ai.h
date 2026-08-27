
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __APP_AI_H
#define __APP_AI_H
#ifdef __cplusplus
extern "C" {
#endif
/**
  ******************************************************************************
  * @file    app_x-cube-ai.h
  * @author  X-CUBE-AI C code generator
  * @brief   AI entry function definitions
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* Includes ------------------------------------------------------------------*/
#include "ai_platform.h"

void MX_X_CUBE_AI_Init(void);
void MX_X_CUBE_AI_Process(void);
/* USER CODE BEGIN includes */
/**
 * @brief Get POLICY decision totals since boot.
 * @param[out] normal  Total decisions classified as NORMAL by the policy layer.
 * @param[out] missing Total decisions classified as MISSING_STEP by the policy layer.
 * @param[out] zindex  Total decisions classified as Z_INDEX by the policy layer.
 */
void Model_GetTotals(uint32_t *normal, uint32_t *missing, uint32_t *zindex);

/**
 * @brief Get RAW (pre-policy) argmax totals since boot.
 * @param[out] normal  Total RAW argmax counts for NORMAL.
 * @param[out] missing Total RAW argmax counts for MISSING_STEP.
 * @param[out] zindex  Total RAW argmax counts for Z_INDEX.
 * @note These counters reflect the network’s direct argmax before any
 *       deployment policy is applied.
 */
void Model_GetTotalsRaw(uint32_t *normal, uint32_t *missing, uint32_t *zindex);
/* USER CODE END includes */
#ifdef __cplusplus
}
#endif
#endif /*__STMicroelectronics_X-CUBE-AI_10_2_0_H */
