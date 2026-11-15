// reverb_unit.h -- one reverb instance that can run any of the algorithms (plate, hall, shimmer, room, spring) and switch between
// them at run time. Instances are zero-initialised statics; select() before use. Not thread safe by itself: the
// audio thread owns the unit (the control thread only changes atomics that the audio thread reads).
#ifndef NAM_REVERB_UNIT_H
#define NAM_REVERB_UNIT_H

#include "dattorro_reverb.h"
#include "fdn_reverb.h"
#include "room_spring_reverb.h"

namespace reverb
{
enum Algo : int
{
  kPlate = 0,   // Dattorro plate, dense and smooth (dattorro_reverb.h)
  kHall = 1,    // 8-line FDN hall, long and wide (fdn_reverb.h)
  kShimmer = 2, // the hall with an octave-up pitch shifter in its feedback (fdn_reverb.h)
  kRoom = 3,    // small FDN room with early reflections (room_spring_reverb.h)
  kSpring = 4,  // two dispersive spring tanks (room_spring_reverb.h)
  kAlgoCount = 5
};

inline const char* algo_name(int a)
{
  static const char* const n[kAlgoCount] = {"plate", "hall", "shimmer", "room", "spring"};
  return a >= 0 && a < kAlgoCount ? n[a] : "?";
}

struct Unit
{
  dattorro::Reverb plate;
  fdn::Shimmer fdn; // used for both Hall (fdn.hall) and Shimmer
  room::Room room;
  spring::Spring spring;
  int algo; // current algorithm, -1 = not initialised

  // Switches to `a` (re-initialising it, which clears its tail). No-op when already selected.
  bool ready_for(int a) const
  {
    return a == kPlate ? plate.ready : a == kRoom ? room.ready : a == kSpring ? spring.ready : fdn.hall.ready;
  }

  void select(int a, double sample_rate)
  {
    if (a == algo && ready_for(a))
      return;
    algo = a;
    if (a == kPlate)
      plate.init(sample_rate);
    else if (a == kRoom)
      room.init(sample_rate);
    else if (a == kSpring)
      spring.init(sample_rate);
    else
      fdn.init(sample_rate);
  }

  // drop the tail (bypass); the next process() snaps its controls again
  void clear()
  {
    if (algo == kPlate)
      plate.clear();
    else if (algo == kRoom)
      room.clear();
    else if (algo == kSpring)
      spring.clear();
    else if (algo >= 0)
      fdn.clear();
  }

  void process(float* const* in, float* const* out, uint32_t channels, int32_t frames, float mix, float decay,
               float damp)
  {
    switch (algo)
    {
    case kPlate:
      plate.process(in, out, channels, frames, mix, decay, damp);
      break;
    case kHall:
      fdn.hall.process(in, out, channels, frames, mix, decay, damp);
      break;
    case kShimmer:
      fdn.process(in, out, channels, frames, mix, decay, damp);
      break;
    case kRoom:
      room.process(in, out, channels, frames, mix, decay, damp);
      break;
    case kSpring:
      spring.process(in, out, channels, frames, mix, decay, damp);
      break;
    default:
      break;
    }
  }
};
} // namespace reverb

#endif
