// license:GPLv3+

#include "core/stdafx.h"

#include "core/ReelDmd.h"

#include "core/player.h"
#include "parts/dispreel.h"
#include "renderer/Texture.h"
#include "core/ReelClassifier.h"
#include "core/extern.h" // g_pvp (resolves the bundled-assets path for the label font)
#include "utils/Logger.h"

#include <vector>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
#include <utility>

#ifdef __STANDALONE__
#include <SDL3_ttf/SDL_ttf.h>
#endif

// Implemented in lib/src/VPinballLib.cpp (statically linked into libvpinball.so).
// Hands a tightly-packed w*h*4 sRGBA image of the active score reels to the
// ScoreView plugin's Image visual. Digits are opaque; the surround panel is
// semi-transparent so the table shows through behind the reels.
// (0,0,nullptr) clears the channel.
extern "C" void SetReelImage(int width, int height, const uint8_t* rgba);

namespace
{
// Cap the composited image width so a many-reel set with a large strip never
// blows up the upload. Cells are scaled by an integer factor when the natural
// composite would exceed this.
constexpr int kMaxImageWidth = 1024;

// Surround panel (the area behind/around the score digits): a gunmetal vertical
// gradient with a SEMI-TRANSPARENT field, so the cabinet behind the score box
// shows through (the renderer skips the embedded score-view's opaque black fill
// when a reel image is active - see Renderer::ClearEmbeddedAncillaryWindow). The
// raised bevel frame, the per-digit window borders, and the digits themselves
// are written fully opaque (those set a=255), so the panel reads as glass-with-
// chrome: solid frame + solid digit bezels, see-through field between them.
constexpr uint8_t kPanelTopR = 60, kPanelTopG = 63, kPanelTopB = 72, kPanelTopA = 184; // ~72% field
constexpr uint8_t kPanelBotR = 26, kPanelBotG = 27, kPanelBotB = 34, kPanelBotA = 204; // ~80% at base

// Active-player / lit-indicator accent (app primary indigo).
constexpr uint8_t kAccentR = 99, kAccentG = 116, kAccentB = 230;
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
      in.image = r->m_d.m_szImage;
      in.reelCount = r->GetReels();
      in.digitRange = r->GetRange();
      in.currentValue = (long)r->GetCurrentValue();
      in.hasImage = !r->m_d.m_szImage.empty();
      in.useImageGrid = r->m_d.m_useImageGrid;
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
   const bool decimal = reel::ClassifyReels(inputs).activate;
   // A custom character/segment display (no decimal score reels) also drives the
   // panel; decimal scores take priority when both somehow exist.
   const bool charDisp = !decimal && !reel::ClassifyCharDisplay(inputs).empty();
   const bool act = decimal || charDisp;
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
      // No decimal score reels. Some original tables render their score as a custom
      // character/segment display (a bank of single-wheel image-grid reels sharing a
      // font strip). Composite that; otherwise drop the channel so the ScoreView
      // shows the real PinMAME display (solid-state tables whose DispReels are just
      // status flags). The reel image stays a LAST-RESORT source: ScoreView::Select
      // demotes it below any fully-matched real display, so a table that DOES have a
      // PinMAME segment/DMD display (e.g. Ali) is never hijacked by this composite.
      const std::vector<int> cells = reel::ClassifyCharDisplay(inputs);
      if (cells.empty())
      {
         ClearActive();
         return;
      }
      // Rebuild only when a displayed glyph or visibility changes.
      uint64_t csig = 1469598103934665603ull;
      const auto cmix = [&csig](long v) {
         uint64_t x = (uint64_t)(v + 1);
         for (int b = 0; b < 8; ++b) { csig ^= (x & 0xFF); csig *= 1099511628211ull; x >>= 8; }
      };
      for (int idx : cells)
      {
         cmix(reels[idx]->GetCurrentValue());
         cmix(reels[idx]->m_d.m_visible ? 1 : 0);
      }
      if (m_haveImage && m_haveSig && csig == m_lastSig)
         return;
      if (!CompositeCharDisplay(reels, cells))
      {
         ClearActive();
         return;
      }
      m_lastSig = csig;
      m_haveSig = true;
      return;
   }

   // Change-signature over everything we will display (every score/overflow/credit/
   // ball reel's value + visibility), so we only rebuild when something shown changes.
   uint64_t sig = 1469598103934665603ull; // FNV-1a basis
   const auto mix = [&sig](long v) {
      uint64_t x = (uint64_t)(v + 1); // shift past -1 sentinels
      for (int b = 0; b < 8; ++b) { sig ^= (x & 0xFF); sig *= 1099511628211ull; x >>= 8; }
   };
   const auto mixReel = [&](int idx) {
      if (idx < 0 || idx >= (int)reels.size()) { mix(-1); return; }
      mix(reels[idx]->GetCurrentValue());
      mix(reels[idx]->m_d.m_visible ? 1 : 0); // lit/dim visibility swap changes what is shown
   };
   for (int idx : plan.scoreReels)    mixReel(idx);
   for (int idx : plan.overflowReels) mixReel(idx);
   for (int idx : plan.creditReels)   mixReel(idx);
   for (int idx : plan.bipReels)      mixReel(idx);
   for (int idx : plan.playerUpReels) mixReel(idx); // rebuild when the active player changes

   if (m_haveImage && m_haveSig && sig == m_lastSig)
      return; // nothing displayed changed, skip rebuild

   const bool built = CompositeUnified(reels, plan);
   if (!built)
   {
      // No usable strip art -> drop the channel rather than show stale/blank.
      static int s_failLog = 12; // throttle: LoadStrip logs carry the real reason
      if (s_failLog > 0) { --s_failLog; PLOGW << "[ReelDmd] build produced no image (scoreReels="
            << plan.scoreReels.size() << ")"; }
      ClearActive();
      return;
   }

   m_lastSig = sig;
   m_haveSig = true;
}

void ReelDmd::FillSurroundPanel(int w, int h)
{
   if (w <= 0 || h <= 0 || m_scratch.size() < (size_t)w * h * 4)
      return;
   // Vertical gradient at a semi-transparent alpha (top lighter, base a touch
   // darker and more opaque).
   for (int y = 0; y < h; ++y)
   {
      const float t = (h > 1) ? (float)y / (float)(h - 1) : 0.0f;
      const uint8_t r = (uint8_t)(kPanelTopR + ((int)kPanelBotR - (int)kPanelTopR) * t + 0.5f);
      const uint8_t g = (uint8_t)(kPanelTopG + ((int)kPanelBotG - (int)kPanelTopG) * t + 0.5f);
      const uint8_t b = (uint8_t)(kPanelTopB + ((int)kPanelBotB - (int)kPanelTopB) * t + 0.5f);
      const uint8_t a = (uint8_t)(kPanelTopA + ((int)kPanelBotA - (int)kPanelTopA) * t + 0.5f);
      uint8_t* const row = m_scratch.data() + (size_t)y * w * 4;
      for (int x = 0; x < w; ++x) { row[x * 4] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = a; }
   }
   // Raised 2px beveled metal frame: bright highlight on the top/left edges, dark
   // shadow on the bottom/right, so the panel reads as a raised piece of hardware.
   const auto edge = [this, w, h](int x, int y, int dRGB) {
      if (x < 0 || x >= w || y < 0 || y >= h)
         return;
      uint8_t* const p = m_scratch.data() + ((size_t)y * w + x) * 4;
      for (int c = 0; c < 3; ++c) { const int v = (int)p[c] + dRGB; p[c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
      p[3] = 255;
   };
   for (int x = 0; x < w; ++x) { edge(x, 0, +46); edge(x, 1, +24); edge(x, h - 2, -16); edge(x, h - 1, -30); }
   for (int y = 0; y < h; ++y) { edge(0, y, +46); edge(1, y, +24); edge(w - 2, y, -16); edge(w - 1, y, -30); }
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

// Draw one digit cell (cellIndex into the strip) stretched to FILL dstCellW x
// dstCellH at (boxX, boxY), plus the recessed-window border. Stretching matches
// the engine (DispReel maps the whole source cell onto a quad sized from the
// reel's authored width:height), so the on-glass digit aspect is the AUTHORED
// aspect, not the strip image's native cell aspect.
void ReelDmd::DrawDigitCell(const ReelStrip& strip, int cellIndex, int boxX, int boxY, int cellW, int cellH)
{
   if (!strip.ok || cellW <= 0 || cellH <= 0)
      return;
   const int srcCW = strip.cellW, srcCH = strip.cellH;
   const int gc = (strip.gridCols > 0) ? (cellIndex % strip.gridCols) : 0;
   const int gr = (strip.gridCols > 0) ? (cellIndex / strip.gridCols) : 0;
   const int srcCellX = gc * strip.cellW, srcCellY = gr * strip.cellH;

   for (int y = 0; y < cellH; ++y)
   {
      const int dy = boxY + y;
      if (dy < 0 || dy >= m_outH) continue;
      const int sy = srcCellY + (cellH > 1 ? (y * (srcCH - 1)) / (cellH - 1) : 0);
      if (sy < 0 || sy >= strip.imgH) continue;
      const uint8_t* const srow = strip.src + (size_t)sy * strip.srcPitch;
      for (int x = 0; x < cellW; ++x)
      {
         const int dx = boxX + x;
         if (dx < 0 || dx >= m_outW) continue;
         const int sx = srcCellX + (cellW > 1 ? (x * (srcCW - 1)) / (cellW - 1) : 0);
         if (sx < 0 || sx >= strip.imgW) continue;
         const uint8_t* const sp = srow + (size_t)sx * 3;
         uint8_t* const dp = m_scratch.data() + ((size_t)dy * m_outW + dx) * 4;
         dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
         dp[3] = 255; // digits are fully opaque (the surround carries the alpha)
      }
   }

   // Recessed-window edge: top/left darkened (shadow), bottom/right lightened.
   const auto shade = [this](int x, int y, int delta) {
      if (x < 0 || x >= m_outW || y < 0 || y >= m_outH) return;
      uint8_t* const p = m_scratch.data() + ((size_t)y * m_outW + x) * 4;
      for (int c = 0; c < 3; ++c) { const int v = (int)p[c] + delta; p[c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
      p[3] = 255;
   };
   const int x0 = boxX, y0 = boxY, x1 = boxX + cellW - 1, y1 = boxY + cellH - 1;
   for (int x = x0; x <= x1; ++x) { shade(x, y0, -66); shade(x, y1, +32); }
   for (int y = y0; y <= y1; ++y) { shade(x0, y, -66); shade(x1, y, +32); }
}

// Blit `count` of the reel's current digit cells (starting at reel index
// startReel), packed left to right from (dstX, dstY). Returns the dest x past the
// last cell. (Reads the reel's own digits; used for the real score reels.)
int ReelDmd::BlitReelCells(const ReelStrip& strip, DispReel* reel, int startReel, int count,
                           int dstX, int dstY, int dstCellW, int dstCellH)
{
   if (!strip.ok || reel == nullptr || dstCellW <= 0 || dstCellH <= 0)
      return dstX;
   const int dr = (reel->m_d.m_digitrange > 0) ? reel->m_d.m_digitrange : 0;
   for (int i = 0; i < count; ++i)
   {
      const int digit = reel->GetReelDigit(startReel + i);
      const int cellIndex = (digit >= 0 && digit <= dr) ? digit : 0;
      DrawDigitCell(strip, cellIndex, dstX + i * dstCellW, dstY, dstCellW, dstCellH);
   }
   return dstX + count * dstCellW;
}

// Render `value` as `count` right-aligned digits using `strip`'s 0-9 cells, so an
// aux value (ball / credit / overflow carry) renders in the clean score font
// instead of the table's backglass aux graphic.
void ReelDmd::BlitValue(const ReelStrip& strip, long value, int count, int dstX, int dstY, int cellW, int cellH)
{
   if (!strip.ok || value < 0 || count <= 0)
      return;
   long pow10 = 1;
   for (int i = 1; i < count; ++i) pow10 *= 10;
   for (int i = 0; i < count; ++i)
   {
      const int digit = (int)((value / pow10) % 10);
      DrawDigitCell(strip, digit, dstX + i * cellW, dstY, cellW, cellH);
      pow10 /= (pow10 > 1) ? 10 : 1;
   }
}

// Multiply the RGB of pixels in the rect toward black by pct% (dim inactive rows).
void ReelDmd::DimRect(int x, int y, int w, int h, int pct)
{
   const int keep = std::max(0, 100 - pct);
   for (int yy = y; yy < y + h; ++yy)
   {
      if (yy < 0 || yy >= m_outH) continue;
      for (int xx = x; xx < x + w; ++xx)
      {
         if (xx < 0 || xx >= m_outW) continue;
         uint8_t* const p = m_scratch.data() + ((size_t)yy * m_outW + xx) * 4;
         p[0] = (uint8_t)((int)p[0] * keep / 100);
         p[1] = (uint8_t)((int)p[1] * keep / 100);
         p[2] = (uint8_t)((int)p[2] * keep / 100);
      }
   }
}

// Soft glow frame in (r,g,b) around the rect: bright inner edge + outward falloff.
void ReelDmd::DrawGlow(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
   const int thick = std::max(2, h / 12);
   const auto blend = [this, r, g, b](int px, int py, float a) {
      if (px < 0 || px >= m_outW || py < 0 || py >= m_outH || a <= 0.f) return;
      if (a > 1.f) a = 1.f;
      uint8_t* const p = m_scratch.data() + ((size_t)py * m_outW + px) * 4;
      p[0] = (uint8_t)(p[0] * (1.f - a) + r * a);
      p[1] = (uint8_t)(p[1] * (1.f - a) + g * a);
      p[2] = (uint8_t)(p[2] * (1.f - a) + b * a);
      const int av = (int)p[3] + (int)(a * 200.f); p[3] = (uint8_t)(av > 255 ? 255 : av);
   };
   // Outward falloff rings around the rect border.
   for (int t = 1; t <= thick; ++t)
   {
      const float a = 0.55f * (1.f - (float)(t - 1) / (float)thick);
      for (int xx = x - t; xx <= x + w - 1 + t; ++xx) { blend(xx, y - t, a); blend(xx, y + h - 1 + t, a); }
      for (int yy = y - t; yy <= y + h - 1 + t; ++yy) { blend(x - t, yy, a); blend(x + w - 1 + t, yy, a); }
   }
   // Bright inner 1px edge.
   for (int xx = x; xx < x + w; ++xx) { blend(xx, y, 0.8f); blend(xx, y + h - 1, 0.8f); }
   for (int yy = y; yy < y + h; ++yy) { blend(x, yy, 0.8f); blend(x + w - 1, yy, 0.8f); }
}

// Procedural indicator icon centred at (cx,cy): metallic pinball or silver coin.
void ReelDmd::DrawIconDisc(int cx, int cy, int radius, bool coin)
{
   if (radius <= 0) return;
   const int r2 = radius * radius;
   for (int yy = -radius; yy <= radius; ++yy)
   {
      const int py = cy + yy;
      if (py < 0 || py >= m_outH) continue;
      for (int xx = -radius; xx <= radius; ++xx)
      {
         const int d2 = xx * xx + yy * yy;
         if (d2 > r2) continue;
         const int px = cx + xx;
         if (px < 0 || px >= m_outW) continue;
         // Radial shade: bright highlight toward top-left, darker toward edge.
         const float nd = (float)d2 / (float)r2;             // 0 center -> 1 edge
         const float hl = (float)(-xx - yy) / (float)(2 * radius) + 0.5f; // top-left bias
         int base = coin ? 150 : 205;
         int v = (int)(base * (1.10f - 0.55f * nd) * (0.78f + 0.32f * hl));
         v = v < 30 ? 30 : (v > 255 ? 255 : v);
         uint8_t cr = (uint8_t)v, cg = (uint8_t)v, cb = (uint8_t)v;
         if (coin) { cb = (uint8_t)(v * 0.92f); } // faint cool tint so it reads as a coin
         // Edge ring darkening for both; coin gets a thin inner rim.
         if (nd > 0.82f) { cr = (uint8_t)(cr * 0.5f); cg = (uint8_t)(cg * 0.5f); cb = (uint8_t)(cb * 0.5f); }
         else if (coin && nd > 0.60f && nd < 0.72f) { cr = (uint8_t)(cr * 0.7f); cg = (uint8_t)(cg * 0.7f); cb = (uint8_t)(cb * 0.7f); }
         uint8_t* const p = m_scratch.data() + ((size_t)py * m_outW + px) * 4;
         p[0] = cr; p[1] = cg; p[2] = cb; p[3] = 255;
      }
   }
}

// Rasterize a label string with the bundled era serif (TeX Gyre Bonum) via SDL_ttf
// once per (text, height), caching the glyph-coverage (alpha) bitmap. The font is
// opened lazily and kept for the session (intentionally not closed - avoids any
// SDL_ttf teardown-order issues; the process is killed on table close).
const ReelDmd::LabelBitmap* ReelDmd::GetLabelBitmap(const char* s, int glyphH) const
{
#ifdef __STANDALONE__
   if (s == nullptr) return nullptr;
   if (glyphH < 6) glyphH = 6;
   const std::string key = std::string(s) + ":" + std::to_string(glyphH);
   const auto it = m_labelCache.find(key);
   if (it != m_labelCache.end())
      return &it->second;

   if (!m_labelFontTried)
   {
      m_labelFontTried = true;
      TTF_Init(); // refcounted / idempotent (the engine may already have inited it)
      if (g_pvp != nullptr)
      {
         const std::string path = g_pvp->m_myPath + "assets" + PATH_SEPARATOR_CHAR + "TeXGyreBonum-Regular.otf";
         m_labelFont = TTF_OpenFont(path.c_str(), (float)glyphH);
         if (m_labelFont == nullptr)
            PLOGW << "[ReelDmd] label font load failed: " << path;
      }
   }

   LabelBitmap lb;
   TTF_Font* const font = static_cast<TTF_Font*>(m_labelFont);
   if (font != nullptr)
   {
      TTF_SetFontSize(font, (float)glyphH);
      const SDL_Color white = { 255, 255, 255, 255 };
      SDL_Surface* const surf = TTF_RenderText_Blended(font, s, strlen(s), white);
      if (surf != nullptr)
      {
         SDL_Surface* const rgba = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
         SDL_DestroySurface(surf);
         if (rgba != nullptr)
         {
            lb.w = rgba->w;
            lb.h = rgba->h;
            lb.cov.resize((size_t)lb.w * lb.h, 0);
            const uint8_t* const px = static_cast<const uint8_t*>(rgba->pixels);
            for (int yy = 0; yy < lb.h; ++yy)
               for (int xx = 0; xx < lb.w; ++xx)
                  lb.cov[(size_t)yy * lb.w + xx] = px[(size_t)yy * rgba->pitch + xx * 4 + 3]; // alpha = coverage
            SDL_DestroySurface(rgba);
         }
      }
   }
   const auto res = m_labelCache.emplace(key, std::move(lb));
   return &res.first->second;
#else
   (void)s; (void)glyphH;
   return nullptr;
#endif
}

int ReelDmd::TextWidth(const char* s, int glyphH) const
{
   const LabelBitmap* const lb = GetLabelBitmap(s, glyphH);
   return (lb != nullptr) ? lb->w : 0;
}

// Blit the cached label coverage, tinting with (r,g,b); `y` is the vertical CENTRE
// of the row (the rasterized bitmap is centred on it). Returns the width drawn.
int ReelDmd::DrawLabel(const char* s, int x, int y, int glyphH, uint8_t r, uint8_t g, uint8_t b)
{
   const LabelBitmap* const lb = GetLabelBitmap(s, glyphH);
   if (lb == nullptr || lb->w <= 0 || lb->h <= 0)
      return 0;
   const int top = y - lb->h / 2;
   for (int yy = 0; yy < lb->h; ++yy)
   {
      const int dy = top + yy;
      if (dy < 0 || dy >= m_outH) continue;
      for (int xx = 0; xx < lb->w; ++xx)
      {
         const int a = lb->cov[(size_t)yy * lb->w + xx];
         if (a <= 0) continue;
         const int dx = x + xx;
         if (dx < 0 || dx >= m_outW) continue;
         uint8_t* const p = m_scratch.data() + ((size_t)dy * m_outW + dx) * 4;
         p[0] = (uint8_t)((p[0] * (255 - a) + (int)r * a) / 255);
         p[1] = (uint8_t)((p[1] * (255 - a) + (int)g * a) / 255);
         p[2] = (uint8_t)((p[2] * (255 - a) + (int)b * a) / 255);
         const int av = (int)p[3] + a; p[3] = (uint8_t)(av > 255 ? 255 : av);
      }
   }
   return lb->w;
}

bool ReelDmd::CompositeCharDisplay(const std::vector<DispReel*>& reels, const std::vector<int>& cells)
{
   if (cells.empty())
      return false;

   // Authored-position rects (one wheel each). PlaceReels culls off-canvas HUD
   // duplicates and returns the bounding box of the on-glass cells.
   std::vector<reel::ReelRect> rects;
   rects.reserve(cells.size());
   for (int j = 0; j < (int)cells.size(); ++j)
   {
      DispReel* const r = reels[cells[j]];
      reel::ReelRect rect;
      rect.index = j;
      rect.x = r->m_d.m_v1.x;
      rect.y = r->m_d.m_v1.y;
      rect.w = (r->m_d.m_width > 0.0f) ? r->m_d.m_width : 1.0f;
      rect.h = (r->m_d.m_height > 0.0f) ? r->m_d.m_height : 1.0f;
      rect.visible = r->m_d.m_visible;
      rects.push_back(rect);
   }

   constexpr float kMargin = 150.0f;
   reel::ReelLayout layout = reel::PlaceReels(rects, (float)EDITOR_BG_WIDTH, (float)EDITOR_BG_HEIGHT, kMargin, /*requireVisible*/ false);
   if (layout.placed.empty() || layout.w <= 0.0f || layout.h <= 0.0f)
      return false;

   // Output sized to the authored display's aspect, width capped so the panel
   // texture stays small. Height follows the authored bounding-box ratio.
   constexpr int kOutW = 512;
   const int outW = kOutW;
   int outH = (int)((layout.h / layout.w) * (float)kOutW + 0.5f);
   if (outH < 1) outH = 1;
   m_outW = outW;
   m_outH = outH;
   m_scratch.assign((size_t)outW * outH * 4, 0);
   FillSurroundPanel(outW, outH);

   const float sx = (float)outW / layout.w;
   const float sy = (float)outH / layout.h;
   bool anyDrawn = false;
   for (const reel::PlacedReel& pr : layout.placed)
   {
      DispReel* const r = reels[cells[pr.index]];
      ReelStrip strip;
      if (!LoadStrip(r, strip))
         continue;
      const int dstX = (int)((pr.x - layout.x) * sx + 0.5f);
      const int dstY = (int)((pr.y - layout.y) * sy + 0.5f);
      int dstW = (int)(pr.w * sx + 0.5f);
      int dstH = (int)(pr.h * sy + 0.5f);
      if (dstW < 1) dstW = 1;
      if (dstH < 1) dstH = 1;
      // One wheel, one glyph: BlitReelCells draws this reel's current cell index.
      BlitReelCells(strip, r, /*startReel*/ 0, /*count*/ 1, dstX, dstY, dstW, dstH);
      anyDrawn = true;
   }
   if (!anyDrawn)
      return false;

   SetReelImage(outW, outH, m_scratch.data());
   m_haveImage = true;
   return true;
}

// The unified composite. Every table renders here: player score reels in a
// position-ranked grid (4 players -> 2x2, single score -> 1x1), the active
// player's row highlighted, plus our own clean indicators for the non-score
// state: a separate lit overflow (100K) lamp pip, and a status row with the
// ball-in-play number and credit - each drawn in the score font with a
// procedural icon, NOT the table's backglass aux graphics (which are graphics
// meant for a real backglass and look like disembodied chunks out of context).
// Positions/sizes are in editor-canvas space.
bool ReelDmd::CompositeUnified(const std::vector<DispReel*>& reels, const reel::ReelPlan& plan)
{
   if (plan.scoreReels.empty())
      return false;

   constexpr float kMargin = 150.0f;

   // DIAGNOSTIC: dump every score reel's name / current value / visibility, so we
   // can see which reel actually carries the live score (and whether it changes).
   {
      std::string sv;
      for (int idx : plan.scoreReels)
      {
         DispReel* const r = reels[idx];
         sv += r->GetName() + "=" + std::to_string((long)r->GetCurrentValue())
             + (r->m_d.m_visible ? "(vis) " : "(hid) ");
      }
      PLOGI << "[ReelDmd] scoreVals " << sv;
   }

   // A reel digit's on-glass aspect (width:height) is the AUTHORED reel box, since
   // the engine stretches the source cell to m_width:m_height. Fall back to the
   // strip cell aspect only when a reel carries no authored size.
   const auto authoredAR = [](const DispReel* r, const ReelStrip& s) -> float {
      const float w = r->m_d.m_width, h = r->m_d.m_height;
      if (w > 0.0f && h > 0.0f) return w / h;
      return (s.cellH > 0) ? (float)s.cellW / (float)s.cellH : 0.5f;
   };

   // Some tables (e.g. El Dorado) HIDE the reel that actually carries the score in
   // cabinet view (ShowDT=false hides the desktop reels) while leaving decoy reels
   // visible at 0. Detect that: if no visible score reel has a value but a hidden
   // one does, the table's `visible` flag is unreliable - select reels by VALUE
   // (non-zero) instead. Only triggers on that exact pattern; normal tables, where
   // the score sits on a visible reel, keep visibility-based selection.
   bool anyVisibleNonZero = false, anyHiddenNonZero = false;
   for (int idx : plan.scoreReels)
   {
      const bool nz = reels[idx]->GetCurrentValue() != 0;
      if (reels[idx]->m_d.m_visible) anyVisibleNonZero |= nz;
      else                           anyHiddenNonZero |= nz;
   }
   const bool selectByValue = !anyVisibleNonZero && anyHiddenNonZero;

   // --- 1. Select + grid the score reels (visible-first, fallback all on-canvas) ---
   std::vector<reel::ReelRect> rects;
   rects.reserve(plan.scoreReels.size());
   for (int j = 0; j < (int)plan.scoreReels.size(); ++j)
   {
      DispReel* r = reels[plan.scoreReels[j]];
      const int rc = r->GetReels();
      if (rc <= 0)
         continue;
      const float cw = r->m_d.m_width, ch = r->m_d.m_height;
      const float sp = (r->m_d.m_reelspacing > 0.0f) ? r->m_d.m_reelspacing : 0.0f;
      reel::ReelRect rect;
      rect.index = j;
      rect.x = r->m_d.m_v1.x + sp;
      rect.y = r->m_d.m_v1.y + sp;
      rect.w = (float)rc * (cw + sp);
      rect.h = ch;
      rect.visible = selectByValue ? (r->GetCurrentValue() != 0) : r->m_d.m_visible;
      rects.push_back(rect);
   }
   reel::ReelLayout layout = reel::PlaceReels(rects, (float)EDITOR_BG_WIDTH, (float)EDITOR_BG_HEIGHT, kMargin, /*requireVisible*/ true);
   if (layout.placed.empty())
      layout = reel::PlaceReels(rects, (float)EDITOR_BG_WIDTH, (float)EDITOR_BG_HEIGHT, kMargin, /*requireVisible*/ false);
   if (layout.placed.empty())
   {
      if (plan.primaryScore >= 0 && plan.primaryScore < (int)reels.size())
         return Composite(reels[plan.primaryScore]);
      return false;
   }

   const int nPlaced = (int)layout.placed.size();
   std::vector<ReelStrip> strips(nPlaced);
   std::vector<int> reelCounts(nPlaced, 0);
   std::vector<bool> okReel(nPlaced, false);
   std::vector<DispReel*> placedReel(nPlaced, nullptr);
   float sumH = 0.0f, sumBlockW = 0.0f;
   int okCount = 0, anchor = -1;
   for (int i = 0; i < nPlaced; ++i)
   {
      DispReel* r = reels[plan.scoreReels[layout.placed[i].index]];
      placedReel[i] = r;
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
   const reel::GridLayout grid = reel::AssignGrid(layout.placed, avgH * 0.6f, avgBlockW * 0.6f);
   const int gridCols = std::max(1, grid.cols);
   const int gridRows = std::max(1, grid.rows);

   // Per-reel digit aspect from the authored reel box (not the strip's native cell
   // aspect), so each score's digits render at the proportions the table draws.
   std::vector<float> reelAR(nPlaced, 0.8f);
   for (int i = 0; i < nPlaced; ++i)
      if (okReel[i])
         reelAR[i] = authoredAR(placedReel[i], strips[i]);

   // --- 2. Overflow carry value per score cell. Drawn later as a SEPARATE lit lamp
   //        pip (not folded into the number). Find the nearest same-row overflow reel. ---
   std::vector<long> ovValue(nPlaced, 0); // carry digit to show (0 = no pip for this cell)
   bool anyOverflow = false;
   for (int i = 0; i < nPlaced; ++i)
   {
      if (!okReel[i]) continue;
      const float scx = layout.placed[i].x + 0.5f * layout.placed[i].w;
      const float scy = layout.placed[i].y + 0.5f * layout.placed[i].h;
      int best = -1; float bestD = 1e18f;
      for (int k = 0; k < (int)plan.overflowReels.size(); ++k)
      {
         DispReel* ov = reels[plan.overflowReels[k]];
         const float ow = (float)std::max(1, ov->GetReels()) * (ov->m_d.m_width + ov->m_d.m_reelspacing);
         const float ocx = ov->m_d.m_v1.x + 0.5f * ow;
         const float ocy = ov->m_d.m_v1.y + 0.5f * ov->m_d.m_height;
         const float dyAbs = (ocy > scy) ? (ocy - scy) : (scy - ocy);
         if (dyAbs > avgH) continue;
         const float d = (ocx - scx) * (ocx - scx) + (ocy - scy) * (ocy - scy);
         if (d < bestD) { bestD = d; best = k; }
      }
      if (best >= 0)
      {
         const long v = (long)reels[plan.overflowReels[best]]->GetCurrentValue();
         if (v > 0) { ovValue[i] = v; anyOverflow = true; }
      }
   }

   // --- 3. Aux STATE only (we render our own digits, never the table's aux graphic) ---
   DispReel* const bipReel    = plan.bipReels.empty()    ? nullptr : reels[plan.bipReels[0]];
   DispReel* const creditReel = plan.creditReels.empty() ? nullptr : reels[plan.creditReels[0]];
   const long bipValue  = bipReel ? (long)bipReel->GetCurrentValue() : -1;
   const long credValue = creditReel ? (long)creditReel->GetCurrentValue() : -1;
   const bool haveBall = bipValue >= 0;
   const bool haveCred = credValue >= 0;
   const int credDigits = haveCred ? (credValue >= 100 ? 3 : (credValue >= 10 ? 2 : 1)) : 0;

   // Current player: the single lit PlayerUp lamp, mapped to its nearest score row.
   // Resolve by lamp value first, then by a single visible lamp; if ambiguous or
   // absent, no row is highlighted (graceful - the rest renders unchanged).
   int activeCell = -1;
   {
      int litLamp = -1; bool ambiguous = false;
      for (int idx : plan.playerUpReels)
         if (reels[idx]->GetCurrentValue() > 0) { if (litLamp < 0) litLamp = idx; else ambiguous = true; }
      if (litLamp < 0 && !ambiguous)
         for (int idx : plan.playerUpReels)
            if (reels[idx]->m_d.m_visible) { if (litLamp < 0) litLamp = idx; else ambiguous = true; }
      if (litLamp >= 0 && !ambiguous)
      {
         DispReel* const lamp = reels[litLamp];
         const float lcx = lamp->m_d.m_v1.x + 0.5f * (lamp->m_d.m_width + lamp->m_d.m_reelspacing);
         const float lcy = lamp->m_d.m_v1.y + 0.5f * lamp->m_d.m_height;
         float bestD = 1e18f;
         for (int i = 0; i < nPlaced; ++i)
         {
            if (!okReel[i]) continue;
            const float cx = layout.placed[i].x + 0.5f * layout.placed[i].w;
            const float cy = layout.placed[i].y + 0.5f * layout.placed[i].h;
            const float d = (cx - lcx) * (cx - lcx) + (cy - lcy) * (cy - lcy);
            if (d < bestD) { bestD = d; activeCell = i; }
         }
      }
   }

   // Active-scorer fallback: when no PlayerUp lamp resolves (e.g. player-up gated on
   // ShowDT, which is false in cabinet view), highlight the placed score reel whose
   // value most recently increased - that player is "up". Reset when all are zero.
   {
      bool allZero = true;
      for (int i = 0; i < nPlaced; ++i)
      {
         if (!okReel[i]) continue;
         const long v = (long)placedReel[i]->GetCurrentValue();
         if (v != 0) allZero = false;
         auto it = m_prevScoreVal.find(placedReel[i]);
         if (it != m_prevScoreVal.end() && v > it->second) m_activeScorer = placedReel[i];
         m_prevScoreVal[placedReel[i]] = v;
      }
      if (allZero) m_activeScorer = nullptr; // new game / attract
   }
   if (activeCell < 0)
   {
      DispReel* want = m_activeScorer;
      if (want == nullptr && plan.primaryScore >= 0 && plan.primaryScore < (int)reels.size())
         want = reels[plan.primaryScore]; // default before any scoring (typically player 1)
      for (int i = 0; want != nullptr && i < nPlaced; ++i)
         if (okReel[i] && placedReel[i] == want) { activeCell = i; break; }
   }

   // --- 4. Resolve pixel geometry (cellH uniform; per-reel digit widths). The grid
   //        reserves a left slot for the overflow pip; a status row holds ball + credit. ---
   int cellH = 120, gapX = 2, gapY = 2, border = 2, outW = 1, outH = 1;
   int smallCellH = 1, statusRowH = 0, statDigitW = 1, pipW = 0, pipGap = 0;
   std::vector<int> cw(nPlaced, 1);
   std::vector<int> colW;
   for (int attempt = 0; attempt < 12; ++attempt)
   {
      for (int i = 0; i < nPlaced; ++i)
         if (okReel[i]) cw[i] = std::max(1, (int)(cellH * reelAR[i] + 0.5f));
      const int cellWref = std::max(1, cw[anchor]);
      gapX = std::max(2, cellWref / 3);
      gapY = std::max(2, cellH / 4);
      border = std::max(3, cellH / 6);
      pipW = anyOverflow ? cw[anchor] : 0;          // overflow lamp pip width
      pipGap = anyOverflow ? gapX : 0;
      const int lead = pipW + pipGap;               // reserved left slot per row (blank if no pip)
      colW.assign(gridCols, 0);
      for (int i = 0; i < nPlaced; ++i)
         if (okReel[i])
         {
            const int bw = lead + reelCounts[i] * cw[i];
            if (bw > colW[grid.pos[i].col]) colW[grid.pos[i].col] = bw;
         }
      int gridW = (gridCols - 1) * gapX;
      for (int w : colW) gridW += w;
      const int gridH = gridRows * cellH + (gridRows - 1) * gapY;

      smallCellH = std::max(1, (cellH * 7) / 10);
      statDigitW = std::max(1, (int)(smallCellH * reelAR[anchor] + 0.5f));
      const int labelH = std::max(7, (smallCellH * 55) / 100);
      const int lblGap = std::max(2, statDigitW / 4);
      const int midGap = statDigitW * 2;
      const int ballW = haveBall ? (TextWidth("BALL", labelH) + lblGap + statDigitW) : 0;
      const int credW = haveCred ? (TextWidth("CREDIT", labelH) + lblGap + credDigits * statDigitW) : 0;
      const int statusW = ballW + credW + ((haveBall && haveCred) ? midGap : 0);
      statusRowH = (haveBall || haveCred) ? smallCellH : 0;
      const int rowGap = (statusRowH > 0) ? gapY : 0;

      outW = std::max(gridW, statusW) + 2 * border;
      outH = gridH + rowGap + statusRowH + 2 * border;
      if (outW <= kMaxImageWidth) break;
      cellH = std::max(8, (cellH * kMaxImageWidth) / outW);
   }

   // --- 5. Allocate + surround fill ---
   m_outW = outW; m_outH = outH;
   m_scratch.assign((size_t)outW * outH * 4, 0);
   FillSurroundPanel(outW, outH);

   int gridW = (gridCols - 1) * gapX;
   for (int w : colW) gridW += w;
   const int gridH = gridRows * cellH + (gridRows - 1) * gapY;
   const int lead = pipW + pipGap;
   std::vector<int> colX(gridCols, 0);
   colX[0] = border + std::max(0, (outW - 2 * border - gridW) / 2);
   for (int c = 1; c < gridCols; ++c)
      colX[c] = colX[c - 1] + colW[c - 1] + gapX;

   // --- 6. Score cells + overflow lamp pip, then current-player highlight/dim ---
   for (int i = 0; i < nPlaced; ++i)
   {
      if (!okReel[i]) continue;
      const int col = grid.pos[i].col, row = grid.pos[i].row;
      const int dgw = cw[i];
      const int rowContentW = lead + reelCounts[i] * dgw;
      int dx = colX[col] + (colW[col] - rowContentW); // right-align score within its column
      const int dy = border + row * (cellH + gapY);
      if (anyOverflow)
      {
         if (ovValue[i] > 0)
         {
            BlitValue(strips[anchor], ovValue[i], 1, dx, dy, pipW, cellH); // carry in the score font
            DrawGlow(dx, dy, pipW, cellH, kAccentR, kAccentG, kAccentB);   // separate "lit lamp" glow
         }
         dx += lead; // advance past the pip slot (blank when this player hasn't rolled over)
      }
      BlitReelCells(strips[i], placedReel[i], 0, reelCounts[i], dx, dy, dgw, cellH);
   }
   if (activeCell >= 0) // current player resolved -> glow active row, dim the others
   {
      for (int i = 0; i < nPlaced; ++i)
      {
         if (!okReel[i]) continue;
         const int rx = colX[grid.pos[i].col], rw = colW[grid.pos[i].col];
         const int dy = border + grid.pos[i].row * (cellH + gapY);
         if (i == activeCell) DrawGlow(rx, dy, rw, cellH, kAccentR, kAccentG, kAccentB);
         else                 DimRect(rx, dy, rw, cellH, 40);
      }
   }

   // --- 7. Status row: "BALL" + lit digit, and "CREDIT" + number (small text font) ---
   if (statusRowH > 0)
   {
      const int labelH = std::max(7, (smallCellH * 55) / 100);
      const int lblGap = std::max(2, statDigitW / 4);
      const int midGap = statDigitW * 2;
      const int ballW = haveBall ? (TextWidth("BALL", labelH) + lblGap + statDigitW) : 0;
      const int credW = haveCred ? (TextWidth("CREDIT", labelH) + lblGap + credDigits * statDigitW) : 0;
      const int statusW = ballW + credW + ((haveBall && haveCred) ? midGap : 0);
      const int rowY = border + gridH + gapY;
      const int labelY = rowY + smallCellH / 2; // DrawLabel centres the text on this y
      constexpr uint8_t lR = 158, lG = 162, lB = 170;                   // muted label grey
      int sx = border + std::max(0, (outW - 2 * border - statusW) / 2);
      if (haveBall)
      {
         sx += DrawLabel("BALL", sx, labelY, labelH, lR, lG, lB);
         sx += lblGap;
         DrawGlow(sx, rowY, statDigitW, smallCellH, kAccentR, kAccentG, kAccentB);
         BlitValue(strips[anchor], bipValue, 1, sx, rowY, statDigitW, smallCellH);
         sx += statDigitW;
         if (haveCred) sx += midGap;
      }
      if (haveCred)
      {
         sx += DrawLabel("CREDIT", sx, labelY, labelH, lR, lG, lB);
         sx += lblGap;
         BlitValue(strips[anchor], credValue, credDigits, sx, rowY, statDigitW, smallCellH);
      }
   }

   PLOGI << "[ReelDmd] unified placed=" << nPlaced << " grid=" << gridRows << "x" << gridCols
         << " overflow=" << (anyOverflow ? 1 : 0) << " ball=" << bipValue << " credit=" << credValue
         << " playerUpLamps=" << plan.playerUpReels.size() << " active=" << activeCell
         << " cell=" << cw[anchor] << "x" << cellH << " image=" << outW << "x" << outH;
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

   // Digit aspect from the authored reel box (engine stretches the source cell to
   // m_width:m_height); strip cell aspect only as a fallback.
   const float w = reel->m_d.m_width, h = reel->m_d.m_height;
   const float ar = (w > 0.0f && h > 0.0f) ? (w / h)
                  : ((strip.cellH > 0) ? (float)strip.cellW / (float)strip.cellH : 0.5f);

   int down = 1;
   while ((reelCount * strip.cellW) / down > kMaxImageWidth)
      ++down;

   const int outCellH = std::max(1, strip.cellH / down);
   int outCellW = std::max(1, (int)(outCellH * ar + 0.5f));
   if (reelCount * outCellW > kMaxImageWidth) // keep the upload within budget
      outCellW = std::max(1, kMaxImageWidth / std::max(1, reelCount));

   const int outW = reelCount * outCellW;
   const int outH = outCellH;
   m_outW = outW;
   m_outH = outH;
   m_scratch.assign((size_t)outW * outH * 4, 0);
   FillSurroundPanel(outW, outH);

   BlitReelCells(strip, reel, 0, reelCount, 0, 0, outCellW, outCellH);

   SetReelImage(outW, outH, m_scratch.data());
   m_haveImage = true;
   return true;
}
