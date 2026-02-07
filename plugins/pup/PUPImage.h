// license:GPLv3+

#pragma once

#include "common.h"

namespace PUP {

class PUPImage
{
public:
   PUPImage();
   ~PUPImage();

   const string& GetFile() const { return m_file; }
   bool GetDimensions(int& width, int& height) const;
   bool GetTransparentRegion(float& x, float& y, float& w, float& h) const;

   void Load(const string& szFile);
   void Render(VPXRenderContext2D* const ctx, const SDL_Rect& rect);

private:
   void AnalyzeTransparency();

   string m_file;
   std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> m_pSurface;
   VPXTexture m_pTexture = nullptr;

   struct { float x = 0, y = 0, w = 0, h = 0; bool valid = false; } m_transparentRegion;
};

}
