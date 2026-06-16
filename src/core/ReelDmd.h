// license:GPLv3+
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "core/ReelClassifier.h"
class Player;
class DispReel;
class BaseTexture;

// Composites a table's electro-mechanical reels into a full-colour image of the
// real reel-strip artwork and hands it to the ScoreView plugin via SetReelImage
// (drawn as an alpha-blended textured quad, NOT dots).
//
// ONE unified path for every table: ReelClassifier assigns each live DispReel a
// role from its geometry/name/image (no per-table special-casing), and the
// composite shows whatever roles the table actually has - player scores in a
// position-ranked grid, the 100K overflow carry digit when a player passes it,
// and a credit / ball-in-play readout row. Solid-state tables (no numeric score
// reels) do not activate, leaving their real PinMAME display in the box.
class ReelDmd
{
public:
   explicit ReelDmd(Player* player);
   bool ShouldActivate() const; // true if the table has >=1 numeric score reel (ReelClassifier)
   void Update();               // per-frame; rebuilds + pushes the reel image when a displayed value changes
private:
   // Normalized strip for a single reel: tightly-packed 3-byte sRGB, plus the
   // source cell geometry (matching DispReel::RenderSetup). Held across the
   // BlitReelCells calls of one composite so we Convert each strip once.
   struct ReelStrip
   {
      std::shared_ptr<const BaseTexture> bmp; // keeps the converted pixels alive
      const uint8_t* src = nullptr;
      int imgW = 0, imgH = 0, srcPitch = 0;
      int gridCols = 0, gridRows = 0;
      int cellW = 0, cellH = 0;
      bool ok = false;
   };

   // Load + normalize a reel's strip image and compute its cell geometry.
   bool LoadStrip(DispReel* reel, ReelStrip& out) const;

   // Draw one digit cell (cellIndex into the strip) into m_scratch, stretched to
   // fill dstCellW x dstCellH at (boxX, boxY), with the recessed-window border.
   void DrawDigitCell(const ReelStrip& strip, int cellIndex, int boxX, int boxY, int cellW, int cellH);

   // Blit `count` of the reel's current digit cells (starting at reel index
   // `startReel`) into m_scratch, scaled to dstCellW x dstCellH, packed left to
   // right starting at pixel (dstX, dstY). m_scratch must already be sized to
   // m_outW x m_outH. Returns the destination x just past the last cell drawn.
   int BlitReelCells(const ReelStrip& strip, DispReel* reel, int startReel, int count,
                     int dstX, int dstY, int dstCellW, int dstCellH);

   // Render `value` as `count` right-aligned digits using `strip`'s digit cells (so
   // an aux value - ball/credit/overflow carry - is drawn in the clean score font,
   // NOT the table's backglass aux graphic). Leading positions show 0.
   void BlitValue(const ReelStrip& strip, long value, int count, int dstX, int dstY, int cellW, int cellH);

   // Multiply the RGB of every pixel in the rect toward black by pct% (dim inactive
   // player rows). Alpha untouched.
   void DimRect(int x, int y, int w, int h, int pct);

   // Soft glow frame in (r,g,b) around the rect: a bright inner edge plus an outward
   // falloff. Used for the active-player row, the overflow lamp pip, the ball bulb.
   void DrawGlow(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);

   // Procedural indicator icon centred at (cx,cy): a metallic pinball (coin=false)
   // or a silver credit coin (coin=true). Opaque.
   void DrawIconDisc(int cx, int cy, int radius, bool coin);

   // Status labels (BALL / CREDIT) rendered with the bundled era serif (TeX Gyre
   // Bonum, an open Bookman) via SDL_ttf - the reel art carries only digits, no
   // letters. Glyph coverage is rasterized once per (text,height) and cached.
   // TextWidth measures; DrawLabel blits `s` top-left at (x,y), height glyphH, in
   // (r,g,b), returning the width drawn. If the font can't load, draws nothing.
   struct LabelBitmap { std::vector<uint8_t> cov; int w = 0, h = 0; };
   const LabelBitmap* GetLabelBitmap(const char* s, int glyphH) const;
   int TextWidth(const char* s, int glyphH) const;
   int DrawLabel(const char* s, int x, int y, int glyphH, uint8_t r, uint8_t g, uint8_t b);

   // The unified composite: player scores in a position-ranked grid (source-aspect
   // digit cells), each cell optionally prefixed by its 100K overflow digit (only
   // when that player has rolled over), plus a smaller credit / ball-in-play row
   // beneath. `reels` are all the live DispReels; `plan` indexes into them by role.
   bool CompositeUnified(const std::vector<DispReel*>& reels, const reel::ReelPlan& plan);

   // Single active-reel fallback: composite one reel's current digits left to
   // right. Returns false if its strip is unusable.
   bool Composite(DispReel* reel);

   // Fill m_scratch (sized w*h*4 RGBA) with the surround panel: a subtle vertical
   // gradient at a semi-transparent alpha plus a 1px bevel frame. Digits are blitted
   // opaque on top; the table shows through the translucent surround on screen.
   void FillSurroundPanel(int w, int h);

   // Clear the reel-image channel (no active reel / inactive table).
   void ClearActive();

   Player* m_player = nullptr;
   uint64_t m_lastSig = 0;           // combined signature of all displayed values
   bool m_haveSig = false;           // whether m_lastSig is valid
   bool m_haveImage = false;         // whether the channel currently holds our image
   int m_outW = 0, m_outH = 0;       // current scratch dimensions
   std::vector<uint8_t> m_scratch;   // reused composite buffer (w*h*4 sRGBA)
   mutable int m_loggedActivate = -1; // diagnostics: last logged ShouldActivate result (-1 = none)

   // Label font (lazy) + rasterized-glyph cache for the BALL / CREDIT labels.
   mutable void* m_labelFont = nullptr; // TTF_Font* (opaque here to keep SDL_ttf out of the header)
   mutable bool m_labelFontTried = false;
   mutable std::map<std::string, LabelBitmap> m_labelCache;

   // Active-scorer heuristic for the current-player highlight when the table gives
   // no readable PlayerUp lamp (e.g. player-up gated on ShowDT, false in cabinet
   // view). Tracks each placed score reel's last value; the one that most recently
   // increased is "up". Reset to none when all scores are zero (new game).
   std::map<DispReel*, long> m_prevScoreVal;
   DispReel* m_activeScorer = nullptr;
};
