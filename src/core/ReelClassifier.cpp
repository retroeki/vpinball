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

static bool Has(const std::string& haystack, const char* needle)
{
   return haystack.find(needle) != std::string::npos;
}

bool IsScoreReel(const ReelInput& r)
{
   // Two-or-more decimal (0-9) digit wheels, backed by a drawable strip image.
   // See header for the full rationale; this rule admits real EM score reels
   // regardless of how the table author named them, while excluding single-digit
   // status/credit flags and imageless data reels.
   return r.reelCount >= 2 && r.digitRange == 9 && r.hasImage;
}

ReelRole ClassifyRole(const ReelInput& r)
{
   if (!r.hasImage)
      return ReelRole::Ignore; // nothing to draw

   const std::string nm = ToLower(r.name);

   // Multi-wheel numeric reels are player scores - unless named as a high-score
   // readout (those are static records, not a live player's score).
   if (IsScoreReel(r))
      return (Has(nm, "high") || Has(nm, "hscore")) ? ReelRole::Ignore : ReelRole::Score;

   // From here, single (or non-9) wheels: only the auxiliary readouts are kept.
   if (r.reelCount != 1)
      return ReelRole::Ignore;

   const std::string img = ToLower(r.image);
   const std::string s = nm + " " + img;

   // Player-up lamp (which player is currently at bat): kept as STATE only - the
   // renderer highlights that player's score row - so it is recognised before the
   // exclusions and never drawn as a number. (numplayer / canplay stay excluded.)
   if (Has(nm, "playerup") || Has(nm, "player up")
      || Has(nm, "p1up") || Has(nm, "p2up") || Has(nm, "p3up") || Has(nm, "p4up"))
      return ReelRole::CurrentPlayer;

   // Exclude obvious non-readout single reels first (so e.g. a status lamp that
   // happens to reuse a ball-in-play image is not misread as ball-in-play).
   if (Has(nm, "tilt") || Has(nm, "gameover") || Has(nm, "game_over") || Has(nm, "game-over")
      || Has(nm, "shootagain") || Has(nm, "shoot again")
      || Has(nm, "canplay") || Has(nm, "numplayer") || Has(nm, "thermo") || Has(nm, "match")
      || Has(nm, "card") || Has(nm, "lastball") || Has(nm, "door") || Has(nm, "horse") || Has(nm, "hat")
      || Has(nm, "reel1000") || Has(nm, "scores") || Has(nm, "high") || Has(nm, "hscore"))
      return ReelRole::Ignore;

   // Overflow / 100K rollover carry digit.
   if (Has(s, "100k") || Has(s, "100000") || Has(s, "1000k") || Has(s, "rollover")
      || Has(s, "k100") || Has(s, "million") || Has(s, "10m"))
      return ReelRole::Overflow;

   // Credit readout (name or a credit-wheel image).
   if (Has(nm, "credit") || Has(img, "credit"))
      return ReelRole::Credit;

   // Ball-in-play readout.
   if (Has(nm, "ballinplay") || Has(nm, "ball in play") || Has(nm, "ball_in_play")
      || Has(nm, "inplay") || nm.compare(0, 3, "bip") == 0 || Has(nm, "bipreel"))
      return ReelRole::BallInPlay;

   return ReelRole::Ignore;
}

ReelPlan ClassifyReels(const std::vector<ReelInput>& reels)
{
   ReelPlan plan;
   const int n = (int)reels.size();
   plan.roles.resize(n, ReelRole::Ignore);

   for (int i = 0; i < n; ++i)
   {
      const ReelRole role = ClassifyRole(reels[i]);
      plan.roles[i] = role;
      switch (role)
      {
      case ReelRole::Score:         plan.scoreReels.push_back(i); break;
      case ReelRole::Overflow:      plan.overflowReels.push_back(i); break;
      case ReelRole::Credit:        plan.creditReels.push_back(i); break;
      case ReelRole::BallInPlay:    plan.bipReels.push_back(i); break;
      case ReelRole::CurrentPlayer: plan.playerUpReels.push_back(i); break;
      default: break;
      }
   }

   plan.activate = !plan.scoreReels.empty();
   if (plan.activate)
   {
      // Active scorer: highest current value (the player in the lead), tie-broken
      // by the most digit wheels (the main score), then lowest index for stability.
      int best = plan.scoreReels[0];
      for (int idx : plan.scoreReels)
      {
         const ReelInput& a = reels[idx];
         const ReelInput& b = reels[best];
         if (a.currentValue != b.currentValue) { if (a.currentValue > b.currentValue) best = idx; }
         else if (a.reelCount != b.reelCount)   { if (a.reelCount > b.reelCount)       best = idx; }
      }
      plan.primaryScore = best;
   }
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
