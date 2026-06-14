// license:GPLv3+

// Pure, engine-independent reel classification. Intentionally includes no engine
// headers (no stdafx) so it also compiles standalone for tests/test-reel-classifier.cpp.

#include "core/ReelClassifier.h"

#include <algorithm>
#include <cctype>

namespace reel
{

static std::string ToLower(const std::string& s)
{
   std::string r(s);
   std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)std::tolower(c); });
   return r;
}

bool IsScoreReel(const ReelInput& r)
{
   // Two-or-more decimal (0-9) digit wheels, backed by a drawable strip image.
   // See header for the full rationale; this rule admits real EM score reels
   // regardless of how the table author named them, while excluding single-digit
   // status/credit flags and imageless data reels.
   return r.reelCount >= 2 && r.digitRange == 9 && r.hasImage;
}

ReelPlan ClassifyReels(const std::vector<ReelInput>& reels)
{
   ReelPlan plan;

   const int n = (int)reels.size();

   // Map the exact (case-insensitive) Gottlieb 4-player names to roles.
   int gScore[4] = { -1, -1, -1, -1 };
   int gK100[4] = { -1, -1, -1, -1 };
   int gBip = -1, gCredit = -1;
   for (int i = 0; i < n; ++i)
   {
      const std::string nm = ToLower(reels[i].name);
      if (nm == "scorereel1")      gScore[0] = i;
      else if (nm == "scorereel2") gScore[1] = i;
      else if (nm == "scorereel3") gScore[2] = i;
      else if (nm == "scorereel4") gScore[3] = i;
      else if (nm == "reel100k1")  gK100[0] = i;
      else if (nm == "reel100k2")  gK100[1] = i;
      else if (nm == "reel100k3")  gK100[2] = i;
      else if (nm == "reel100k4")  gK100[3] = i;
      else if (nm == "bipreel")    gBip = i;
      else if (nm == "credittxt")  gCredit = i;
   }

   // Collect numeric score reels (the name-independent signal).
   std::vector<int> scoreReels;
   scoreReels.reserve(n);
   for (int i = 0; i < n; ++i)
      if (IsScoreReel(reels[i]))
         scoreReels.push_back(i);

   // Gottlieb path: the curated 2x2 backglass is for genuine multi-player layouts.
   // Require at least two ScoreReelN present (a lone ScoreReel1 is a single score,
   // better shown by the generic active-scorer path) and at least one of them an
   // actual numeric score reel (guards a decorative part that carries the name).
   int gottliebScoreCount = 0;
   bool gottliebHasNumeric = false;
   for (int p = 0; p < 4; ++p)
      if (gScore[p] >= 0)
      {
         ++gottliebScoreCount;
         if (IsScoreReel(reels[gScore[p]]))
            gottliebHasNumeric = true;
      }
   const bool gottliebUsable = (gottliebScoreCount >= 2) && gottliebHasNumeric;

   if (gottliebUsable)
   {
      plan.activate = true;
      plan.mode = ReelMode::Gottlieb4Player;
      for (int p = 0; p < 4; ++p) { plan.score[p] = gScore[p]; plan.k100[p] = gK100[p]; }
      plan.bip = gBip;
      plan.credit = gCredit;
      return plan;
   }

   if (!scoreReels.empty())
   {
      plan.activate = true;
      plan.mode = ReelMode::GenericActiveScore;
      plan.scoreReels = scoreReels; // every numeric score reel (renderer places the visible ones)
      // Active scorer: the reel showing the highest current value (the player in
      // the lead), tie-broken by the most digit wheels (the main score, not a
      // narrower sub-display), then by lowest index for stability. At game start
      // all values are 0 so the most-wheels reel is chosen until scoring begins.
      int best = scoreReels[0];
      for (int idx : scoreReels)
      {
         const ReelInput& a = reels[idx];
         const ReelInput& b = reels[best];
         if (a.currentValue != b.currentValue) { if (a.currentValue > b.currentValue) best = idx; }
         else if (a.reelCount != b.reelCount)   { if (a.reelCount > b.reelCount)       best = idx; }
         // equal value and reel count -> keep the lower index (we iterate ascending)
      }
      plan.primaryScore = best;
      return plan;
   }

   plan.activate = false;
   plan.mode = ReelMode::None;
   return plan;
}

ReelLayout PlaceReels(const std::vector<ReelRect>& rects, float canvasW, float canvasH, float margin, bool requireVisible)
{
   ReelLayout layout;
   float minX = 0, minY = 0, maxX = 0, maxY = 0;
   bool any = false;
   for (const ReelRect& r : rects)
   {
      if ((requireVisible && !r.visible) || r.w <= 0.0f || r.h <= 0.0f)
         continue;
      const float cx = r.x + 0.5f * r.w;
      const float cy = r.y + 0.5f * r.h;
      if (cx < -margin || cx > canvasW + margin || cy < -margin || cy > canvasH + margin)
         continue; // off-canvas (HUD duplicate) - not part of the backglass head
      layout.placed.push_back({ r.index, r.x, r.y, r.w, r.h });
      if (!any) { minX = r.x; minY = r.y; maxX = r.x + r.w; maxY = r.y + r.h; any = true; }
      else
      {
         if (r.x < minX) minX = r.x;
         if (r.y < minY) minY = r.y;
         if (r.x + r.w > maxX) maxX = r.x + r.w;
         if (r.y + r.h > maxY) maxY = r.y + r.h;
      }
   }
   if (any)
   {
      layout.x = minX;
      layout.y = minY;
      layout.w = maxX - minX;
      layout.h = maxY - minY;
   }
   return layout;
}

// Cluster one set of center coordinates into ordered bands: walking the sorted
// values, start a new band when the gap to the previous value exceeds tol. Writes
// a 0-based band index per input element (in input order) and returns the count.
static int ClusterCenters(const std::vector<float>& vals, float tol, std::vector<int>& bandOf)
{
   const int n = (int)vals.size();
   bandOf.assign(n, 0);
   if (n == 0)
      return 0;
   std::vector<int> order(n);
   for (int i = 0; i < n; ++i)
      order[i] = i;
   std::sort(order.begin(), order.end(), [&vals](int a, int b) { return vals[a] < vals[b]; });
   int band = 0;
   float prev = vals[order[0]];
   bandOf[order[0]] = 0;
   for (int k = 1; k < n; ++k)
   {
      const int i = order[k];
      if (vals[i] - prev > tol)
         ++band;
      bandOf[i] = band;
      prev = vals[i];
   }
   return band + 1;
}

GridLayout AssignGrid(const std::vector<PlacedReel>& placed, float rowTol, float colTol)
{
   GridLayout g;
   const int n = (int)placed.size();
   g.pos.resize(n);
   if (n == 0)
      return g;

   std::vector<float> cy(n), cx(n);
   for (int i = 0; i < n; ++i)
   {
      cy[i] = placed[i].y + 0.5f * placed[i].h;
      cx[i] = placed[i].x + 0.5f * placed[i].w;
   }
   std::vector<int> rowOf, colOf;
   g.rows = ClusterCenters(cy, rowTol, rowOf);
   g.cols = ClusterCenters(cx, colTol, colOf);
   for (int i = 0; i < n; ++i)
   {
      g.pos[i].row = rowOf[i];
      g.pos[i].col = colOf[i];
   }
   return g;
}

} // namespace reel
