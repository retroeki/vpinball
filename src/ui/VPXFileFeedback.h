// license:GPLv3+

#pragma once

class VPXFileFeedback
{
   public:
      virtual ~VPXFileFeedback() {}

      virtual void SetProgress(unsigned int progress) { };
      virtual void SetLength(unsigned int length) { };

      // Check if the loading operation should be cancelled.
      // Implementations can override this to support cancellation.
      virtual bool IsCancelled() { return false; }
};
