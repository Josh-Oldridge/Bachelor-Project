// Auto-generated from training notebook — DO NOT EDIT BY HAND
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @file model_meta.h
 *  @brief Model I/O sizes, class/feature names, and policy thresholds.
 */

#define MODEL_SEQ_LEN 20            /**< Rows (time steps) per window. */
#define MODEL_FEAT_DIM 10           /**< Features per row. */
#define MODEL_NUM_CLASSES 3         /**< Number of output classes. */
#define MODEL_HAS_THRESHOLDS 1      /**< 1 if OP_THRESH is provided. */

/** @brief Human-friendly feature names (index-stable). */
static const char* const MODEL_FEATURES[MODEL_FEAT_DIM] = {
  "AB_State",
  "SRC_A",
  "SRC_B",
  "dir_fwd",
  "dir_bwd",
  "SCR",
  "Z",
  "SSZ",
  "same_src",
  "adjacent",
};

/** @brief Class names in the order of the network output. */
static const char* const MODEL_CLASSES[MODEL_NUM_CLASSES] = {
  "missing_step",
  "normal",
  "z_index",
};

/** @brief Feature-wise min (for normalization). */
static const float MODEL_FEAT_MIN[MODEL_FEAT_DIM] = {
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  1.0f,
  0.0f,
  0.0f
};

/** @brief Feature-wise scale (y = (x - min) * scale; clamped to [0,1]). */
static const float MODEL_FEAT_SCALE[MODEL_FEAT_DIM] = {
  0.33333334f,
  1.0f,
  1.0f,
  1.0f,
  1.0f,
  0.90193528f,
  1.0f,
  0.000481f,
  1.0f,
  1.0f
};

/** @brief Optional operating thresholds (same order as MODEL_CLASSES). */
static const float OP_THRESH[MODEL_NUM_CLASSES] = {
  0.1f,
  0.12945783f,
  0.1f
};

/* -------- Policy parameters (mirror Python eval) -------- */
#define OP_Z_HI 0.950000f
#define OP_Z_LO 0.760000f
#define OP_MS_TAU 0.500000f
#define OP_MS_MARGIN 0.050000f
#define OP_SSZ_MIN_RAW 2000.0f
#define OP_SSZ_MIN_NORM 0.960000f
#define OP_USE_HASZ_GATE 1
#define OP_Z_REQUIRE_HEALTHY   1      /* 1 = require SSZ >= floor at Z row */
#define OP_Z_STRICT_HI         0.995f /* absolute pZ that can bypass SSZ gate */
#define OP_Z_MARGIN            0.05f  /* pZ must beat pN by this margin */
#define OP_Z_CONSEC            1
#define MODEL_FEAT_IDX_SSZ 7          /**< Feature index for SSZ. */
#define MODEL_FEAT_IDX_Z 6            /**< Feature index for Z. */

#ifdef __cplusplus
}
#endif
