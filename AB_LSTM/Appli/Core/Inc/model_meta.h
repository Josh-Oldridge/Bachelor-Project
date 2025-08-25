// Auto-generated from training notebook — DO NOT EDIT BY HAND
#pragma once

#define MODEL_SEQ_LEN 20
#define MODEL_FEAT_DIM 10
#define MODEL_NUM_CLASSES 4
#define MODEL_HAS_THRESHOLDS 1

static const char* const MODEL_FEATURES[MODEL_FEAT_DIM] = {
  "AB_State",
  "State_Change",
  "Direction_forward",
  "Direction_backward",
  "Speed_Change_Rate",
  "Channel_A",
  "Channel_B",
  "Channel_Z",
  "Edge_RISING",
  "Steps_Since_Z",
};

static const char* const MODEL_CLASSES[MODEL_NUM_CLASSES] = {
  "bounce",
  "missing_step",
  "normal",
  "z_index",
};

static const float MODEL_FEAT_MIN[MODEL_FEAT_DIM] = {
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f,
  0.0f
};

static const float MODEL_FEAT_SCALE[MODEL_FEAT_DIM] = {
  0.33333333f,
  1.0f,
  1.0f,
  1.0f,
  0.774704f,
  1.0f,
  1.0f,
  1.0f,
  1.0f,
  0.00025f
};

// Optional operating thresholds (same order as MODEL_CLASSES)
static const float OP_THRESH[MODEL_NUM_CLASSES] = {
  0.71250892f,
  0.32637107f,
  0.34809512f,
  0.75181735f
};
