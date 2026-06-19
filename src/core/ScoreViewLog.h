// license:GPLv3+
#pragma once

// Master switch for the score-reel / ScoreView pipeline DIAGNOSTIC logging: reel
// detection + per-reel dumps, EM/char-display compositing, PUP deferral, and ScoreView
// layout selection. 0 = OFF (default; keeps the normal logcat clean). Set to 1 and
// rebuild to trace the scoreview pipeline when bringing up a new table. Genuine error
// reporting (load/parse failures) is NOT gated by this.
//
// Used by src/core/ReelDmd.cpp (via SVLOG/SVLOGW) and plugins/scoreview/ScoreView.cpp
// (via SVLOG_D/SVLOG_W). Override at build time with -DVPX_SCOREVIEW_DEBUG_LOG=1 if
// preferred; otherwise flip the default below.
#ifndef VPX_SCOREVIEW_DEBUG_LOG
#define VPX_SCOREVIEW_DEBUG_LOG 0
#endif
