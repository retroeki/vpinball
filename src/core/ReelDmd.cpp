// license:GPLv3+

#include "core/stdafx.h"

#include "core/ReelDmd.h"

#include "core/player.h"
#include "parts/dispreel.h"
#include "renderer/Texture.h"

#include <vector>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <algorithm>

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

// Lowercase a name for case-insensitive comparison.
std::string ToLower(const std::string& s)
{
   std::string r(s);
   std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)std::tolower(c); });
   return r;
}
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

// Match the collected reels to the Gottlieb 4-player backglass naming convention
// (case-insensitive): ScoreReel1..4, Reel100K1..4, BIPReel, Credittxt. Any may be
// absent. Returns true if at least one ScoreReelN was found.
bool ReelDmd::IdentifyReels(const std::vector<DispReel*>& reels, ReelDmd::NamedReels& out)
{
   for (DispReel* r : reels)
   {
      const std::string n = ToLower(r->GetName());
      if (n == "scorereel1")      out.score[0] = r;
      else if (n == "scorereel2") out.score[1] = r;
      else if (n == "scorereel3") out.score[2] = r;
      else if (n == "scorereel4") out.score[3] = r;
      else if (n == "reel100k1")  out.k100[0] = r;
      else if (n == "reel100k2")  out.k100[1] = r;
      else if (n == "reel100k3")  out.k100[2] = r;
      else if (n == "reel100k4")  out.k100[3] = r;
      else if (n == "bipreel")    out.bip = r;
      else if (n == "credittxt")  out.credit = r;
   }
   return out.HasAnyScore();
}

bool ReelDmd::ShouldActivate() const
{
   if (m_player == nullptr)
      return false;
   std::vector<DispReel*> reels;
   CollectReels(m_player, reels);
   return !reels.empty();
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

   NamedReels named;
   const bool haveNamed = IdentifyReels(reels, named);

   // Build a combined change-signature over everything currently displayed, so we
   // only rebuild the composite when a shown value actually changes.
   uint64_t sig = 1469598103934665603ull; // FNV-1a basis
   const auto mix = [&sig](long v) {
      uint64_t x = (uint64_t)(v + 1); // shift past -1 sentinels
      for (int b = 0; b < 8; ++b) { sig ^= (x & 0xFF); sig *= 1099511628211ull; x >>= 8; }
   };

   DispReel* primary = nullptr; // fallback path's chosen single reel

   if (haveNamed)
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
      // Fallback: the score displays are the reel sets with the most reels (so we
      // skip single-digit credit / ball-in-play / overflow reels). Pick the
      // highest-value set among the largest ones (the active scorer).
      int maxReels = 0;
      for (DispReel* r : reels)
         if (r->GetReels() > maxReels)
            maxReels = r->GetReels();
      primary = reels[0];
      long bestValue = -1;
      for (DispReel* r : reels)
      {
         if (r->GetReels() != maxReels)
            continue;
         const long v = r->GetCurrentValue();
         if (v > bestValue)
         {
            bestValue = v;
            primary = r;
         }
      }
      mix(primary->GetCurrentValue());
   }

   // Diagnostic (temporary): periodically log every reel's name + reel-count +
   // value so we can confirm on-device which reels hold the live values.
   static int s_diagFrame = 0;
   if ((s_diagFrame++ % 120) == 0)
   {
      for (size_t i = 0; i < reels.size(); ++i)
         PLOGI << "[ReelDmd] reel[" << i << "] name='" << reels[i]->GetName() << "' reels=" << reels[i]->GetReels()
               << " value=" << reels[i]->GetCurrentValue();
      PLOGI << "[ReelDmd] mode=" << (haveNamed ? "backglass(named)" : "fallback(single)")
            << " sets=" << reels.size();
   }

   if (m_haveImage && m_haveSig && sig == m_lastSig)
      return; // nothing displayed changed, skip rebuild

   const bool built = haveNamed ? CompositeBackglass(named) : Composite(primary);
   if (!built)
   {
      // No usable strip art -> drop the channel rather than show stale/blank.
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
   if (reel == nullptr || m_player == nullptr || m_player->m_ptable == nullptr)
      return false;

   const Texture* pin = m_player->m_ptable->GetImage(reel->m_d.m_szImage);
   if (pin == nullptr)
      return false;

   std::shared_ptr<const BaseTexture> raw = pin->GetRawBitmap(false, 0);
   if (raw == nullptr)
      return false;

   std::shared_ptr<const BaseTexture> bmp = raw;
   if (bmp->m_format != BaseTexture::SRGB)
   {
      std::shared_ptr<BaseTexture> conv = raw->Convert(BaseTexture::SRGB);
      if (conv == nullptr)
         return false;
      bmp = conv;
   }

   const int imgW = (int)bmp->width();
   const int imgH = (int)bmp->height();
   if (imgW <= 0 || imgH <= 0)
      return false;

   const uint8_t* const src = static_cast<const uint8_t*>(bmp->datac());
   if (src == nullptr)
      return false;

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
      return false;

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
