// license:GPLv3+

#pragma once

#include "Shader.h"

class RenderDevice;
class RenderDeviceState;
class RenderPass;
class RenderCommand;

class RenderFrame final
{
public:
   RenderFrame(RenderDevice* renderDevice);
   ~RenderFrame();

   RenderCommand* NewCommand();

   RenderPass* AddPass(const string& name, RenderTarget* const rt);
   void AddBeginOfFrameCmd(const std::function<void()>& cmd) { m_beginOfFrameCmds.push_back(cmd); }
   void AddEndOfFrameCmd(const std::function<void()>& cmd) { m_endOfFrameCmds.push_back(cmd); }
   // Drop queued begin/end-of-frame deferred commands without executing them. Used when a
   // prepared frame is abandoned (e.g. render-thread park on Android onPause): these lambdas
   // capture engine objects (Ball*, RenderDevice*) by pointer and must not survive to run on a
   // later frame after those objects were freed (see Ball::Render begin-of-frame command).
   void ClearDeferredCommands() { m_beginOfFrameCmds.clear(); m_endOfFrameCmds.clear(); }
   bool Execute(const bool log = false);
   void Discard();

private:
   void SortPasses(RenderPass* finalPass, vector<RenderPass*>& sortedPasses);

   RenderDevice* const m_rd;
   std::unique_ptr<RenderDeviceState> m_rdState = nullptr;
   vector<RenderPass*> m_passes;
   vector<RenderPass*> m_passPool;
   vector<RenderCommand*> m_commandPool;
   vector<std::function<void()>> m_beginOfFrameCmds;
   vector<std::function<void()>> m_endOfFrameCmds;
};
