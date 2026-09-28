// license:GPLv3+

#pragma once

#include "common.h"
#include <mutex>
#include <atomic>

namespace PUP {

class PUPImage
{
public:
   PUPImage();
   ~PUPImage();

   const std::filesystem::path& GetFile() const { return m_file; }
   bool GetDimensions(int& width, int& height) const;
   bool GetTransparentRegion(float& x, float& y, float& w, float& h) const;

   void Load(const std::filesystem::path& szFile);
   void Clear();
   void Render(VPXRenderContext2D* const ctx, const SDL_Rect& rect, float alpha = 1.f);

private:
   struct TransparentRegion { float x = 0, y = 0, w = 0, h = 0; bool valid = false; };
   static TransparentRegion AnalyzeTransparency(SDL_Surface* surface, const std::filesystem::path& file);

   std::filesystem::path m_file;
   std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> m_pSurface;
   VPXTexture m_pTexture = nullptr;

   mutable std::mutex m_loadMutex;
   std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> m_pendingSurface;
   std::atomic<bool> m_loading { false };
   TransparentRegion m_transparentRegion; // guarded by m_loadMutex (computed by the async loader)
};

}
