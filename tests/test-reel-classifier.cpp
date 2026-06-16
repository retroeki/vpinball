// license:GPLv3+
//
// Standalone unit tests for src/core/ReelClassifier (no engine dependencies).
//
// These guard the EM score-reel detection that drives the in-app ScoreView reel
// view (ReelDmd). The fixtures are real reel manifests extracted from shipping
// tables (see issue_investigations/scorereels/gen_fixtures.py): the score must be
// shown for genuine EM reel tables regardless of how the author named the reels,
// and must NOT activate for solid-state tables whose DispReels are decorative
// single-digit status flags / segment-character cells (their real score lives on
// a PinMAME segment/DMD display the ScoreView already renders).
//
// Build & run from the repo root (C:\vpinball-master):
//   g++ -std=c++17 -I src tests/test-reel-classifier.cpp src/core/ReelClassifier.cpp -o reeltest && ./reeltest
//
// It links only ReelClassifier.cpp; it does not touch the engine, so it runs
// anywhere a C++17 compiler is available.

#include "core/ReelClassifier.h"

#include <cstdio>
#include <cctype>
#include <string>
#include <vector>

using reel::ReelInput;
using reel::ReelRole;
using reel::ReelPlan;
using reel::ClassifyReels;
using reel::ClassifyRole;
using reel::IsScoreReel;
using reel::ReelRect;
using reel::ReelLayout;
using reel::PlaceReels;
using reel::PlacedReel;
using reel::GridLayout;
using reel::AssignGrid;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                      \
   do {                                                                       \
      if (cond) { ++g_pass; }                                                 \
      else { ++g_fail; std::printf("FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } \
   } while (0)

static ReelInput R(const char* name, int reelCount, int digitRange, long value = 0, bool hasImage = true,
                   const char* image = "")
{
   ReelInput r;
   r.name = name;
   r.image = image;
   r.reelCount = reelCount;
   r.digitRange = digitRange;
   r.currentValue = value;
   r.hasImage = hasImage;
   return r;
}

static bool IEq(const std::string& a, const char* b)
{
   std::string lb(b);
   if (a.size() != lb.size())
      return false;
   for (size_t i = 0; i < a.size(); ++i)
      if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)lb[i]))
         return false;
   return true;
}

// Name of the reel a plan index points at (or "<none>" / "<oob>").
static std::string NameAt(const std::vector<ReelInput>& reels, int idx)
{
   if (idx < 0) return "<none>";
   if (idx >= (int)reels.size()) return "<oob>";
   return reels[idx].name;
}

// ----------------------------------------------------------------------------
// Real table fixtures (generated; see gen_fixtures.py).
// ----------------------------------------------------------------------------

// Royal Flush (Gottlieb 1976) BorgDog - the curated 4-player Gottlieb backglass.
static const std::vector<ReelInput> RoyalFlush = {
   R("ScoreReel3", 5, 9), R("Credittxt", 1, 25), R("Reel100K1", 1, 3),
   R("Reel100K2", 1, 3),  R("BIPReel", 1, 5),    R("ScoreReel4", 5, 9),
   R("Reel100K4", 1, 3),  R("Reel100K3", 1, 3),  R("ScoreReel2", 5, 9),
   R("ScoreReel1", 5, 9),
};

// Card Whiz (Gottlieb 1976) BorgDog - same 4-player convention as Royal Flush.
static const std::vector<ReelInput> CardWhiz = {
   R("ScoreReel2", 5, 9), R("ScoreReel1", 5, 9), R("Credittxt", 1, 25),
   R("Reel100K1", 1, 3),  R("Reel100K2", 1, 3),  R("BIPReel", 1, 5),
   R("ScoreReel4", 5, 9), R("Reel100K4", 1, 3),  R("ScoreReel3", 5, 9),
   R("Reel100K3", 1, 3),
};

// Centigrade 37 (Gottlieb 1977) - single multi-digit "ScoreReel" (no index).
static const std::vector<ReelInput> Centigrade37 = {
   R("ScoreReel", 5, 9), R("CreditReel", 1, 25), R("ThermoReel", 1, 24),
   R("BIPReel", 1, 5),   R("TiltReel", 1, 1),    R("GameOverReel", 1, 1),
};

// Fast Draw (Gottlieb 1975) - the common "EMReelN" remake convention.
// NOTE: CreditsReel is (1,9): a single-wheel 0-9 reel that must NOT be treated as
// a score reel - this is exactly why IsScoreReel requires reelCount >= 2.
static const std::vector<ReelInput> FastDraw = {
   // EMReel5 is the off-screen 6-digit data reel whose strip image does not resolve
   // at runtime (hasImage=false) - it must be excluded as a score reel.
   R("EMReel5", 6, 9, 0, /*hasImage*/ false), R("EMReel1", 5, 9), R("EMReel2", 5, 9), R("EMReel3", 5, 9),
   R("EMReel4", 5, 9), R("EMReel6", 5, 9), R("EMReel7", 5, 9), R("EMReel8", 5, 9),
   R("EMReel9", 5, 9), R("PlayerUp4", 1, 1), R("PlayerUp2", 1, 1), R("PlayerUp1", 1, 1),
   R("PlayerUp3", 1, 1), R("TiltReel", 1, 1), R("CanPlayReel", 1, 4), R("CreditsReel", 1, 9),
   R("GameOverReel", 1, 1), R("BallInPlayReel", 1, 5), R("MatchReel", 1, 10),
};

// Volley (Gottlieb 1976) - "EMReelN" convention, two score reels.
static const std::vector<ReelInput> Volley = {
   R("EMReel1", 5, 9), R("EMReel2", 6, 9),
   R("EMReel6", 1, 15, 0, true, "ballycreditwheel"), // credit by image
   R("BallInPlayReel", 1, 5),
   R("MatchReel", 1, 10), R("TiltReel", 1, 1), R("GameOverReel", 1, 1),
};

// Nags (Williams 1960) - EM table; one 3-wheel ScoreReel1 amid single-wheel flags.
static const std::vector<ReelInput> Nags = {
   R("dtReel", 1, 1), R("ScoreReel1", 3, 9), R("CreditReel", 1, 18), R("door1to3Reel", 1, 3),
   R("door4to6Reel", 1, 3), R("horse1to3Reel", 1, 3), R("horse4to6Reel", 1, 3),
   R("redHatReel", 1, 10), R("yellowHatReel", 1, 2),
};

// --- solid-state tables: DispReels are decorative; must NOT activate ---

// Mata Hari (Bally 1978) - the regression case. All reels are single-digit status
// flags; real score is on PinMAME segment displays. (from a device logcat)
static const std::vector<ReelInput> MataHari_SS = {
   R("MatchReel", 1, 10), R("BIPReel", 1, 5), R("GameOverReel", 1, 1),
   R("HighScoreReel", 1, 1), R("ShootAgainReel", 1, 1), R("TiltReel", 1, 1),
   R("CreditsReel", 1, 25),
};

// Andromeda (Game Plan 1985) - single-wheel status flags only.
static const std::vector<ReelInput> Andromeda_SS = {
   R("HighscoreR", 1, 1), R("TiltReel", 1, 1), R("GameOverR", 1, 1),
   R("ShootAgainR", 1, 1), R("BallinplayR", 1, 1),
};

// Bobby Orr Power Play (Bally 1978) - 15 single-wheel status flags.
static const std::vector<ReelInput> BobbyOrr_SS = {
   R("OneCanPlayREEL", 1, 1), R("TwoCanPlayREEL", 1, 1), R("ThreeCanPlayREEL", 1, 1),
   R("FourCanPlayREEL", 1, 1), R("NumPlayersReel", 1, 1), R("ShootAgainReel", 1, 1),
   R("P4UpREEL", 1, 1), R("P2UpREEL", 1, 1), R("MatchReel", 1, 1), R("BIPReel", 1, 1),
   R("TiltReel", 1, 1), R("HighScoreReel", 1, 1), R("P1UpREEL", 1, 1), R("P3UpREEL", 1, 1),
   R("GameOverReel", 1, 1),
};

// Ali (Stern 1980) JPSalas - per-character alphanumeric segment cells (single wheel).
static const std::vector<ReelInput> Ali_SS = {
   R("a0", 1, 10), R("a1", 1, 10), R("a2", 1, 10), R("a3", 1, 10), R("a4", 1, 10), R("a5", 1, 10),
   R("c0", 1, 10), R("c1", 1, 10), R("c2", 1, 10), R("c3", 1, 10), R("c4", 1, 10), R("c5", 1, 10),
   R("e0", 1, 10), R("e1", 1, 10), R("e2", 1, 10), R("e3", 1, 10),
   R("b0", 1, 10), R("b1", 1, 10), R("b2", 1, 10), R("b3", 1, 10), R("b4", 1, 10), R("b5", 1, 10),
   R("d0", 1, 10), R("d1", 1, 10), R("d2", 1, 10), R("d3", 1, 10), R("d4", 1, 10), R("d5", 1, 10),
};

// ----------------------------------------------------------------------------

static void TestIsScoreReel()
{
   CHECK(IsScoreReel(R("x", 5, 9)),  "5-wheel decimal is a score reel");
   CHECK(IsScoreReel(R("x", 2, 9)),  "2-wheel decimal is a score reel");
   CHECK(!IsScoreReel(R("x", 1, 9)), "1-wheel decimal (credit) is NOT a score reel");
   CHECK(!IsScoreReel(R("x", 1, 1)), "1-wheel status flag is NOT a score reel");
   CHECK(!IsScoreReel(R("x", 5, 10)),"multi-wheel non-decimal (dr!=9) is NOT a score reel");
   CHECK(!IsScoreReel(R("x", 1, 10)),"1-wheel segment cell is NOT a score reel");
   CHECK(!IsScoreReel(R("x", 6, 9, 0, /*hasImage*/ false)), "imageless reel is NOT a score reel");
}

// Activating table: expected score-reel count, active scorer, and role group sizes.
static void TestActivates(const char* label, const std::vector<ReelInput>& reels,
                          const char* expectedPrimaryName, int expectedScore,
                          int expectedOverflow, int expectedCredit, int expectedBip)
{
   const ReelPlan p = ClassifyReels(reels);
   CHECK(p.activate, label);
   CHECK(p.primaryScore >= 0 && IEq(NameAt(reels, p.primaryScore), expectedPrimaryName), label);
   CHECK((int)p.scoreReels.size() == expectedScore, label);
   CHECK((int)p.overflowReels.size() == expectedOverflow, label);
   CHECK((int)p.creditReels.size() == expectedCredit, label);
   CHECK((int)p.bipReels.size() == expectedBip, label);
   // Every reported score reel must actually be a numeric score reel.
   bool allScore = true;
   for (int idx : p.scoreReels)
      if (idx < 0 || idx >= (int)reels.size() || !IsScoreReel(reels[idx]))
         allScore = false;
   CHECK(allScore, label);
}

static void TestInactive(const char* label, const std::vector<ReelInput>& reels)
{
   const ReelPlan p = ClassifyReels(reels);
   CHECK(!p.activate, label);
   CHECK(p.scoreReels.empty(), label);
   CHECK(p.primaryScore < 0, label);
}

static void TestClassifyRole()
{
   CHECK(ClassifyRole(R("ScoreReel1", 5, 9)) == ReelRole::Score, "role: multi-wheel 0-9 -> Score");
   CHECK(ClassifyRole(R("EMReel1", 5, 9)) == ReelRole::Score, "role: EMReel -> Score");
   CHECK(ClassifyRole(R("HighScoreReel", 6, 9)) == ReelRole::Ignore, "role: high-score reel -> Ignore");
   CHECK(ClassifyRole(R("EMReel5", 6, 9, 0, /*hasImage*/ false)) == ReelRole::Ignore, "role: imageless -> Ignore");
   CHECK(ClassifyRole(R("Reel100K1", 1, 3)) == ReelRole::Overflow, "role: Reel100K -> Overflow");
   CHECK(ClassifyRole(R("RolloverReel2", 1, 1)) == ReelRole::Overflow, "role: Rollover -> Overflow");
   CHECK(ClassifyRole(R("Player1100K", 1, 1)) == ReelRole::Overflow, "role: PlayerN100K -> Overflow");
   CHECK(ClassifyRole(R("Credittxt", 1, 25)) == ReelRole::Credit, "role: credit by name -> Credit");
   CHECK(ClassifyRole(R("EMReel10", 1, 15, 0, true, "ballycreditwheel")) == ReelRole::Credit, "role: credit by image -> Credit");
   CHECK(ClassifyRole(R("BIPReel", 1, 5)) == ReelRole::BallInPlay, "role: BIPReel -> BallInPlay");
   CHECK(ClassifyRole(R("BallInPlayReel", 1, 5)) == ReelRole::BallInPlay, "role: BallInPlay -> BallInPlay");
   // Player-up is its own role (state only), recognised before the ball-in-play check
   // even when it reuses a ball-in-play image - so it is NOT misread as ball-in-play.
   CHECK(ClassifyRole(R("PlayerUp4", 1, 1, 0, true, "hud-ballinplay")) == ReelRole::CurrentPlayer, "role: PlayerUp -> CurrentPlayer");
   CHECK(ClassifyRole(R("TiltReel", 1, 1)) == ReelRole::Ignore, "role: tilt -> Ignore");
   CHECK(ClassifyRole(R("GameOverReel", 1, 1)) == ReelRole::Ignore, "role: game-over -> Ignore");
   CHECK(ClassifyRole(R("MatchReel", 1, 10)) == ReelRole::Ignore, "role: match -> Ignore");
   CHECK(ClassifyRole(R("a0", 1, 10)) == ReelRole::Ignore, "role: segment cell -> Ignore");
}

static void TestActiveScorerSelection()
{
   // Three numeric reels: pick the one showing the highest current value.
   std::vector<ReelInput> v = {
      R("P1", 5, 9, 1200), R("P2", 5, 9, 9800), R("P3", 5, 9, 50),
   };
   ReelPlan p = ClassifyReels(v);
   CHECK(p.activate && IEq(NameAt(v, p.primaryScore), "P2"),
         "active scorer = highest current value");

   // Tie on value -> the reel with more wheels (the main score) wins.
   std::vector<ReelInput> v2 = {
      R("small", 4, 9, 0), R("big", 6, 9, 0), R("mid", 5, 9, 0),
   };
   p = ClassifyReels(v2);
   CHECK(IEq(NameAt(v2, p.primaryScore), "big"), "tie on value -> most wheels wins");

   // Tie on value and wheels -> stable: the first (lowest index) wins.
   std::vector<ReelInput> v3 = {
      R("first", 5, 9, 7), R("second", 5, 9, 7),
   };
   p = ClassifyReels(v3);
   CHECK(IEq(NameAt(v3, p.primaryScore), "first"), "tie on value+wheels -> lowest index");

   // Fast Draw with player 1 leading: EMReel1 should win over the 6-wheel EMReel5.
   std::vector<ReelInput> fd = FastDraw;
   for (auto& r : fd) if (IEq(r.name, "EMReel1")) r.currentValue = 4500;
   p = ClassifyReels(fd);
   CHECK(IEq(NameAt(fd, p.primaryScore), "EMReel1"),
         "Fast Draw: leading player's reel wins even with fewer wheels");
}

static ReelRect RR(int idx, float x, float y, float w, float h, bool vis)
{
   ReelRect r; r.index = idx; r.x = x; r.y = y; r.w = w; r.h = h; r.visible = vis; return r;
}

// Count distinct (rounded-to-10px) column/row positions among placed reels.
static int DistinctCols(const ReelLayout& l)
{
   std::vector<int> xs;
   for (const auto& p : l.placed) { int c = (int)(p.x / 10); bool seen=false; for (int v:xs) if (v==c) seen=true; if(!seen) xs.push_back(c); }
   return (int)xs.size();
}
static int DistinctRows(const ReelLayout& l)
{
   std::vector<int> ys;
   for (const auto& p : l.placed) { int c = (int)(p.y / 10); bool seen=false; for (int v:ys) if (v==c) seen=true; if(!seen) ys.push_back(c); }
   return (int)ys.size();
}

static void TestFaithfulLayout()
{
   const float CW = 1000.f, CH = 750.f, M = 150.f;

   // Royal Flush: 4 score reels form a real 2x2 (TL/TR/BL/BR), all visible.
   std::vector<ReelRect> rf = {
      RR(0, -3, 172, 237, 84, true),  // ScoreReel1 TL
      RR(1, 774, 172, 231, 82, true), // ScoreReel2 TR
      RR(2, -3, 258, 237, 84, true),  // ScoreReel3 BL
      RR(3, 773, 258, 231, 82, true), // ScoreReel4 BR
   };
   ReelLayout l = PlaceReels(rf, CW, CH, M, /*requireVisible*/ true);
   CHECK(l.placed.size() == 4, "RoyalFlush: all 4 player reels placed");
   CHECK(DistinctCols(l) == 2 && DistinctRows(l) == 2, "RoyalFlush: 2x2 arrangement");
   CHECK(l.w > 800.f && l.h > 120.f, "RoyalFlush: bbox spans both columns and rows");

   // Fast Draw: 4 player positions, each an overlapping dim/lit pair (only one
   // visible per position), plus an off-canvas HUD reel (EMReel5 at x=1106).
   std::vector<ReelRect> fd = {
      RR(0, 818, 40, 88, 28, true),   // P1 lit (current player)
      RR(1, 818, 40, 88, 28, false),  // P1 dim (hidden)
      RR(2, 913, 40, 88, 28, true),   // P2 dim
      RR(3, 913, 40, 88, 28, false),  // P2 lit (hidden)
      RR(4, 817, 90, 88, 28, true),   // P3 dim
      RR(5, 817, 90, 88, 28, false),  // P3 lit (hidden)
      RR(6, 913, 90, 88, 28, true),   // P4 dim
      RR(7, 913, 90, 88, 28, false),  // P4 lit (hidden)
      RR(8, 1106, 216, 118, 23, true),// EMReel5 HUD, off-canvas -> excluded
   };
   l = PlaceReels(fd, CW, CH, M, /*requireVisible*/ true);
   CHECK(l.placed.size() == 4, "FastDraw: one reel per player position (dim/lit resolved)");
   CHECK(DistinctCols(l) == 2 && DistinctRows(l) == 2, "FastDraw: 4 players in a 2x2");
   bool fdIdx = true;
   for (const auto& p : l.placed) if (!(p.index == 0 || p.index == 2 || p.index == 4 || p.index == 6)) fdIdx = false;
   CHECK(fdIdx, "FastDraw: only the visible reels were placed");

   // Volley: one on-canvas score reel + one off-canvas HUD reel.
   std::vector<ReelRect> vol = {
      RR(0, 872, 30, 120, 35, true),   // EMReel1 (on canvas)
      RR(1, 1186, 406, 118, 23, true), // EMReel2 (off canvas) -> excluded
   };
   l = PlaceReels(vol, CW, CH, M, /*requireVisible*/ true);
   CHECK(l.placed.size() == 1 && l.placed[0].index == 0, "Volley: off-canvas HUD reel excluded");

   // Attract: all on-canvas reels hidden. requireVisible=true -> empty (caller then
   // falls back); requireVisible=false -> still placed, so the box is not blank.
   std::vector<ReelRect> hidden = {
      RR(0, 818, 40, 88, 28, false),  // on-canvas, hidden
      RR(1, 913, 40, 88, 28, false),  // on-canvas, hidden
      RR(2, 1106, 216, 118, 23, true),// off-canvas (excluded regardless)
   };
   l = PlaceReels(hidden, CW, CH, M, /*requireVisible*/ true);
   CHECK(l.placed.empty(), "attract: requireVisible=true with all hidden -> empty (triggers fallback)");
   l = PlaceReels(hidden, CW, CH, M, /*requireVisible*/ false);
   CHECK(l.placed.size() == 2, "attract fallback: requireVisible=false places the on-canvas reels");
   CHECK(DistinctCols(l) == 2, "attract fallback: both on-canvas positions placed");
}

static void TestAssignGrid()
{
   // Fast Draw: 4 player positions -> 2x2.
   std::vector<PlacedReel> fd = {
      { 0, 818, 40, 88, 28 }, { 1, 913, 40, 88, 28 },
      { 2, 817, 90, 88, 28 }, { 3, 913, 90, 88, 28 },
   };
   GridLayout g = AssignGrid(fd, 28 * 0.6f, 88 * 0.6f);
   CHECK(g.rows == 2 && g.cols == 2, "AssignGrid: Fast Draw -> 2x2");
   CHECK(g.pos[0].row == 0 && g.pos[0].col == 0, "AssignGrid: reel0 top-left");
   CHECK(g.pos[3].row == 1 && g.pos[3].col == 1, "AssignGrid: reel3 bottom-right");

   // Single reel -> 1x1.
   std::vector<PlacedReel> one = { { 0, 872, 30, 120, 35 } };
   g = AssignGrid(one, 35 * 0.6f, 120 * 0.6f);
   CHECK(g.rows == 1 && g.cols == 1, "AssignGrid: single reel -> 1x1");

   // Overlapping dim/lit pair at the same position -> same cell.
   std::vector<PlacedReel> pair = { { 0, 818, 40, 88, 28 }, { 1, 818, 40, 88, 28 } };
   g = AssignGrid(pair, 28 * 0.6f, 88 * 0.6f);
   CHECK(g.rows == 1 && g.cols == 1, "AssignGrid: dim/lit pair -> single cell (count)");
   CHECK(g.pos[0].row == g.pos[1].row && g.pos[0].col == g.pos[1].col, "AssignGrid: dim/lit pair share a cell");
}

int main()
{
   TestIsScoreReel();
   TestClassifyRole();

   // One unified role-based system for every table. Counts: score, overflow, credit, ball-in-play.
   // Royal Flush / Card Whiz now go through the SAME path (no curated names):
   //   4 ScoreReel + 4 Reel100K(overflow) + Credittxt(credit) + BIPReel(ball).
   // primary = active scorer; all values 0 at start so it ties to the lowest-index
   // score reel in the fixture (RoyalFlush's first score reel is ScoreReel3).
   TestActivates("RoyalFlush", RoyalFlush, "ScoreReel3", 4, 4, 1, 1);
   TestActivates("CardWhiz", CardWhiz, "ScoreReel2", 4, 4, 1, 1);

   // The originally-broken EM tables + the broad EM family.
   TestActivates("Centigrade37", Centigrade37, "ScoreReel", 1, 0, 1, 1);
   // EMReel5 (imageless) excluded; 8 imaged player reels remain.
   TestActivates("FastDraw", FastDraw, "EMReel1", 8, 0, 1, 1);
   TestActivates("Volley", Volley, "EMReel2", 2, 0, 1, 1);       // EMReel6 = credit (BallyCreditWheel image)
   TestActivates("Nags (EM, 1 score reel among flags)", Nags, "ScoreReel1", 1, 0, 1, 0);

   // Solid-state tables: must NOT activate (real score is on PinMAME displays).
   TestInactive("MataHari (regression)", MataHari_SS);
   TestInactive("Andromeda", Andromeda_SS);
   TestInactive("BobbyOrr", BobbyOrr_SS);
   TestInactive("Ali (segment cells)", Ali_SS);
   TestInactive("empty", {});

   TestActiveScorerSelection();
   TestFaithfulLayout();
   TestAssignGrid();

   std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
