#ifndef SYNTHETIC_H
#define SYNTHETIC_H

#include <stdint.h>

/* Global toggle: 1 = use synthetic window, 0 = live encoder */
#ifndef INJECT_SYNTH
#define INJECT_SYNTH 1
#endif

/* What kind of window to synthesize */
typedef enum {
  SYNTH_NORMAL = 0,
  SYNTH_BOUNCE = 1,
  SYNTH_MISSING_STEP = 2,
  SYNTH_Z_INDEX = 3,
} EncSynth_Mode;

/* Core helpers (unchanged) */
const float* EncSynth_GetFlat(void);
void EncSynth_DebugPrint(void);

/* Normal: perfect Gray sequence
   - forward: 1=fwd 0=bwd
   - start_state: 0..3  (Gray)
   - steps0: initial Steps_Since_Z
   - rising_only: 1 => only rising edges become rows; 0 => both rise & fall become rows
*/
void EncSynth_MakeNormalWindow(int forward, int start_state, int steps0, int rising_only);

/* Bounce: inject repeated edges on a single channel in the middle of the window
   - forward, start_state, steps0 as above
   - which_channel: 1=A, 2=B
   - t_beg: where to start bounce cluster (output row index, 0..SEQ_LEN-1)
   - n_bounces: how many *extra* edges to inject (2..5 is reasonable)
   - rising_only: 1 => produce repeated rising edges on that channel; 0 => toggle rise+fall quickly
*/
void EncSynth_MakeBounceWindow(int forward, int start_state, int steps0,
                               int which_channel, int t_beg, int n_bounces,
                               int rising_only);

/* Missing-step: “skip” the other channel once so the same channel fires twice in a row
   - which_channel: 1=A repeats twice, 2=B repeats twice
   - t_at: output row index at which the anomaly begins (0..SEQ_LEN-2)
   - rising_only: honored same as above
*/
void EncSynth_MakeMissingStepWindow(int forward, int start_state, int steps0,
                                    int which_channel, int t_at, int rising_only);

/* Z-index: inject a Z pulse once and reset Steps_Since_Z from that row onward
   - t_z: output row index for the Z pulse
   - rising_only: honored same as above
*/
void EncSynth_MakeZIndexWindow(int forward, int start_state, int steps0,
                               int t_z, int rising_only);

#endif /* SYNTHETIC_H */
