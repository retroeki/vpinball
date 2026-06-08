// license:GPLv3+

#include "core/stdafx.h"

#include "core/ReelDmd.h"

#include "core/player.h"
#include "parts/dispreel.h"
#include "renderer/Texture.h"

#include <vector>
#include <cstdint>
#include <cstring>

// Implemented in lib/src/VPinballLib.cpp (statically linked into libvpinball.so).
// Hands a tightly-packed w*h*3 sRGB image of the active score reels to the
// ScoreView plugin's Image visual. (0,0,nullptr) clears the channel.
extern "C" void SetReelImage(int width, int height, const uint8_t* rgb);

namespace
{
// Cap the composited image width so a many-reel set with a large strip never
// blows up the upload. The reel digits are scaled by an integer factor when the
// natural cell size would exceed this.
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

   // The score displays are the reel sets with the most reels (so we skip the
   // single-digit credit / ball-in-play / 100K-overflow reels). EM tables have
   // one such set per player (ScoreReel1..4); in a 1-player game only the active
   // player's set is driven and the rest stay 0. Pick the highest-value set among
   // the largest ones — i.e. the active scorer. (Proper active-player tracking is
   // refined in a later task.)
   int maxReels = 0;
   for (DispReel* r : reels)
      if (r->GetReels() > maxReels)
         maxReels = r->GetReels();

   DispReel* primary = reels[0];
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

   const long value = primary->GetCurrentValue();

   // Diagnostic (temporary): periodically log every reel set's reel-count + value
   // so we can confirm on-device which set holds the live score.
   static int s_diagFrame = 0;
   if ((s_diagFrame++ % 120) == 0)
   {
      for (size_t i = 0; i < reels.size(); ++i)
         PLOGI << "[ReelDmd] reel[" << i << "] reels=" << reels[i]->GetReels()
               << " value=" << reels[i]->GetCurrentValue();
      PLOGI << "[ReelDmd] chosen value=" << value << " (maxReels=" << maxReels << ", sets=" << reels.size() << ')';
   }

   if (m_haveImage && value == m_lastValue)
      return; // nothing changed, skip rebuild

   if (!Composite(primary, value))
   {
      // No usable strip image -> drop the channel rather than show stale art.
      ClearActive();
      return;
   }
}

void ReelDmd::ClearActive()
{
   if (m_haveImage)
   {
      SetReelImage(0, 0, nullptr);
      m_haveImage = false;
   }
   m_lastValue = -1;
}

// Build a tightly-packed reelCount*cellW x cellH (after optional integer
// downscale) sRGB buffer by blitting each reel's current digit cell from the
// reel's strip image, laid out left-to-right (reel 0 = most significant = SetValue
// order). Returns false if the strip image is missing/unusable.
bool ReelDmd::Composite(DispReel* reel, long value)
{
   if (reel == nullptr)
      return false;

   const int reelCount = reel->GetReels();
   if (reelCount <= 0)
      return false;

   const Texture* pin = (m_player->m_ptable != nullptr) ? m_player->m_ptable->GetImage(reel->m_d.m_szImage) : nullptr;
   if (pin == nullptr)
      return false;

   std::shared_ptr<const BaseTexture> raw = pin->GetRawBitmap(false, 0);
   if (raw == nullptr)
      return false;

   // Normalize to tightly-packed 3-byte sRGB regardless of the source format
   // (handles SRGBA/RGBA alpha drop, SRGB565, float, etc. in one place).
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
   const int srcPitch = (int)bmp->pitch(); // bytes per row (== imgW*3 for SRGB)

   // Cell layout, matching DispReel::RenderSetup (dispreel.cpp:186-252).
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

   // Integer downscale factor so the full composite stays within kMaxImageWidth.
   int down = 1;
   while ((reelCount * cellW) / down > kMaxImageWidth)
      ++down;

   const int outCellW = cellW / down;
   const int outCellH = cellH / down;
   if (outCellW <= 0 || outCellH <= 0)
      return false;

   const int outW = reelCount * outCellW;
   const int outH = outCellH;
   m_scratch.assign((size_t)outW * outH * 3, 0);

   for (int i = 0; i < reelCount; ++i)
   {
      const int digit = reel->GetReelDigit(i);
      const int dr = (reel->m_d.m_digitrange > 0) ? reel->m_d.m_digitrange : 0;
      // Clamp the digit to the strip's available cells.
      const int cellIndex = (digit >= 0 && digit <= dr) ? digit : 0;
      const int gc = (gridCols > 0) ? (cellIndex % gridCols) : 0;
      const int gr = (gridCols > 0) ? (cellIndex / gridCols) : 0;
      const int srcCellX = gc * cellW; // top-left of source cell, in source px
      const int srcCellY = gr * cellH;
      const int dstCellX = i * outCellW; // reel 0 leftmost (most significant)

      for (int y = 0; y < outCellH; ++y)
      {
         const int sy = srcCellY + y * down;
         if (sy < 0 || sy >= imgH)
            continue;
         const uint8_t* const srow = src + (size_t)sy * srcPitch;
         uint8_t* const drow = m_scratch.data() + ((size_t)y * outW + dstCellX) * 3;
         for (int x = 0; x < outCellW; ++x)
         {
            const int sx = srcCellX + x * down;
            if (sx < 0 || sx >= imgW)
               continue;
            const uint8_t* const sp = srow + (size_t)sx * 3;
            uint8_t* const dp = drow + (size_t)x * 3;
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
         }
      }
   }

   SetReelImage(outW, outH, m_scratch.data());
   m_haveImage = true;
   m_lastValue = value;
   return true;
}
