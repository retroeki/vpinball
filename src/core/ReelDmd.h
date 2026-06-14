// license:GPLv3+
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
class Player;
class DispReel;
class BaseTexture;

// Composites a table's electro-mechanical score reels into a smooth, full-color
// image of the real reel-strip artwork (e.g. gottlieb_reel digits) and hands it
// to the ScoreView plugin via the host SetReelImage channel. The ScoreView's
// Image visual then draws it as an alpha-blended textured quad (NOT dots).
// Active only for EM tables that expose at least one numeric score reel; the
// decision (and which reels are the score) is made by ReelClassifier, not by
// name-matching here. See ReelClassifier.h for the rule.
//
// For tables that follow the Gottlieb 4-player naming convention (ScoreReel1..4,
// Reel100K1..4, BIPReel, Credittxt) it builds the curated 2x2 "backglass" composite:
// all four player scores in a 2x2 grid (each folding in its 100K overflow digit)
// plus a centered ball-in-play / credit row underneath. Every other EM table (any
// reel naming) is rendered faithfully by position: each visible score reel is
// placed at its real backglass location (CompositeByPosition), so a multi-player
// table shows every player's score in its true arrangement, not just one reel.
class ReelDmd
{
public:
   explicit ReelDmd(Player* player);
   bool ShouldActivate() const; // true if the table has >=1 numeric score reel (ReelClassifier)
   void Update();               // per-frame; rebuilds + pushes the reel image when a displayed value changes
private:
   // Identified reels (by name) for the 4-player Gottlieb backglass layout.
   struct NamedReels
   {
      DispReel* score[4] = { nullptr, nullptr, nullptr, nullptr };  // ScoreReel1..4
      DispReel* k100[4] = { nullptr, nullptr, nullptr, nullptr };   // Reel100K1..4
      DispReel* bip = nullptr;                                      // BIPReel
      DispReel* credit = nullptr;                                   // Credittxt
      bool HasAnyScore() const { return score[0] || score[1] || score[2] || score[3]; }
   };

   // Normalized strip for a single reel: tightly-packed 3-byte sRGB, plus the
   // source cell geometry (matching DispReel::RenderSetup). Held across the
   // BlitReelDigits calls of one composite so we Convert each strip once.
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

   // Blit `count` of the reel's current digit cells (starting at reel index
   // `startReel`) into m_scratch, scaled to dstCellW x dstCellH, packed left to
   // right starting at pixel (dstX, dstY). m_scratch must already be sized to
   // m_outW x m_outH. Returns the destination x just past the last cell drawn.
   int BlitReelCells(const ReelStrip& strip, DispReel* reel, int startReel, int count,
                     int dstX, int dstY, int dstCellW, int dstCellH);

   // Build the full 2x2 backglass composite from the named reels. Returns false
   // if no usable artwork could be produced (caller then falls back / clears).
   bool CompositeBackglass(const NamedReels& named);

   // Faithful multi-reel composite: place each visible, on-backglass score reel
   // at its real backglass position (normalized by the editor canvas), mirroring
   // the table's EM head - e.g. a 4-player 2x2 with the current player's lit reel
   // and the others dim. Off-canvas HUD duplicates and hidden reels are skipped.
   // Returns false if no reel was usable (caller falls back to Composite).
   bool CompositeByPosition(const std::vector<DispReel*>& scoreReels);

   // Single active-reel fallback: composite one reel's current digits left to
   // right. Returns false if its strip is unusable.
   bool Composite(DispReel* reel);

   // Clear the reel-image channel (no active reel / inactive table).
   void ClearActive();

   Player* m_player = nullptr;
   uint64_t m_lastSig = 0;           // combined signature of all displayed values
   bool m_haveSig = false;           // whether m_lastSig is valid
   bool m_haveImage = false;         // whether the channel currently holds our image
   int m_outW = 0, m_outH = 0;       // current scratch dimensions
   std::vector<uint8_t> m_scratch;   // reused composite buffer (w*h*3 sRGB)
   mutable int m_loggedActivate = -1; // diagnostics: last logged ShouldActivate result (-1 = none)
};
