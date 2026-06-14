// license:GPLv3+

#pragma once

#include <string>
#include <vector>

// Engine-independent classification of a table's electro-mechanical score reels.
//
// ReelDmd builds one ReelInput per live DispReel part (name + GetReels() +
// GetRange() + GetCurrentValue()) and asks ClassifyReels() what to render. The
// logic is kept free of engine types (Player, DispReel, BaseTexture) so it can be
// unit-tested in isolation (see tests/test-reel-classifier.cpp) and so the
// "which reels are the score" decision lives in one auditable place rather than
// being scattered through the renderer.
namespace reel
{

// A minimal description of one DispReel part.
struct ReelInput
{
   std::string name;       // DispReel::GetName()
   int reelCount = 0;      // DispReel::GetReels()  - number of digit wheels in the set
   int digitRange = 0;     // DispReel::GetRange()  - max digit per wheel (10s of EM tables: 9)
   long currentValue = 0;  // DispReel::GetCurrentValue() - 0 when only classifying (no live values)
   bool hasImage = true;   // reel has a (resolvable) strip image - imageless reels can't be drawn
};

enum class ReelMode
{
   None,                // no score reels -> do not activate the reel view
   Gottlieb4Player,     // curated ScoreReel1..4 / Reel100K1..4 / BIPReel / Credittxt 2x2 backglass
   GenericActiveScore   // any other EM table: render its active scorer (one numeric reel set)
};

struct ReelPlan
{
   bool activate = false;
   ReelMode mode = ReelMode::None;

   // Gottlieb 4-player backglass roles. Indices into the input vector, -1 = absent.
   int score[4] = { -1, -1, -1, -1 }; // ScoreReel1..4
   int k100[4] = { -1, -1, -1, -1 };  // Reel100K1..4
   int bip = -1;                      // BIPReel
   int credit = -1;                   // Credittxt

   // GenericActiveScore: all numeric score reels in the table (indices into the
   // input vector). The renderer composites the visible ones at their real
   // backglass positions (so a multi-player table shows every player's score),
   // and uses primaryScore as the single-reel fallback (the active scorer) when
   // none are currently visible / on-canvas.
   std::vector<int> scoreReels;
   int primaryScore = -1;
};

// True when a reel is a numeric multi-digit score reel: at least two digit wheels
// (an EM score is never a single digit) on a 0-9 decimal strip (digitRange == 9),
// and backed by a strip image we can actually draw.
//
// The reelCount/digitRange test excludes the single-digit status/auxiliary reels
// that EM and solid-state tables also model as DispReels - ball-in-play, match,
// tilt, game-over, player-up, credit (range 25), thermometer, and the 100K
// overflow wheel - all of which have reelCount == 1. It is what keeps solid-state
// tables whose DispReels are purely decorative status flags (e.g. Bally Mata Hari,
// real score on PinMAME segment displays) from activating the reel view.
//
// The hasImage test excludes imageless "data" reels - e.g. Fast Draw's off-screen
// 6-digit EMReel5, which carries a value but no resolvable strip and so cannot be
// rendered (and must not be chosen as the reel to display).
bool IsScoreReel(const ReelInput& r);

// Decide what (if anything) the reel view should render for this set of reels.
// Name-independent: the curated Gottlieb 2x2 backglass is chosen only when two or
// more ScoreReelN names are present (a genuine multi-player layout); every other
// EM table with at least one numeric score reel - including a lone ScoreReel1 -
// renders its active scorer generically.
ReelPlan ClassifyReels(const std::vector<ReelInput>& reels);

// --- faithful position-based layout (used by the generic render path) ---

// One score reel's on-screen rectangle in editor-canvas coordinates (the space
// DispReel uses, EDITOR_BG_WIDTH x EDITOR_BG_HEIGHT). Built by ReelDmd from each
// reel's position/size; `visible` is the reel's live runtime visibility.
struct ReelRect
{
   int index = -1;  // caller's handle (e.g. index into its reel list)
   float x = 0, y = 0, w = 0, h = 0;
   bool visible = false;
};

struct PlacedReel
{
   int index = -1;
   float x = 0, y = 0, w = 0, h = 0;
};

struct ReelLayout
{
   std::vector<PlacedReel> placed;   // kept reels, in input order
   float x = 0, y = 0, w = 0, h = 0; // bounding box over `placed` (canvas space)
};

// Choose which reels to draw and their bounding box for the faithful composite:
// keep the reels whose center lies within `margin` of the [0,canvasW] x
// [0,canvasH] backglass (dropping off-canvas HUD duplicates), then bound them.
// When `requireVisible` is true only currently-visible reels are kept (the
// in-game case: show the lit reel of each dim/lit pair); when false, visibility
// is ignored (the attract/all-hidden fallback, so the box is not left blank).
// Pure: no pixel work, so the multi-player layout decision is unit-tested directly.
ReelLayout PlaceReels(const std::vector<ReelRect>& rects, float canvasW, float canvasH, float margin, bool requireVisible);

// Row/column grid assignment for the placed reels, so a multi-reel table renders
// as a compact grid (e.g. a 4-player 2x2) instead of being spread across the
// backglass with big black gaps. Walking the sorted reel centers, a new row /
// column begins when the gap exceeds rowTol / colTol; reels that share a position
// (an EM dim/lit pair) land in the same cell. Pure: unit-tested directly.
struct GridPos { int row = 0; int col = 0; };
struct GridLayout
{
   std::vector<GridPos> pos; // one entry per placed reel, in input order
   int rows = 0;
   int cols = 0;
};

GridLayout AssignGrid(const std::vector<PlacedReel>& placed, float rowTol, float colTol);

} // namespace reel
