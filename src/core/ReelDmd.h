// license:GPLv3+
#pragma once
#include <memory>
class Player;

// Composites a table's electro-mechanical score reels into the player's default
// DMD frame (g_pplayer->m_dmdFrame) so the ScoreView displays them. Active only
// for EM reel tables that have DispReel parts and no script-driven DMD.
class ReelDmd
{
public:
   explicit ReelDmd(Player* player);
   bool ShouldActivate() const; // true if table has DispReels and no script DMD
   void Update();               // per-frame; (re)builds m_dmdFrame from reel values when changed
private:
   Player* m_player = nullptr;
   long m_lastValue = -1;       // skip rebuild when unchanged
};
