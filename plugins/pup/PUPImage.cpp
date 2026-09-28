// license:GPLv3+

#include "PUPImage.h"

#include <SDL3_image/SDL_image.h>
#include <thread>

namespace PUP {

PUPImage::PUPImage()
   : m_pSurface(nullptr, SDL_DestroySurface)
   , m_pendingSurface(nullptr, SDL_DestroySurface)
{
}

PUPImage::~PUPImage()
{
   while (m_loading.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   if (m_pTexture)
      DeleteTexture(m_pTexture);
}

void PUPImage::Clear()
{
   m_file.clear();
   if (m_pTexture) {
      DeleteTexture(m_pTexture);
      m_pTexture = nullptr;
   }
   m_pSurface.reset();
   std::lock_guard lock(m_loadMutex);
   m_pendingSurface.reset();
   m_transparentRegion.valid = false;
}

void PUPImage::Load(const std::filesystem::path& szFile)
{
   m_file = szFile;

   if (m_pTexture) {
      DeleteTexture(m_pTexture);
      m_pTexture = nullptr;
   }
   m_pSurface.reset();
   {
      std::lock_guard lock(m_loadMutex);
      m_transparentRegion.valid = false;
   }

   m_loading.store(true);
   std::thread([this, szFile]() {
      auto surface = std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)>(IMG_Load(szFile.string().c_str()), SDL_DestroySurface);
      if (surface && surface->format != SDL_PIXELFORMAT_RGBA32)
         surface = std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)>(SDL_ConvertSurface(surface.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
      const TransparentRegion region = AnalyzeTransparency(surface.get(), szFile);
      {
         std::lock_guard lock(m_loadMutex);
         m_pendingSurface = std::move(surface);
         m_transparentRegion = region;
      }
      m_loading.store(false);
   }).detach();
}

bool PUPImage::GetDimensions(int& width, int& height) const
{
   if (m_pTexture) {
      VPXTextureInfo* texInfo = GetTextureInfo(m_pTexture);
      width = texInfo->width;
      height = texInfo->height;
      return true;
   }
   if (m_pSurface) {
      width = m_pSurface->w;
      height = m_pSurface->h;
      return true;
   }
   std::lock_guard lock(m_loadMutex);
   if (m_pendingSurface) {
      width = m_pendingSurface->w;
      height = m_pendingSurface->h;
      return true;
   }
   return false;
}

PUPImage::TransparentRegion PUPImage::AnalyzeTransparency(SDL_Surface* surface, const std::filesystem::path& file)
{
   TransparentRegion region;
   if (!surface || surface->format != SDL_PIXELFORMAT_RGBA32)
      return region;

   SDL_LockSurface(surface);
   const int w = surface->w;
   const int h = surface->h;
   const int pitch = surface->pitch / 4; // pitch in pixels (4 bytes per RGBA32 pixel)
   const uint32_t* pixels = static_cast<const uint32_t*>(surface->pixels);

   int minX = w, minY = h, maxX = -1, maxY = -1;

   for (int y = 0; y < h; y++)
   {
      for (int x = 0; x < w; x++)
      {
         const uint8_t alpha = static_cast<uint8_t>(pixels[y * pitch + x] >> 24);
         if (alpha < 128) // Transparent pixel (frame window area)
         {
            if (x < minX) minX = x;
            if (y < minY) minY = y;
            if (x > maxX) maxX = x;
            if (y > maxY) maxY = y;
         }
      }
   }
   SDL_UnlockSurface(surface);

   if (maxX > minX && maxY > minY)
   {
      region.x = static_cast<float>(minX) / static_cast<float>(w);
      region.y = static_cast<float>(minY) / static_cast<float>(h);
      region.w = static_cast<float>(maxX - minX + 1) / static_cast<float>(w);
      region.h = static_cast<float>(maxY - minY + 1) / static_cast<float>(h);
      region.valid = true;
      LOGD_DBG(std::format("PUP FRAME WINDOW: file='{}' transparent region=({:.1f}%, {:.1f}%, {:.1f}%, {:.1f}%) imageSize=({},{})",
         file.string(),
         region.x * 100.f, region.y * 100.f,
         region.w * 100.f, region.h * 100.f,
         w, h));
   }
   return region;
}

bool PUPImage::GetTransparentRegion(float& x, float& y, float& w, float& h) const
{
   std::lock_guard lock(m_loadMutex);
   if (!m_transparentRegion.valid)
      return false;
   x = m_transparentRegion.x;
   y = m_transparentRegion.y;
   w = m_transparentRegion.w;
   h = m_transparentRegion.h;
   return true;
}

void PUPImage::Render(VPXRenderContext2D* const ctx, const SDL_Rect& rect, float alpha)
{
   // Pick up async-loaded surface
   {
      std::lock_guard lock(m_loadMutex);
      if (m_pendingSurface) {
         m_pSurface = std::move(m_pendingSurface);
         if (m_pTexture) {
            DeleteTexture(m_pTexture);
            m_pTexture = nullptr;
         }
      }
   }

   // Update texture
   if (m_pTexture == nullptr && m_pSurface) {
      m_pTexture = CreateTexture(m_pSurface.get());
      m_pSurface = nullptr;
   }

   // Render image
   if (m_pTexture && alpha > 0.f)
   {
      VPXTextureInfo* texInfo = GetTextureInfo(m_pTexture);

      // Clip dest rect to output area to prevent overflow when viewport offsets are applied
      const float dstL = static_cast<float>(rect.x);
      const float dstT = static_cast<float>(rect.y);
      const float dstR = static_cast<float>(rect.x + rect.w);
      const float dstB = static_cast<float>(rect.y + rect.h);

      const float clipL = (dstL < 0.f) ? 0.f : dstL;
      const float clipT = (dstT < 0.f) ? 0.f : dstT;
      const float clipR = (dstR > ctx->srcWidth) ? ctx->srcWidth : dstR;
      const float clipB = (dstB > ctx->srcHeight) ? ctx->srcHeight : dstB;

      if (clipL >= clipR || clipT >= clipB)
         return; // Fully outside output area

      // Adjust texture coordinates for the clipped region
      const float texW = static_cast<float>(texInfo->width);
      const float texH = static_cast<float>(texInfo->height);
      const float invW = 1.f / (dstR - dstL);
      const float invH = 1.f / (dstB - dstT);
      const float clippedTexX = (clipL - dstL) * invW * texW;
      const float clippedTexY = (clipT - dstT) * invH * texH;
      const float clippedTexW = (clipR - clipL) * invW * texW;
      const float clippedTexH = (clipB - clipT) * invH * texH;

      ctx->DrawImage(ctx, m_pTexture, 1.f, 1.f, 1.f, alpha,
         clippedTexX, clippedTexY, clippedTexW, clippedTexH,
         0.f, 0.f, 0.f,
         clipL, clipT, clipR - clipL, clipB - clipT);
   }
}

}
