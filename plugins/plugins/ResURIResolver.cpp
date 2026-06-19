// license:GPLv3+

#include "ResURIResolver.h"

#include "simple-uri-parser/uri_parser.h"

#include <assert.h>
#include <sstream>
#include <charconv>
#include <unordered_set>
#ifdef __ANDROID__
#include <android/log.h>
#endif
// Display-source enumeration diagnostics (OnDisplaySrcChanged logs every DMD/display
// source change). Off by default to keep the logcat clean; set RESURI_DEBUG_LOG to 1
// (or -DRESURI_DEBUG_LOG=1) to trace display-source resolution.
#ifndef RESURI_DEBUG_LOG
#define RESURI_DEBUG_LOG 0
#endif
using std::string;
using namespace std::string_literals;

// Tracks live ResURIResolver instances so static message callbacks can safely
// detect a stale userData pointer before dereferencing it.
//
// The bug this works around lives in MsgPluginManager::UnsubscribeMsg, whose
// signature is `(msgId, callback)` - it has NO userData parameter, so it matches
// subscriptions by callback function pointer only and erases the first match it
// finds. Multiple ResURIResolver instances coexist in a typical session (one in
// Player::m_resURIResolver, plus one each owned by the B2SRenderer, ScoreView,
// and B2SLegacy::Form plugins), and they ALL subscribe with the same static
// callback (OnDisplaySrcChanged et al.) just with different `this` as userData.
// As a result, when one resolver calls Unsubscribe in BeginDestruction, the
// manager may erase a DIFFERENT resolver's subscription, leaving the
// to-be-destroyed resolver's entry in the subscriber list. After that resolver
// is freed, the next BroadcastMsg invokes the static callback with a dangling
// userData pointer - SIGSEGV at NULL+offset inside the callback.
//
// Fixing this in the plugin manager would mean changing the C ABI of the
// MsgPluginAPI struct used by every plugin. Containing it here is cheaper and
// also defuses the same latent bug for any future plugin that creates a
// ResURIResolver.
//
// Threading: MsgPluginManager asserts single-threaded API usage; Subscribe/
// Unsubscribe/Broadcast all run on the API thread, and resolver ctors/dtors
// happen on the same thread because they call those APIs. So no mutex needed.
static std::unordered_set<const ResURIResolver*> s_liveResolvers;

ResURIResolver::ResURIResolver(const MsgPluginAPI &msgAPI, unsigned int endpointId, bool trackDisplays, bool trackSegDisplays, bool trackInputs, bool trackDevices)
   : m_msgAPI(msgAPI)
   , m_endpointId(endpointId)
   , m_getDevSrcMsgId(trackDevices ? m_msgAPI.GetMsgID(CTLPI_NAMESPACE, CTLPI_DEVICE_GET_SRC_MSG) : 0)
   , m_onDevChangedMsgId(trackDevices ? m_msgAPI.GetMsgID(CTLPI_NAMESPACE, CTLPI_DEVICE_ON_SRC_CHG_MSG) : 0)
   , m_getInputSrcMsgId(trackInputs ? m_msgAPI.GetMsgID(CTLPI_NAMESPACE, CTLPI_INPUT_GET_SRC_MSG) : 0)
   , m_onInputChangedMsgId(trackInputs ? m_msgAPI.GetMsgID(CTLPI_NAMESPACE, CTLPI_INPUT_ON_SRC_CHG_MSG) : 0)
   , m_getSegSrcMsgId(trackSegDisplays ? m_msgAPI.GetMsgID(CTLPI_NAMESPACE, CTLPI_SEG_GET_SRC_MSG) : 0)
   , m_onSegChangedMsgId(trackSegDisplays ? m_msgAPI.GetMsgID(CTLPI_NAMESPACE, CTLPI_SEG_ON_SRC_CHG_MSG) : 0)
   , m_getDisplaySrcMsgId(trackDisplays ? m_msgAPI.GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_GET_SRC_MSG) : 0)
   , m_onDisplayChangedMsgId(trackDisplays ? m_msgAPI.GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_ON_SRC_CHG_MSG) : 0)
{
   // Register this instance as live. Paired with s_liveResolvers.erase() in
   // BeginDestruction (which the destructor also calls). See the comment on
   // s_liveResolvers above for why this tracking is necessary.
   s_liveResolvers.insert(this);
   if (trackDisplays)
   {
      m_msgAPI.SubscribeMsg(m_endpointId, m_onDisplayChangedMsgId, OnDisplaySrcChanged, this);
      OnDisplaySrcChanged(m_onDisplayChangedMsgId, this, nullptr);
   }
   if (trackSegDisplays)
   {
      m_msgAPI.SubscribeMsg(m_endpointId, m_onSegChangedMsgId, OnSegSrcChanged, this);
      OnSegSrcChanged(m_onSegChangedMsgId, this, nullptr);
   }
   if (trackInputs)
   {
      m_msgAPI.SubscribeMsg(m_endpointId, m_onInputChangedMsgId, OnInputSrcChanged, this);
      OnInputSrcChanged(m_onInputChangedMsgId, this, nullptr);
   }
   if (trackDevices)
   {
      m_msgAPI.SubscribeMsg(m_endpointId, m_onDevChangedMsgId, OnDevSrcChanged, this);
      OnDevSrcChanged(m_onDevChangedMsgId, this, nullptr);
   }
}

void ResURIResolver::BeginDestruction()
{
   if (m_isDestroyed)
      return;
   m_isDestroyed = true;

   // Remove from live set FIRST so any callback that fires before the unsubscribe
   // completes (or any callback that survives the unsubscribe due to the manager's
   // first-match-by-callback semantics) will see this instance as dead.
   s_liveResolvers.erase(this);

   // Unsubscribe from all messages immediately to prevent callbacks on stale pointers
   if (m_onInputChangedMsgId)
      m_msgAPI.UnsubscribeMsg(m_onInputChangedMsgId, OnInputSrcChanged);
   if (m_onDevChangedMsgId)
      m_msgAPI.UnsubscribeMsg(m_onDevChangedMsgId, OnDevSrcChanged);
   if (m_onSegChangedMsgId)
      m_msgAPI.UnsubscribeMsg(m_onSegChangedMsgId, OnSegSrcChanged);
   if (m_onDisplayChangedMsgId)
      m_msgAPI.UnsubscribeMsg(m_onDisplayChangedMsgId, OnDisplaySrcChanged);
}

ResURIResolver::~ResURIResolver()
{
   // Ensure we're unsubscribed (in case BeginDestruction wasn't called)
   BeginDestruction();

   // Release message IDs
   if (m_onInputChangedMsgId)
   {
      m_msgAPI.ReleaseMsgID(m_onInputChangedMsgId);
      m_msgAPI.ReleaseMsgID(m_getInputSrcMsgId);
   }
   if (m_onDevChangedMsgId)
   {
      m_msgAPI.ReleaseMsgID(m_onDevChangedMsgId);
      m_msgAPI.ReleaseMsgID(m_getDevSrcMsgId);
   }
   if (m_onSegChangedMsgId)
   {
      m_msgAPI.ReleaseMsgID(m_onSegChangedMsgId);
      m_msgAPI.ReleaseMsgID(m_getSegSrcMsgId);
   }
   if (m_onDisplayChangedMsgId)
   {
      m_msgAPI.ReleaseMsgID(m_onDisplayChangedMsgId);
      m_msgAPI.ReleaseMsgID(m_getDisplaySrcMsgId);
   }
}

string ResURIResolver::trim_string(const string &str)
{
   size_t start = 0;
   size_t end = str.length();
   while (start < end && (str[start] == ' ' || str[start] == '\t' || str[start] == '\r' || str[start] == '\n'))
      ++start;
   while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t' || str[end - 1] == '\r' || str[end - 1] == '\n'))
      --end;
   return str.substr(start, end - start);
}

// trims leading whitespace or similar
bool ResURIResolver::try_parse_int(const string &str, int &value)
{
   const string tmp = trim_string(str);
   return (std::from_chars(tmp.c_str(), tmp.c_str() + tmp.length(), value).ec == std::errc {});
}

void ResURIResolver::OnInputSrcChanged(const unsigned int msgId, void *userData, void *msgData)
{
   ResURIResolver* me = static_cast<ResURIResolver *>(userData);
   // Guard against a stale `this` - userData may point to a freed resolver if the
   // plugin manager's Unsubscribe erased a different instance's subscription. The
   // m_isDestroyed check below would crash on a freed `me`; this check runs first
   // and is safe because s_liveResolvers is a pointer-keyed set. See the comment
   // on s_liveResolvers at the top of the file for the full background.
   if (s_liveResolvers.count(me) == 0)
      return;
   if (me->m_isDestroyed)
      return;
   GetInputSrcMsg getSrcMsg = { 0, 0, nullptr };
   me->m_msgAPI.BroadcastMsg(me->m_endpointId, me->m_getInputSrcMsgId, &getSrcMsg);
   me->m_inputSources.clear();
   me->m_inputSources.resize(getSrcMsg.count);
   getSrcMsg = { getSrcMsg.count, 0, me->m_inputSources.data() };
   me->m_msgAPI.BroadcastMsg(me->m_endpointId, me->m_getInputSrcMsgId, &getSrcMsg);
   me->m_floatCache.clear();
}

void ResURIResolver::OnDevSrcChanged(const unsigned int msgId, void *userData, void *msgData)
{
   ResURIResolver* me = static_cast<ResURIResolver *>(userData);
   // Stale-`this` guard - see comment on s_liveResolvers above.
   if (s_liveResolvers.count(me) == 0)
      return;
   if (me->m_isDestroyed)
      return;
   GetDevSrcMsg getSrcMsg = { 0, 0, nullptr };
   me->m_msgAPI.BroadcastMsg(me->m_endpointId, me->m_getDevSrcMsgId, &getSrcMsg);
   me->m_deviceSources.clear();
   me->m_deviceSources.resize(getSrcMsg.count);
   getSrcMsg = { getSrcMsg.count, 0, me->m_deviceSources.data() };
   me->m_msgAPI.BroadcastMsg(me->m_endpointId, me->m_getDevSrcMsgId, &getSrcMsg);
   me->m_floatCache.clear();
}

void ResURIResolver::OnSegSrcChanged(const unsigned int msgId, void *userData, void *msgData)
{
   ResURIResolver* me = static_cast<ResURIResolver *>(userData);
   // Stale-`this` guard - see comment on s_liveResolvers above.
   if (s_liveResolvers.count(me) == 0)
      return;
   if (me->m_isDestroyed)
      return;
   GetSegSrcMsg getSrcMsg = { 0, 0, nullptr };
   me->m_msgAPI.BroadcastMsg(me->m_endpointId, me->m_getSegSrcMsgId, &getSrcMsg);
   me->m_segSources.clear();
   me->m_segSources.resize(getSrcMsg.count);
   getSrcMsg = { getSrcMsg.count, 0, me->m_segSources.data() };
   me->m_msgAPI.BroadcastMsg(me->m_endpointId, me->m_getSegSrcMsgId, &getSrcMsg);
   me->m_segCache.clear();
}

void ResURIResolver::OnDisplaySrcChanged(const unsigned int msgId, void *userData, void *msgData)
{
   ResURIResolver* me = static_cast<ResURIResolver *>(userData);
   // Stale-`this` guard - see comment on s_liveResolvers above. This callback in
   // particular was the one observed crashing during table-exit teardown: FlexDMD's
   // dtor (released by VBScript engine cleanup) broadcasts an OnDmdSrcChanged
   // message, which fans out to every resolver subscribed to that ID - including
   // the one belonging to a Player that had already been freed.
   if (s_liveResolvers.count(me) == 0)
      return;
   if (me->m_isDestroyed)
      return;
   GetDisplaySrcMsg getSrcMsg = { 0, 0, nullptr };
   me->m_msgAPI.BroadcastMsg(me->m_endpointId, me->m_getDisplaySrcMsgId, &getSrcMsg);
   // Log the number of display sources found
   #if defined(__ANDROID__) && RESURI_DEBUG_LOG
   __android_log_print(ANDROID_LOG_INFO, "ResURIResolver", "OnDisplaySrcChanged: found %d display sources (endpoint=%u)", getSrcMsg.count, me->m_endpointId);
   #endif
   me->m_displaySources.clear();
   me->m_displaySources.resize(getSrcMsg.count);
   getSrcMsg = { getSrcMsg.count, 0, me->m_displaySources.data() };
   me->m_msgAPI.BroadcastMsg(me->m_endpointId, me->m_getDisplaySrcMsgId, &getSrcMsg);
   #if defined(__ANDROID__) && RESURI_DEBUG_LOG
   for (unsigned int i = 0; i < getSrcMsg.count; i++)
      __android_log_print(ANDROID_LOG_INFO, "ResURIResolver", "  source[%d]: %ux%u endpoint=%u resId=%d", i,
         me->m_displaySources[i].width, me->m_displaySources[i].height,
         me->m_displaySources[i].id.endpointId, me->m_displaySources[i].id.resId);
   #endif
   me->m_displayCache.clear();
}

float ResURIResolver::GetFloatState(const string &link)
{
   const auto &cache = m_floatCache.find(link);
   if (cache != m_floatCache.end())
      return cache->second(link);

   floatCacheLambda lambda = nullptr;
   const auto uri = uri::parse_uri(link);
   if (uri.error != uri::Error::None)
   {
      // FIXME log PLOGE << "Invalid resource URI: " << link;
   }
   else if (uri.scheme == "ctrl")
   {
      if (uri.authority.host == "default")
      {
         // Which definitions do we want to give for this (if any) ?
      }
      else
      {
         const unsigned int plugin = m_msgAPI.GetPluginEndpoint(uri.authority.host.c_str());
         if (plugin)
         {
            int resId = 0;
            auto resIdPart = uri.query.find("id"s);
            if (resIdPart != uri.query.end())
               try_parse_int(resIdPart->second, resId);

            if (uri.path == "/device")
            {
               auto ioSource = std::ranges::find_if(m_deviceSources.begin(), m_deviceSources.end(), [plugin, resId](const DevSrcId &cd) { return cd.id.endpointId == plugin && cd.id.resId == resId; });
               if (ioSource != m_deviceSources.end())
               {
                  int ioId = 0;
                  auto ioIdPart = uri.query.find("io"s);
                  if (ioIdPart != uri.query.end())
                     try_parse_int(ioIdPart->second, ioId);
                  
                  int ioIndex = -1;
                  for (unsigned int i = 0; i < ioSource->nDevices; i++)
                     if (ioSource->deviceDefs[i].mappingId == ioId)
                        ioIndex = i;
                     
                  if (ioIndex >= 0)
                     lambda = [ioSource, ioIndex](const string &) -> float { return ioSource->GetFloatState(ioIndex); };
               }
            }
            else if (uri.path == "/input")
            {
               auto ioSource = std::ranges::find_if(m_inputSources.begin(), m_inputSources.end(), [plugin, resId](const InputSrcId &cd) { return cd.id.endpointId == plugin && cd.id.resId == resId; });
               if (ioSource != m_inputSources.end())
               {
                  int ioId = 0;
                  auto ioIdPart = uri.query.find("io"s);
                  if (ioIdPart != uri.query.end())
                     try_parse_int(ioIdPart->second, ioId);
                  
                  int ioIndex = -1;
                  for (unsigned int i = 0; i < ioSource->nInputs; i++)
                     if (ioSource->inputDefs[i].mappingId == ioId)
                        ioIndex = i;
                     
                  if (ioIndex >= 0)
                     lambda = [ioSource, ioIndex](const string &) -> float { return static_cast<float>(ioSource->GetInputState(ioIndex)); };
               }
            }
            else if (uri.path == "/display")
            {
               // TODO implement (to access individual dots, useful for small LED matrices)
            }
         }
      }
   }

   if (lambda == nullptr)
      lambda = [](const string &) -> float { return 0.f; };
   m_floatCache[link] = lambda;
   return lambda(link);
}

ResURIResolver::SegDisplayState ResURIResolver::GetSegDisplayState(const string &link)
{
   const auto &cache = m_segCache.find(link);
   if (cache != m_segCache.end())
      return cache->second(link);

   segCacheLambda lambda = nullptr;
   const auto uri = uri::parse_uri(link);
   if (uri.error != uri::Error::None)
   {
      // FIXME log PLOGE << "Invalid resource URI: " << link;
   }
   else if ((uri.scheme == "ctrl") && (uri.path == "/seg"))
   {
      SegSrcId *segSource = nullptr;
      if (uri.authority.host == "default")
      {
         if (!m_segSources.empty())
         {
            CtlResId group = m_segSources[0].groupId;
            int index = 0;
            auto indexPart = uri.query.find("id"s); // id is used as index inside selected display group (which expects plugins to report displays in a stable order, should we sort by resId first ?)
            if (indexPart != uri.query.end())
               try_parse_int(indexPart->second, index);
            for (SegSrcId& source : m_segSources)
            {
               if (source.groupId.id == group.id)
               {
                  index--;
                  if (index < 0)
                  {
                     segSource = &source;
                     break;
                  }
               }
            }
         }
      }
      else
      {
         const unsigned int plugin = m_msgAPI.GetPluginEndpoint(uri.authority.host.c_str());
         if (plugin)
         {
            int resId = 0;
            auto resIdPart = uri.query.find("id"s);
            if (resIdPart != uri.query.end())
               try_parse_int(resIdPart->second, resId);

            auto source = std::ranges::find_if(m_segSources.begin(), m_segSources.end(), 
               [plugin, resId](const SegSrcId &cd) { return cd.id.endpointId == plugin && cd.id.resId == resId; });
            if (source != m_segSources.end())
               segSource = &*source;
         }
      }
      if (segSource)
      {
         int subId = -1;
         auto subIdPart = uri.query.find("sub"s);
         if (subIdPart != uri.query.end())
            try_parse_int(subIdPart->second, subId);

         if (subId < 0)
         {
            lambda = [segSource, subIdPart](const string &) -> SegDisplayState { return { segSource, segSource->GetState(segSource->id) }; };
         }
         else if (subId < static_cast<int>(segSource->nElements))
         {
            SegSrcId subSegSrc = *segSource;
            subSegSrc.GetState = nullptr;
            subSegSrc.nElements = 1;
            subSegSrc.elementType[0] = segSource->elementType[subId];
            lambda = [segSource, subSegSrc, subId](const string &) -> SegDisplayState
            {
               SegDisplayFrame state = segSource->GetState(segSource->id);
               return { &subSegSrc, { state.frameId, state.frame + subId * 16 } };
            };
         }
      }
   }

   if (lambda == nullptr)
      lambda = [](const string &) -> SegDisplayState { return { nullptr, { 0, nullptr} }; };
   m_segCache[link] = lambda;
   return lambda(link);
}

void ResURIResolver::SetDisplayFilter(std::function<bool(const DisplaySrcId& src)> filter)
{
   m_displayFilter = filter;
   m_displayCache.clear();
}

ResURIResolver::DisplayState ResURIResolver::GetDisplayState(const string &link)
{
   const auto &cache = m_displayCache.find(link);
   if (cache != m_displayCache.end())
      return cache->second(link);

   displayCacheLambda lambda = nullptr;
   const auto uri = uri::parse_uri(link);
   if (uri.error != uri::Error::None)
   {
      // FIXME log PLOGE << "Invalid resource URI: " << link;
   }
   else if ((uri.scheme == "ctrl") && (uri.path == "/display"))
   {
      DisplaySrcId* displaySource = nullptr;
      bool walkDownOverrides = true;
      if (uri.authority.host == "default")
      {
         unsigned int dsSize = 0; 
         for (auto& source : m_displaySources)
         {
            if (m_displayFilter)
            {
               // If this source is filtered out or override a filtered out source, then do not select it
               bool filteredOut = !m_displayFilter(source);
               uint64_t parentId = source.overrideId.id;
               while (!filteredOut && parentId != 0)
               {
                  auto parentSource = std::ranges::find_if(m_displaySources.begin(), m_displaySources.end(), 
                     [parentId](const DisplaySrcId &src) { return src.id.id == parentId; });
                  if (parentSource != m_displaySources.end())
                  {
                     if (!m_displayFilter(*parentSource))
                        filteredOut = true;
                     else
                        parentId = parentSource->overrideId.id;
                  }
                  else
                  {
                     assert(false); // Override is pointing to a missing parent, there is something wrong if we end up here
                     parentId = 0;
                  }
               }
               if (filteredOut)
                  continue;
            }
            const unsigned int sSize = source.width * source.height;
            if (
               // Priority 1: Find at least one display if any (size > 0)
               displaySource == nullptr
               // Priority 2: Favor the highest resolution display
               || (dsSize < sSize)
               // Priority 3: Favor color over monochrome
               || (dsSize == sSize && displaySource->frameFormat != source.frameFormat && displaySource->frameFormat == CTLPI_DISPLAY_FORMAT_LUM32F)
               // Priority 4: Favor RGB8 over other formats
               || (dsSize == sSize && displaySource->frameFormat != source.frameFormat && source.frameFormat == CTLPI_DISPLAY_FORMAT_SRGB888)
               // Priority 5: Favor the first source provided by an endpoint
               || (dsSize == sSize && displaySource->frameFormat == source.frameFormat && displaySource->id.resId > source.id.resId))
            {
               displaySource = &source;
               dsSize = sSize;
            }
         }
      }
      else
      {
         // TODO allow to enable/disable overrides
         const unsigned int plugin = m_msgAPI.GetPluginEndpoint(uri.authority.host.c_str());
         if (plugin)
         {
            int resId = 0;
            auto resIdPart = uri.query.find("id"s);
            if (resIdPart != uri.query.end())
               try_parse_int(resIdPart->second, resId);

            auto source = std::ranges::find_if(m_displaySources.begin(), m_displaySources.end(), 
               [plugin, resId](const DisplaySrcId &cd) { return cd.id.endpointId == plugin && cd.id.resId == resId; });
            if (source != m_displaySources.end())
               displaySource = &*source;
         }
      }

      // Select the tail of the override chain if any
      while (walkDownOverrides && displaySource != nullptr)
      {
         walkDownOverrides = false;
         // TODO handle situations where a source has multiple overrides (add a selection heuristic)
         for (auto& source : m_displaySources)
         {
            if (m_displayFilter && !m_displayFilter(source))
               continue;
            if (source.overrideId.id == displaySource->id.id)
            {
               displaySource = &source;
               walkDownOverrides = true;
               break;
            }
         }
      }

      if (displaySource != nullptr)
         lambda = [displaySource](const string &) -> DisplayState { return { displaySource, displaySource->GetRenderFrame(displaySource->id)}; };
   }

   if (lambda == nullptr)
      lambda = [](const string &) -> DisplayState { return { nullptr, { 0, nullptr} }; };
   m_displayCache[link] = lambda;
   return lambda(link);
}
