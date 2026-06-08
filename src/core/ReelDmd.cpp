// license:GPLv3+

#include "core/stdafx.h"

#include "core/ReelDmd.h"

#include "core/player.h"
#include "core/VPXPluginAPIImpl.h"
#include "parts/dispreel.h"

#include <vector>

namespace
{
// DMD frame geometry for the numeric proof-of-pipeline render.
constexpr int DMD_W = 128;
constexpr int DMD_H = 32;

// 5x7 block-digit glyphs (0-9). Each row is 5 bits, MSB is the leftmost column.
// These are simple filled blocks, NOT faithful reel-strip art (that is a later task).
constexpr uint8_t kDigitGlyphs[10][7] = {
   { 0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110 }, // 0
   { 0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 }, // 1
   { 0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111 }, // 2
   { 0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110 }, // 3
   { 0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010 }, // 4
   { 0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110 }, // 5
   { 0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110 }, // 6
   { 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000 }, // 7
   { 0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110 }, // 8
   { 0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100 }, // 9
};

constexpr int GLYPH_W = 5;
constexpr int GLYPH_H = 7;
constexpr int GLYPH_GAP = 1; // pixel gap between digit cells

// Draw a single 5x7 block digit with its top-left at (ox, oy), luminance 1.0f.
void DrawDigit(float* const __restrict px, const int w, const int h, const int digit, const int ox, const int oy)
{
   if (digit < 0 || digit > 9)
      return;
   for (int gy = 0; gy < GLYPH_H; ++gy)
   {
      const int y = oy + gy;
      if (y < 0 || y >= h)
         continue;
      const uint8_t bits = kDigitGlyphs[digit][gy];
      for (int gx = 0; gx < GLYPH_W; ++gx)
      {
         if ((bits >> (GLYPH_W - 1 - gx)) & 1)
         {
            const int x = ox + gx;
            if (x < 0 || x >= w)
               continue;
            px[y * w + x] = 1.0f;
         }
      }
   }
}

// Render the absolute decimal value right-aligned into the frame as filled block digits.
void DrawNumberBlocks(float* const __restrict px, const int w, const int h, long value)
{
   // Decompose into decimal digits (at least one digit so 0 renders).
   unsigned long v = (value < 0) ? static_cast<unsigned long>(-value) : static_cast<unsigned long>(value);
   std::vector<int> digits;
   do
   {
      digits.push_back(static_cast<int>(v % 10));
      v /= 10;
   } while (v != 0);

   const int cellW = GLYPH_W + GLYPH_GAP;
   const int oy = (h - GLYPH_H) / 2; // vertically centered
   // Right-aligned: rightmost digit (digits[0]) sits at the right edge.
   int ox = w - GLYPH_W;
   for (size_t i = 0; i < digits.size() && ox + GLYPH_W > 0; ++i)
   {
      DrawDigit(px, w, h, digits[i], ox, oy);
      ox -= cellW;
   }
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

bool ReelDmd::ShouldActivate() const
{
   if (m_player == nullptr)
      return false;
   // A script-driven DMD sets m_dmdSize before pushing pixels (ScriptGlobalTable.cpp
   // put_DMDWidth/put_DMDHeight). Once we have populated our own frame, m_dmdSize is
   // (DMD_W, DMD_H), so keep activating to refresh it. Gate off only when some OTHER
   // source already owns the DMD at a different size.
   if (m_player->m_dmdSize.x != 0 && !(m_player->m_dmdSize.x == DMD_W && m_player->m_dmdSize.y == DMD_H))
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
      return;

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

   const bool hadFrame = (m_player->m_dmdFrame != nullptr);
   if (hadFrame && value == m_lastValue)
      return; // nothing changed, skip rebuild

   // (Re)allocate a 128x32 single-channel float luminance frame.
   if (!hadFrame || m_player->m_dmdFrame->width() != DMD_W || m_player->m_dmdFrame->height() != DMD_H
      || m_player->m_dmdFrame->m_format != BaseTexture::BW_FP32)
   {
      m_player->m_dmdFrame = BaseTexture::Create(DMD_W, DMD_H, BaseTexture::BW_FP32);
   }
   if (m_player->m_dmdFrame == nullptr)
      return;

   float* const __restrict px = static_cast<float*>(m_player->m_dmdFrame->data());
   const int count = DMD_W * DMD_H;
   for (int i = 0; i < count; ++i)
      px[i] = 0.0f;

   DrawNumberBlocks(px, DMD_W, DMD_H, value);

   m_player->m_dmdSize = int2(DMD_W, DMD_H);
   m_player->m_dmdFrameId++;
   m_lastValue = value;

   // On the first frame we create, announce the new display source so the ScoreView
   // (re)queries it (mirrors the script path in ScriptGlobalTable::put_DMDPixels).
   if (!hadFrame)
      VPXPluginAPIImpl::GetInstance().UpdateDMDSource(nullptr, true);
}
