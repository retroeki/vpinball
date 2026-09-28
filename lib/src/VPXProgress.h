// license:GPLv3+

#pragma once

#include "core/stdafx.h"
#include "ui/VPXFileFeedback.h"
#include <atomic>


class VPXProgress: public VPXFileFeedback
{
public:
   void SetProgress(unsigned int progress) override;
   void SetLength(unsigned int length) override;
   bool IsCancelled() override;

   // Static cancellation flag - can be set from any thread
   static std::atomic<bool> s_cancelled;
   static void SetCancelled(bool cancelled) { s_cancelled = cancelled; }
   static void Reset() { s_cancelled = false; }

private:
   unsigned int m_progress = 0;
   unsigned int m_total = 1;
};
