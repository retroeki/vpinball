// license:GPLv3+

#include "core/stdafx.h"

#include "core/ReelDmd.h"

#include "core/player.h"
#include "parts/dispreel.h"
#include "renderer/Texture.h"
#include "core/ReelClassifier.h"
#include "utils/Logger.h"

#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#include <utility>

// Implemented in lib/src/VPinballLib.cpp (statically linked into libvpinball.so).
// Hands a tightly-packed w*h*3 sRGB image of the active score reels to the
// ScoreView plugin's Image visual. (0,0,nullptr) clears the channel.
extern "C" void SetReelImage(int width, int height, const uint8_t* rgb);

namespace
{
// Cap the composited image width so a many-reel set with a large strip never
// blows up the upload. Cells are scaled by an integer factor when the natural
// composite would exceed this.
constexpr int kMaxImageWidth = 1024;
} // namespace

ReelDmd::ReelDmd(Player* player)
   : m_player(player)
{
}

// Collect the live DispReel parts (item type eItemDispReel) from the player's
// renderable parts. Uses the same iterate-m_vhitables / static_cast idiom as
// player.cpp:683-684 (eItemBall -> static_cast<Ball*>).
static void CollectReels(Player* player, std::vector<DispReel*>& out)
{
   if (player == nullptr)
      return;
   for (IEditable* hitable : player->m_vhitables)
      if (hitable->GetItemType() == ItemTypeEnum::eItemDispReel)
         out.push_back(static_cast<DispReel*>(hitable));
}

// Translate the live DispReel parts into the engine-independent descriptors the
// classifier works on (name + wheel count + digit range + current value).
static void BuildInputs(const std::vector<DispReel*>& reels, std::vector<reel::ReelInput>& out)
{
   out.clear();
   out.reserve(reels.size());
   for (DispReel* r : reels)
   {
      reel::ReelInput in;
      in.name = r->GetName();
      in.reelCount = r->GetReels();
      in.digitRange = r->GetRange();
      in.currentValue = (long)r->GetCurrentValue();
      in.hasImage = !r->m_d.m_szImage.empty();
      out.push_back(std::move(in));
   }
}

bool ReelDmd::ShouldActivate() const
{
   if (m_player == nullptr)
      return false;
   std::vector<DispReel*> reels;
   CollectReels(m_player, reels);
   if (reels.empty())
      return false;
   // Activate for any table that exposes a genuine numeric score reel (>=2
   // decimal wheels), regardless of how the author named it. Solid-state tables
   // (e.g. Bally Mata Hari) model only decorative single-digit status flags
   // (Match/Tilt/Credits/GameOver/...) as DispReels while their real score lives
   // on PinMAME segment/DMD displays; those have one wheel each, so the
   // classifier does not activate on them and the ScoreView keeps showing the
   // real display. See ReelClassifier.h.
   std::vector<reel::ReelInput> inputs;
   BuildInputs(reels, inputs);
   const bool act = reel::ClassifyReels(inputs).activate;
   if ((int)act != m_loggedActivate)
   {
      m_loggedActivate = (int)act;
      PLOGI << "[ReelDmd] ShouldActivate=" << (act ? "true" : "false") << " dispReels=" << reels.size();
      // One-shot ground-truth dump: every reel's name/geometry/image, so we can
      // see exactly what ReelDmd reads (e.g. whether m_szImage is populated).
      const int nImages = (m_player->m_ptable != nullptr) ? (int)m_player->m_ptable->GetImageList().size() : -1;
      PLOGI << "[ReelDmd] DUMP tableImages=" << nImages;
      for (DispReel* r : reels)
         PLOGI << "[ReelDmd]   reel '" << r->GetName() << "' rc=" << r->GetReels()
               << " dr=" << r->GetRange() << " grid=" << (r->m_d.m_useImageGrid ? 1 : 0)
               << " vis=" << (r->m_d.m_visible ? 1 : 0) << " img='" << r->m_d.m_szImage << "'";
   }
   return act;
}

void ReelDmd::Update()
{
   if (m_player == nullptr)
      return;

   std::vector<DispReel*> reels;
   CollectReels(m_player, reels);
   if (reels.empty())
   {
      ClearActive();
      return;
   }

   std::vector<reel::ReelInput> inputs;
   BuildInputs(reels, inputs);
   const reel::ReelPlan plan = reel::ClassifyReels(inputs);
   if (!plan.activate)
   {
      // Not an EM score-reel table (e.g. a solid-state table whose DispReels are
      // single-digit status flags, with the real score on PinMAME displays). Drop
      // the channel so the ScoreView renders the real DMD/segment instead.
      ClearActive();
      return;
   }

   // Map the plan's chosen reels back to the live DispReel parts.
   NamedReels named;
   std::vector<DispReel*> genericScore;
   DispReel* primary = nullptr;
   if (plan.mode == reel::ReelMode::Gottlieb4Player)
   {
      for (int p = 0; p < 4; ++p)
      {
         if (plan.score[p] >= 0) named.score[p] = reels[plan.score[p]];
         if (plan.k100[p] >= 0)  named.k100[p] = reels[plan.k100[p]];
      }
      if (plan.bip >= 0)    named.bip = reels[plan.bip];
      if (plan.credit >= 0) named.credit = reels[plan.credit];
   }
   else // GenericActiveScore: place every score reel at its real backglass position
   {
      genericScore.reserve(plan.scoreReels.size());
      for (int idx : plan.scoreReels)
         if (idx >= 0 && idx < (int)reels.size())
            genericScore.push_back(reels[idx]);
      if (plan.primaryScore >= 0)
         primary = reels[plan.primaryScore]; // single-reel fallback (active scorer)
   }

   // Build a combined change-signature over everything we will display, so we
   // only rebuild the composite when a shown value actually changes.
   uint64_t sig = 1469598103934665603ull; // FNV-1a basis
   const auto mix = [&sig](long v) {
      uint64_t x = (uint64_t)(v + 1); // shift past -1 sentinels
      for (int b = 0; b < 8; ++b) { sig ^= (x & 0xFF); sig *= 1099511628211ull; x >>= 8; }
   };

   if (plan.mode == reel::ReelMode::Gottlieb4Player)
   {
      for (int p = 0; p < 4; ++p)
      {
         mix(named.score[p] ? named.score[p]->GetCurrentValue() : -1);
         mix(named.k100[p] ? named.k100[p]->GetCurrentValue() : -1);
      }
      mix(named.bip ? named.bip->GetCurrentValue() : -1);
      mix(named.credit ? named.credit->GetCurrentValue() : -1);
   }
   else
   {
      for (DispReel* r : genericScore)
      {
         mix(r->GetCurrentValue());
         mix(r->m_d.m_visible ? 1 : 0); // lit/dim visibility swap changes what is shown
      }
   }

   if (m_haveImage && m_haveSig && sig == m_lastSig)
      return; // nothing displayed changed, skip rebuild

   const bool built = (plan.mode == reel::ReelMode::Gottlieb4Player)
      ? CompositeBackglass(named)
      : (CompositeByPosition(genericScore) || (primary != nullptr && Composite(primary)));
   if (!built)
   {
      // No usable strip art -> drop the channel rather than show stale/blank.
      static int s_failLog = 12; // throttle: LoadStrip logs carry the real reason
      if (s_failLog > 0) { --s_failLog; PLOGW << "[ReelDmd] build produced no image (mode=" << (int)plan.mode
            << " scoreReels=" << plan.scoreReels.size() << " generic=" << genericScore.size() << ")"; }
      ClearActive();
      return;
   }

   m_lastSig = sig;
   m_haveSig = true;
}

void ReelDmd::ClearActive()
{
   if (m_haveImage)
   {
      SetReelImage(0, 0, nullptr);
      m_haveImage = false;
   }
   m_haveSig = false;
}

// Load + normalize a reel's strip image to tightly-packed 3-byte sRGB and compute
// its source cell geometry, matching DispReel::RenderSetup (dispreel.cpp:186-252).
bool ReelDmd::LoadStrip(DispReel* reel, ReelStrip& out) const
{
   out = ReelStrip();
   static int s_log = 40; // bounded diagnostics budget
   if (reel == nullptr || m_player == nullptr || m_player->m_ptable == nullptr)
      return false;

   const std::string imgName = reel->m_d.m_szImage;
   const Texture* pin = m_player->m_ptable->GetImage(imgName);
   if (pin == nullptr)
   {
      if (s_log > 0) { --s_log; PLOGW << "[ReelDmd] LoadStrip FAIL GetImage('" << imgName << "') -> null"; }
      return false;
   }

   std::shared_ptr<const BaseTexture> raw = pin->GetRawBitmap(false, 0);
   if (raw == nullptr)
   {
      if (s_log > 0) { --s_log; PLOGW << "[ReelDmd] LoadStrip FAIL GetRawBitmap('" << imgName << "') null, fileSize=" << pin->GetFileSize(); }
      return false;
   }

   std::shared_ptr<const BaseTexture> bmp = raw;
   if (bmp->m_format != BaseTexture::SRGB)
   {
      std::shared_ptr<BaseTexture> conv = raw->Convert(BaseTexture::SRGB);
      if (conv == nullptr)
      {
         if (s_log > 0) { --s_log; PLOGW << "[ReelDmd] LoadStrip FAIL Convert(SRGB) null '" << imgName << "' fmt=" << (int)bmp->m_format; }
         return false;
      }
      bmp = conv;
   }

   const int imgW = (int)bmp->width();
   const int imgH = (int)bmp->height();
   if (imgW <= 0 || imgH <= 0)
   {
      if (s_log > 0) { --s_log; PLOGW << "[ReelDmd] LoadStrip FAIL bad size " << imgW << "x" << imgH << " '" << imgName << "'"; }
      return false;
   }

   const uint8_t* const src = static_cast<const uint8_t*>(bmp->datac());
   if (src == nullptr)
   {
      if (s_log > 0) { --s_log; PLOGW << "[ReelDmd] LoadStrip FAIL datac() null '" << imgName << "'"; }
      return false;
   }

   int gridCols, gridRows;
   if (reel->m_d.m_useImageGrid)
   {
      gridCols = reel->m_d.m_imagesPerGridRow;
      if (gridCols != 0)
      {
         gridRows = (reel->m_d.m_digitrange + 1) / gridCols;
         if ((gridRows * gridCols) < (reel->m_d.m_digitrange + 1))
            ++gridRows;
      }
      else
         gridRows = 1;
   }
   else
   {
      gridCols = reel->m_d.m_digitrange + 1;
      gridRows = 1;
   }
   if (gridCols <= 0 || gridRows <= 0)
      return false;

   const int cellW = imgW / gridCols;
   const int cellH = imgH / gridRows;
   if (cellW <= 0 || cellH <= 0)
   {
      if (s_log > 0) { --s_log; PLOGW << "[ReelDmd] LoadStrip FAIL cell " << cellW << "x" << cellH << " (img " << imgW << "x" << imgH << " grid " << gridCols << "x" << gridRows << ") '" << imgName << "'"; }
      return false;
   }

   out.bmp = bmp;
   out.src = src;
   out.imgW = imgW;
   out.imgH = imgH;
   out.srcPitch = (int)bmp->pitch(); // bytes per row (== imgW*3 for SRGB)
   out.gridCols = gridCols;
   out.gridRows = gridRows;
   out.cellW = cellW;
   out.cellH = cellH;
   out.ok = true;
   if (s_log > 0) { --s_log; PLOGI << "[ReelDmd] LoadStrip OK '" << imgName << "' img=" << imgW << "x" << imgH << " cells=" << gridCols << "x" << gridRows << " fmt=" << (int)bmp->m_format; }
   return true;
}

// Blit `count` of the reel's current digit cells (starting at reel index
// startReel) into m_scratch, nearest-neighbor scaled to dstCellW x dstCellH,
// packed left to right starting at pixel (dstX, dstY). Returns the destination x
// just past the last cell drawn.
int ReelDmd::BlitReelCells(const ReelStrip& strip, DispReel* reel, int startReel, int count,
                           int dstX, int dstY, int dstCellW, int dstCellH)
{
   if (!strip.ok || reel == nullptr || dstCellW <= 0 || dstCellH <= 0)
      return dstX;

   const int dr = (reel->m_d.m_digitrange > 0) ? reel->m_d.m_digitrange : 0;

   // Each glyph is drawn aspect-preserving (contain-fit) and centered within its
   // dstCellW x dstCellH box. Reel strips differ wildly in cell aspect (the main
   // gottlieb digit is tall, the 100K/credit glyphs are wide/short), so a plain
   // stretch would badly distort them; contain-fit keeps each glyph true.
   const int srcCW = strip.cellW;
   const int srcCH = strip.cellH;
   // Fit srcCW:srcCH inside dstCellW:dstCellH.
   int drawW = dstCellW;
   int drawH = (srcCW > 0) ? (int)((int64_t)dstCellW * srcCH / srcCW) : dstCellH;
   if (drawH > dstCellH)
   {
      drawH = dstCellH;
      drawW = (srcCH > 0) ? (int)((int64_t)dstCellH * srcCW / srcCH) : dstCellW;
   }
   drawW = std::max(1, std::min(drawW, dstCellW));
   drawH = std::max(1, std::min(drawH, dstCellH));
   const int padX = (dstCellW - drawW) / 2;
   const int padY = (dstCellH - drawH) / 2;

   for (int i = 0; i < count; ++i)
   {
      const int reelIdx = startReel + i;
      const int digit = reel->GetReelDigit(reelIdx);
      const int cellIndex = (digit >= 0 && digit <= dr) ? digit : 0;
      const int gc = (strip.gridCols > 0) ? (cellIndex % strip.gridCols) : 0;
      const int gr = (strip.gridCols > 0) ? (cellIndex / strip.gridCols) : 0;
      const int srcCellX = gc * strip.cellW; // top-left of source cell, in source px
      const int srcCellY = gr * strip.cellH;
      const int boxX = dstX + i * dstCellW;

      for (int y = 0; y < drawH; ++y)
      {
         const int dy = dstY + padY + y;
         if (dy < 0 || dy >= m_outH)
            continue;
         // Nearest-neighbor source row within the source cell.
         const int sy = srcCellY + (drawH > 1 ? (y * (srcCH - 1)) / (drawH - 1) : 0);
         if (sy < 0 || sy >= strip.imgH)
            continue;
         const uint8_t* const srow = strip.src + (size_t)sy * strip.srcPitch;
         for (int x = 0; x < drawW; ++x)
         {
            const int dx = boxX + padX + x;
            if (dx < 0 || dx >= m_outW)
               continue;
            const int sx = srcCellX + (drawW > 1 ? (x * (srcCW - 1)) / (drawW - 1) : 0);
            if (sx < 0 || sx >= strip.imgW)
               continue;
            const uint8_t* const sp = srow + (size_t)sx * 3;
            uint8_t* const dp = m_scratch.data() + ((size_t)dy * m_outW + dx) * 3;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
         }
      }
   }
   return dstX + count * dstCellW;
}

// Build the full 2x2 backglass composite: four player score cells (each folding
// in its Reel100K overflow digit) in a 2x2 grid, plus a centered ball-in-play /
// credit row underneath.
bool ReelDmd::CompositeBackglass(const NamedReels& named)
{
   // The main score strip drives the base cell geometry; find any present score
   // reel to anchor it.
   ReelStrip scoreStrip[4];
   DispReel* anchorScore = nullptr;
   for (int p = 0; p < 4; ++p)
   {
      if (named.score[p] != nullptr && LoadStrip(named.score[p], scoreStrip[p]))
      {
         if (anchorScore == nullptr)
            anchorScore = named.score[p];
      }
   }
   if (anchorScore == nullptr)
      return false; // no usable main score art

   // Base cell from the first usable score strip. All other reels' cells are
   // scaled to this cell height so glyphs line up; cell width keeps each strip's
   // own cell aspect.
   ReelStrip anchorStrip;
   if (!LoadStrip(anchorScore, anchorStrip))
      return false;

   // Integer downscale so the whole composite stays within kMaxImageWidth. The
   // widest element is the player grid: 2 columns, each up to (1 + maxScoreReels)
   // base-width cells, plus a column gap.
   int maxScoreReels = 0;
   for (int p = 0; p < 4; ++p)
      if (named.score[p] != nullptr)
         maxScoreReels = std::max(maxScoreReels, named.score[p]->GetReels());
   if (maxScoreReels <= 0)
      return false;
   const bool anyK100 = named.k100[0] || named.k100[1] || named.k100[2] || named.k100[3];
   const int maxCellsPerCol = (anyK100 ? 1 : 0) + maxScoreReels;

   int down = 1;
   while ((2 * maxCellsPerCol * anchorStrip.cellW) / down > kMaxImageWidth)
      ++down;

   // Uniform digit-box geometry. Every score / 100K digit occupies one
   // baseCellW x baseCellH box; BlitReelCells contain-fits each glyph inside it
   // (so the wide 100K glyph and the tall main digits each render true, just
   // centered in a common box). This keeps the four player columns aligned.
   const int baseCellW = std::max(1, anchorStrip.cellW / down);
   const int baseCellH = std::max(1, anchorStrip.cellH / down);

   // Each player column is (optional 100K box) + maxScoreReels boxes wide, so
   // every column has the same width and the ones-place lines up across players.
   ReelStrip k100Strip[4];
   int scoreReels[4] = { 0, 0, 0, 0 };
   bool playerHas[4] = { false, false, false, false };
   bool k100Has[4] = { false, false, false, false };
   for (int p = 0; p < 4; ++p)
   {
      if (named.score[p] == nullptr || !scoreStrip[p].ok)
         continue;
      playerHas[p] = true;
      scoreReels[p] = named.score[p]->GetReels();
      if (named.k100[p] != nullptr && LoadStrip(named.k100[p], k100Strip[p]))
         k100Has[p] = true;
   }

   const int gap = std::max(2, baseCellH / 8);                  // gap between grid cells
   const int colCells = maxCellsPerCol;                         // boxes per column
   const int colW = colCells * baseCellW;
   const int rowH = baseCellH;
   const int gridW = 2 * colW + gap;
   const int gridH = 2 * rowH + gap;

   // Ball-in-play / credit row, smaller (~0.7x base box), centered beneath grid.
   ReelStrip bipStrip, creditStrip;
   const bool haveBip = (named.bip != nullptr) && LoadStrip(named.bip, bipStrip);
   const bool haveCredit = (named.credit != nullptr) && LoadStrip(named.credit, creditStrip);
   const int smallCellH = std::max(1, (baseCellH * 7) / 10);
   const int smallCellW = std::max(1, (baseCellW * 7) / 10);
   const int bipReels = haveBip ? named.bip->GetReels() : 0;
   const int creditReels = haveCredit ? named.credit->GetReels() : 0;
   const int rowSpacer = (haveBip && haveCredit) ? baseCellW : 0; // gap between the two readouts
   const int bipRowW = (bipReels + creditReels) * smallCellW + rowSpacer;
   const int bipRowH = (haveBip || haveCredit) ? smallCellH : 0;
   const int rowGap = (bipRowH > 0) ? gap : 0;

   // Final composite extents (no outer margin; the box's Fit:Contain centers it).
   const int outW = std::max(gridW, bipRowW);
   const int outH = gridH + rowGap + bipRowH;
   if (outW <= 0 || outH <= 0)
      return false;

   m_outW = outW;
   m_outH = outH;
   m_scratch.assign((size_t)outW * outH * 3, 0);

   // Grid cell origins: SR1 top-left, SR2 top-right, SR3 bottom-left, SR4 bottom-right.
   const int cellX[4] = { 0, colW + gap, 0, colW + gap };
   const int cellY[4] = { 0, 0, rowH + gap, rowH + gap };

   for (int p = 0; p < 4; ++p)
   {
      if (!playerHas[p])
         continue;
      // Right-align the player's boxes within the uniform column so scores of
      // differing reel counts share the ones-place column. Content boxes =
      // (this player's 100K box, if any) + this player's score reels.
      const int contentBoxes = (k100Has[p] ? 1 : 0) + scoreReels[p];
      int dx = cellX[p] + (colCells - contentBoxes) * baseCellW; // right-align
      const int dy = cellY[p];
      if (k100Has[p])
         dx = BlitReelCells(k100Strip[p], named.k100[p], 0, named.k100[p]->GetReels(), dx, dy, baseCellW, rowH);
      BlitReelCells(scoreStrip[p], named.score[p], 0, scoreReels[p], dx, dy, baseCellW, rowH);
   }

   // BIP / credit row, centered horizontally beneath the grid.
   if (bipRowH > 0)
   {
      const int rowY = gridH + rowGap;
      int dx = (outW - bipRowW) / 2;
      if (dx < 0)
         dx = 0;
      if (haveBip)
         dx = BlitReelCells(bipStrip, named.bip, 0, bipReels, dx, rowY, smallCellW, smallCellH);
      if (haveBip && haveCredit)
         dx += rowSpacer;
      if (haveCredit)
         BlitReelCells(creditStrip, named.credit, 0, creditReels, dx, rowY, smallCellW, smallCellH);
   }

   SetReelImage(outW, outH, m_scratch.data());
   m_haveImage = true;
   return true;
}

// Faithful multi-reel composite: place each visible, on-backglass score reel at
// its real backglass position so the box mirrors the table's EM head (a 4-player
// 2x2, a single score, whatever the table actually shows). Naming-independent;
// honoring each reel's live visibility resolves the dim/lit overlapping reel pairs
// that 4-player EM recreations use (only the shown reel is drawn).
//
// Reel positions/sizes are in the editor canvas space (EDITOR_BG_WIDTH x
// EDITOR_BG_HEIGHT), the same coordinates DispReel::Render uses.
bool ReelDmd::CompositeByPosition(const std::vector<DispReel*>& scoreReels)
{
   if (scoreReels.empty())
      return false;

   // Keep reels whose center lies within this margin of the backglass canvas. It
   // drops off-canvas HUD duplicates (parked at x>1000) while keeping edge reels
   // that slightly overhang the canvas (e.g. Royal Flush's ScoreReel1 at x=-3).
   constexpr float kMargin = 150.0f;

   // Build each visible reel's canvas-space rect, mirroring DispReel::Render's
   // advance: first digit at m_v1 + spacing, each digit cw wide, stepping cw + sp.
   std::vector<reel::ReelRect> rects;
   rects.reserve(scoreReels.size());
   for (int i = 0; i < (int)scoreReels.size(); ++i)
   {
      DispReel* r = scoreReels[i];
      if (r == nullptr)
         continue;
      const int rc = r->GetReels();
      if (rc <= 0)
         continue;
      const float cw = r->m_d.m_width;
      const float ch = r->m_d.m_height;
      const float sp = (r->m_d.m_reelspacing > 0.0f) ? r->m_d.m_reelspacing : 0.0f;
      reel::ReelRect rect;
      rect.index = i;
      rect.x = r->m_d.m_v1.x + sp;
      rect.y = r->m_d.m_v1.y + sp;
      rect.w = (float)rc * (cw + sp);
      rect.h = ch;
      rect.visible = r->m_d.m_visible; // only mirror what the backglass actually shows
      rects.push_back(rect);
   }

   // Prefer the currently-visible reels (in a game, the lit reel of each dim/lit
   // pair). If nothing is visible yet (e.g. attract, before the script lights a
   // player up), fall back to all on-canvas reels so the box still shows the
   // scores instead of staying blank.
   reel::ReelLayout layout = reel::PlaceReels(rects, (float)EDITOR_BG_WIDTH, (float)EDITOR_BG_HEIGHT, kMargin, /*requireVisible*/ true);
   if (layout.placed.empty())
      layout = reel::PlaceReels(rects, (float)EDITOR_BG_WIDTH, (float)EDITOR_BG_HEIGHT, kMargin, /*requireVisible*/ false);
   if (layout.placed.empty() || layout.w <= 0.0f || layout.h <= 0.0f)
      return false;

   // Load each placed reel's strip once and gather geometry for the grid.
   const int nPlaced = (int)layout.placed.size();
   std::vector<ReelStrip> strips(nPlaced);
   std::vector<int> reelCounts(nPlaced, 0);
   std::vector<bool> okReel(nPlaced, false);
   float sumH = 0.0f, sumBlockW = 0.0f;
   int okCount = 0, anchor = -1;
   for (int i = 0; i < nPlaced; ++i)
   {
      DispReel* r = scoreReels[layout.placed[i].index];
      if (!LoadStrip(r, strips[i]))
         continue;
      reelCounts[i] = std::max(1, r->GetReels());
      okReel[i] = true;
      if (anchor < 0) anchor = i;
      sumH += layout.placed[i].h;
      sumBlockW += layout.placed[i].w;
      ++okCount;
   }
   if (anchor < 0)
      return false;

   const float avgH = sumH / (float)okCount;
   const float avgBlockW = sumBlockW / (float)okCount;

   // Cluster the reels into a compact row/col grid (4 players -> 2x2, a single
   // score -> 1x1) instead of spreading them across the backglass with black gaps.
   const reel::GridLayout grid = reel::AssignGrid(layout.placed, avgH * 0.6f, avgBlockW * 0.6f);
   const int gridCols = std::max(1, grid.cols);
   const int gridRows = std::max(1, grid.rows);

   // Uniform digit cell sized from the SOURCE strip aspect, so digits keep their
   // true proportions (the on-playfield width/height squashes them - that was the
   // stretch). Lay out for a target height, shrinking it if we'd exceed kMaxImageWidth.
   const float srcAR = (strips[anchor].cellH > 0)
      ? (float)strips[anchor].cellW / (float)strips[anchor].cellH : 0.5f;

   int cellH = 120, cellW = 1, gapX = 2, gapY = 2, border = 2, outW = 1, outH = 1;
   std::vector<int> colW;
   for (int attempt = 0; attempt < 10; ++attempt)
   {
      cellW = std::max(1, (int)(cellH * srcAR + 0.5f));
      gapX = std::max(2, cellW / 3);
      gapY = std::max(2, cellH / 4);
      border = std::max(3, cellH / 6);
      colW.assign(gridCols, 0);
      for (int i = 0; i < nPlaced; ++i)
         if (okReel[i])
         {
            const int bw = reelCounts[i] * cellW;
            if (bw > colW[grid.pos[i].col]) colW[grid.pos[i].col] = bw;
         }
      int gridW = (gridCols - 1) * gapX;
      for (int w : colW) gridW += w;
      const int gridH = gridRows * cellH + (gridRows - 1) * gapY;
      outW = gridW + 2 * border;
      outH = gridH + 2 * border;
      if (outW <= kMaxImageWidth)
         break;
      cellH = std::max(8, (cellH * kMaxImageWidth) / outW); // shrink and retry
   }

   m_outW = outW;
   m_outH = outH;
   // Fill with a muted surround colour (not pure black) so the reels read as a
   // framed readout; the digit strips (black drum + light digit) draw over it, so
   // the gaps and border become the surround.
   m_scratch.assign((size_t)outW * outH * 3, 0);
   for (size_t p = 0; p + 2 < m_scratch.size(); p += 3)
   {
      m_scratch[p] = 28; m_scratch[p + 1] = 28; m_scratch[p + 2] = 34;
   }

   // Column x-origins (columns laid left to right with gapX between).
   std::vector<int> colX(gridCols, border);
   for (int c = 1; c < gridCols; ++c)
      colX[c] = colX[c - 1] + colW[c - 1] + gapX;

   for (int i = 0; i < nPlaced; ++i)
   {
      if (!okReel[i])
         continue;
      const int col = grid.pos[i].col, row = grid.pos[i].row;
      const int blockW = reelCounts[i] * cellW;
      const int dx = colX[col] + (colW[col] - blockW); // right-align so the ones place lines up
      const int dy = border + row * (cellH + gapY);
      BlitReelCells(strips[i], scoreReels[layout.placed[i].index], 0, reelCounts[i], dx, dy, cellW, cellH);
   }

   PLOGI << "[ReelDmd] CompositeByPosition placed=" << nPlaced << " grid=" << gridRows << "x" << gridCols
         << " cell=" << cellW << "x" << cellH << " image=" << outW << "x" << outH;
   SetReelImage(outW, outH, m_scratch.data());
   m_haveImage = true;
   return true;
}

// Single active-reel fallback (increment-1 behavior): composite one reel's
// current digits left to right (reel 0 = most significant).
bool ReelDmd::Composite(DispReel* reel)
{
   if (reel == nullptr)
      return false;

   ReelStrip strip;
   if (!LoadStrip(reel, strip))
      return false;

   const int reelCount = reel->GetReels();
   if (reelCount <= 0)
      return false;

   int down = 1;
   while ((reelCount * strip.cellW) / down > kMaxImageWidth)
      ++down;

   const int outCellW = std::max(1, strip.cellW / down);
   const int outCellH = std::max(1, strip.cellH / down);

   const int outW = reelCount * outCellW;
   const int outH = outCellH;
   m_outW = outW;
   m_outH = outH;
   m_scratch.assign((size_t)outW * outH * 3, 0);

   BlitReelCells(strip, reel, 0, reelCount, 0, 0, outCellW, outCellH);

   SetReelImage(outW, outH, m_scratch.data());
   m_haveImage = true;
   return true;
}
