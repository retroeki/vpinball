// license:GPLv3+

#pragma once

#include "core/stdafx.h"
#include "ui/VPXFileFeedback.h"
#include <atomic>


class VPXProgress: public VPXFileFeedback
{
public:
   void ItemHasBeenProcessed(int itemsCount, int totalItems) override;
   void SoundHasBeenProcessed(int soundCount, int totalSounds) override;
   void ImageHasBeenProcessed(int imageCount, int totalImages) override;
   void FontHasBeenProcessed(int fontCount, int totalFonts) override;
   void CollectionHasBeenProcessed(int collectionCount, int totalCollections) override;
   bool IsCancelled() override;

   // Static cancellation flag - can be set from any thread
   static std::atomic<bool> s_cancelled;
   static void SetCancelled(bool cancelled) { s_cancelled = cancelled; }
   static void Reset() { s_cancelled = false; }
};
