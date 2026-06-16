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
   std::string image;      // DispReel::m_szImage (image name, used for role hints)
   int reelCount = 0;      // DispReel::GetReels()  - number of digit wheels in the set
   int digitRange = 0;     // DispReel::GetRange()  - max digit per wheel (10s of EM tables: 9)
   long currentValue = 0;  // DispReel::GetCurrentValue() - 0 when only classifying (no live values)
   bool hasImage = true;   // reel has a (resolvable) strip image - imageless reels can't be drawn
};

// Every table is handled by ONE system: each reel is classified into a role from
// its geometry + name + image (no per-table special-casing), and the renderer
// shows whatever roles the table actually has - scores always, plus credit /
// ball-in-play / overflow when present. Everything else (match, tilt, game-over,
// player-up, high-score, card displays, ...) is ignored.
enum class ReelRole
{
   Ignore,
   Score,        // a player's score (multi-wheel 0-9, drawable)
   Overflow,     // 100K / rollover carry digit for a score (single wheel)
   Credit,       // credit readout
   BallInPlay,   // ball-in-play readout
   CurrentPlayer // player-up lamp (which player is at bat); state only, not drawn as a number
};

struct ReelPlan
{
   bool activate = false;             // true iff >=1 Score reel
   std::vector<ReelRole> roles;       // one per input reel (parallel to the input vector)
   std::vector<int> scoreReels;       // indices, role == Score
   std::vector<int> overflowReels;    // indices, role == Overflow
   std::vector<int> creditReels;      // indices, role == Credit
   std::vector<int> bipReels;         // indices, role == BallInPlay
   std::vector<int> playerUpReels;    // indices, role == CurrentPlayer
   int primaryScore = -1;             // active scorer among scoreReels (single-reel fallback)
};

// True when a reel is a numeric multi-digit score reel: at least two digit wheels
// (an EM score is never a single digit) on a 0-9 decimal strip (digitRange == 9),
// and backed by a strip image we can actually draw. (High-score reels also satisfy
// this; ClassifyRole excludes them by name so they are not shown as a live score.)
bool IsScoreReel(const ReelInput& r);

// Classify a single reel into its display role (see ReelRole). Name-independent
// for scores; auxiliary roles (overflow / credit / ball-in-play) are recognised
// from name + image, with obvious non-readout single reels (player-up, tilt,
// game-over, match, cards, ...) excluded first.
ReelRole ClassifyRole(const ReelInput& r);

// Classify every reel, group them by role, and pick the active scorer. activate is
// true iff the table has at least one Score reel.
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
