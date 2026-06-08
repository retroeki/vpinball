// license:GPLv3+
#pragma once
#include <cstdint>
#include <vector>
class Player;
class DispReel;

// Composites a table's electro-mechanical score reels into a smooth, full-color
// image of the real reel-strip artwork (e.g. gottlieb_reel digits) and hands it
// to the ScoreView plugin via the host SetReelImage channel. The ScoreView's
// Image visual then draws it as an alpha-blended textured quad (NOT dots).
// Active only for EM reel tables that have DispReel parts.
class ReelDmd
{
public:
   explicit ReelDmd(Player* player);
   bool ShouldActivate() const; // true if table has DispReels
   void Update();               // per-frame; rebuilds + pushes the reel image when the value changes
private:
   // Composite the active reel as real glyphs and push via SetReelImage. Returns
   // false if the reel's strip image is missing/unusable.
   bool Composite(DispReel* reel, long value);
   // Clear the reel-image channel (no active reel / inactive table).
   void ClearActive();

   Player* m_player = nullptr;
   long m_lastValue = -1;            // skip rebuild when unchanged
   bool m_haveImage = false;         // whether the channel currently holds our image
   std::vector<uint8_t> m_scratch;   // reused composite buffer (w*h*3 sRGB)
};
