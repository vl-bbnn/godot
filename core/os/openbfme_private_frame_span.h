#pragma once
#include "core/os/os.h"
#include "core/string/print_string.h"
// Private attribution only. No scheduling, renderer, cache or gameplay changes.
class OpenBFMEFrameSpan {
 const char *label;
 uint64_t begin = 0;
public:
 OpenBFMEFrameSpan(const char *p_label) : label(p_label) {
  static const bool enabled = OS::get_singleton()->get_environment("OPENBFME_MAINLOOP_TRACE") == "1";
  if (enabled) begin = OS::get_singleton()->get_ticks_usec();
 }
 ~OpenBFMEFrameSpan() {
  if (!begin) return;
  uint64_t end = OS::get_singleton()->get_ticks_usec();
  if (end - begin > 20000) print_line("FRAME_SPAN phase=", label, " begin_us=", begin, " end_us=", end, " duration_us=", end - begin);
 }
};
