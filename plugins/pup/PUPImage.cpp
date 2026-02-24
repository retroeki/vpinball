// license:GPLv3+

#include "PUPImage.h"

#include <SDL3_image/SDL_image.h>

namespace PUP {

PUPImage::PUPImage()
   : m_pSurface(nullptr, SDL_DestroySurface)
{
}

PUPImage::~PUPImage()
{
   if (m_pTexture)
      DeleteTexture(m_pTexture);
}

void PUPImage::Load(const string& szFile)
{
   m_file = szFile;
   m_transparentRegion.valid = false;

   if (m_pTexture) {
      DeleteTexture(m_pTexture);
      m_pTexture = nullptr;
   }

   m_pSurface = std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)>(IMG_Load(szFile.c_str()), SDL_DestroySurface);
   if (m_pSurface && m_pSurface->format != SDL_PIXELFORMAT_RGBA32)
      m_pSurface = std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)>(SDL_ConvertSurface(m_pSurface.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);

   AnalyzeTransparency();
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
   return false;
}

void PUPImage::AnalyzeTransparency()
{
   m_transparentRegion.valid = false;
   if (!m_pSurface || m_pSurface->format != SDL_PIXELFORMAT_RGBA32)
      return;

   SDL_LockSurface(m_pSurface.get());
   const int w = m_pSurface->w;
   const int h = m_pSurface->h;
   const int pitch = m_pSurface->pitch / 4; // pitch in pixels (4 bytes per RGBA32 pixel)
   const uint32_t* pixels = static_cast<const uint32_t*>(m_pSurface->pixels);

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
   SDL_UnlockSurface(m_pSurface.get());

   if (maxX > minX && maxY > minY)
   {
      m_transparentRegion.x = static_cast<float>(minX) / static_cast<float>(w);
      m_transparentRegion.y = static_cast<float>(minY) / static_cast<float>(h);
      m_transparentRegion.w = static_cast<float>(maxX - minX + 1) / static_cast<float>(w);
      m_transparentRegion.h = static_cast<float>(maxY - minY + 1) / static_cast<float>(h);
      m_transparentRegion.valid = true;
      LOGI("PUP FRAME WINDOW: file='%s' transparent region=(%.1f%%, %.1f%%, %.1f%%, %.1f%%) imageSize=(%d,%d)",
         m_file.c_str(),
         m_transparentRegion.x * 100.f, m_transparentRegion.y * 100.f,
         m_transparentRegion.w * 100.f, m_transparentRegion.h * 100.f,
         w, h);
   }
}

bool PUPImage::GetTransparentRegion(float& x, float& y, float& w, float& h) const
{
   if (!m_transparentRegion.valid)
      return false;
   x = m_transparentRegion.x;
   y = m_transparentRegion.y;
   w = m_transparentRegion.w;
   h = m_transparentRegion.h;
   return true;
}

void PUPImage::Render(VPXRenderContext2D* const ctx, const SDL_Rect& rect)
{
   // Update texture
   if (m_pTexture == nullptr && m_pSurface) {
      m_pTexture = CreateTexture(m_pSurface.get());
      m_pSurface = nullptr;
   }

   // Render image
   if (m_pTexture)
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

      ctx->DrawImage(ctx, m_pTexture, 1.f, 1.f, 1.f, 1.f,
         clippedTexX, clippedTexY, clippedTexW, clippedTexH,
         0.f, 0.f, 0.f,
         clipL, clipT, clipR - clipL, clipB - clipT);
   }
}

}
