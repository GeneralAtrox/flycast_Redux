#include "Renderer_if.h"
#include "spg.h"
#include "rend/texconv.h"
#include "rend/transform_matrix.h"
#include "cfg/option.h"
#include "emulator.h"
#include "serialize.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/pvr/pvr_mem.h"
#include "profiler/fc_profiler.h"
#include "network/ggpo.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_ta_observation.h"

#include <mutex>
#include <deque>

#ifdef LIBRETRO
void retro_rend_present();
void retro_resize_renderer(int w, int h, float aspectRatio);
#endif

u32 FrameCount=1;

Renderer* renderer;

static cResetEvent renderEnd;
u32 fb_w_cur = 1;
static cResetEvent vramRollback;

// direct framebuffer write detection
static bool render_called = false;
u32 fb_watch_addr_start;
u32 fb_watch_addr_end;
bool fb_dirty;

static bool pend_rend;
static bool rendererEnabled = true;

static bool presented;
static u32 fbAddrHistory[2] { 1, 1 };
static u64 lastScreenRenderGeneration;
static u32 lastScreenRenderWriteAddress = UINT32_MAX;

static u64 captureFramebufferObservation(const FramebufferInfo& info)
{
	if (!research::pvrPresentationObservationBusActive())
		return 0;

	u32 width = (info.fb_r_size.fb_x_size + 1) * 2;
	u32 height = info.fb_r_size.fb_y_size + 1;
	u32 modulus = (info.fb_r_size.fb_modulus - 1) * 2;
	u32 bytesPerPixel;
	switch (info.fb_r_ctrl.fb_depth)
	{
	case fbde_0555:
	case fbde_565:
		bytesPerPixel = 2;
		break;
	case fbde_888:
		bytesPerPixel = 3;
		width = (width * 2) / 3;
		modulus = (modulus * 2) / 3;
		break;
	case fbde_C888:
		bytesPerPixel = 4;
		width /= 2;
		modulus /= 2;
		break;
	default:
		return 0;
	}

	u32 address = info.fb_r_sof1;
	if (info.spg_control.interlace)
	{
		if (width == modulus
				&& info.fb_r_sof2 == info.fb_r_sof1 + width * bytesPerPixel)
		{
			modulus = 0;
			height *= 2;
		}
		else
		{
			address = info.spg_status.fieldnum
					? info.fb_r_sof2 : info.fb_r_sof1;
		}
	}
	else if (info.fb_r_ctrl.vclk_div == 0)
	{
		height = std::min<u32>(height, 240);
	}

	const u32 rowBytes = width * bytesPerPixel;
	if (width == 0 || height == 0 || rowBytes == 0
			|| static_cast<u64>(rowBytes) * height > 32_MB)
		return 0;
	std::vector<u8> bytes(static_cast<size_t>(rowBytes) * height);
	for (u32 y = 0; y < height; ++y)
	{
		for (u32 x = 0; x < rowBytes; ++x)
			bytes[static_cast<size_t>(y) * rowBytes + x] =
					pvr_read32p<u8>(address + x);
		address += rowBytes + modulus * bytesPerPixel;
	}

	research::PvrFramebufferConfig config;
	config.fbReadSize = info.fb_r_size.full;
	config.fbReadControl = info.fb_r_ctrl.full;
	config.spgControl = info.spg_control.full;
	config.spgStatus = info.spg_status.full;
	config.fbReadSof1 = info.fb_r_sof1;
	config.fbReadSof2 = info.fb_r_sof2;
	config.videoControl = info.vo_control.full;
	config.borderColor = info.vo_border_col.full;
	return research::observePvrFramebufferCaptured(
			research::PvrFramebufferKind::DreamcastVram, 0, config, width, height,
			rowBytes, bytes.data(), bytes.size(), sh4_sched_now64());
}

class PvrMessageQueue
{
	using lock_guard = std::lock_guard<std::mutex>;

public:
	enum MessageType { NoMessage = -1, Render, RenderFramebuffer, Present, Stop };
	struct Message
	{
		Message() = default;
		Message(MessageType type, FramebufferInfo config,
				research::PvrPresentationSource source,
				u64 sourceGeneration)
			: type(type), config(config), source(source),
			  sourceGeneration(sourceGeneration) {}

		MessageType type = NoMessage;
		FramebufferInfo config;
		research::PvrPresentationSource source =
				research::PvrPresentationSource::Render;
		u64 sourceGeneration = 0;
	};

	void enqueue(MessageType type, FramebufferInfo config = FramebufferInfo(),
			research::PvrPresentationSource source =
					research::PvrPresentationSource::Render,
			u64 sourceGeneration = 0)
	{
		Message msg { type, config, source, sourceGeneration };
		if (config::ThreadedRendering)
		{
			// FIXME need some synchronization to avoid blinking in densha de go
			// or use !threaded rendering for emufb?
			// or read framebuffer vram on emu thread
			bool dupe;
			do {
				dupe = false;
				{
					const lock_guard lock(mutex);
					for (const auto& m : queue)
						if (m.type == type) {
							dupe = true;
							break;
						}
					if (!dupe || type == Present) {
						// Bound the queue to keep the emu-thread producer and
						// the renderer-thread consumer from drifting apart.
						// Render/RenderFramebuffer/Stop are already deduplicated
						// above, but Present is intentionally allowed to repeat
						// and can stack up indefinitely if the consumer stalls
						// (notably under libretro frontends, which drive the
						// swap from their own video callback). Unbounded growth
						// here is the producer side of progressive audio/video
						// drift in long sessions: the SH4 keeps running ahead
						// while pending Presents pile up. Drop the oldest
						// pending Present to keep latency bounded.
						constexpr size_t MAX_QUEUE_DEPTH = 4;
						if (queue.size() >= MAX_QUEUE_DEPTH)
						{
							for (auto it = queue.begin(); it != queue.end(); ++it)
							{
								if (it->type == Present)
								{
									queue.erase(it);
									break;
								}
							}
						}
						queue.push_back(msg);
						dupe = false;
					}
				}
				if (dupe)
				{
					if (type == Stop)
						return;
					dequeueEvent.Wait();
				}
			} while (dupe);
			enqueueEvent.Set();
		}
		else
		{
			setDefaultRoundingMode();
			// drain the queue after switching to !threaded rendering
			while (!queue.empty())
				waitAndExecute();
			execute(msg);
			Sh4cntx.restoreHostRoundingMode();
		}
	}

	bool waitAndExecute(int timeoutMs = -1)
	{
		return execute(dequeue(timeoutMs));
	}

	void reset() {
		const lock_guard lock(mutex);
		queue.clear();
	}

	void cancelEnqueue()
	{
		const lock_guard lock(mutex);
		for (auto it = queue.begin(); it != queue.end(); )
		{
			if (it->type != Render)
				it = queue.erase(it);
			else
				++it;
		}
		dequeueEvent.Set();
	}
private:
	Message dequeue(int timeoutMs = -1)
	{
		FC_PROFILE_SCOPE;

		Message msg;
		while (true)
		{
			{
				const lock_guard lock(mutex);
				if (!queue.empty())
				{
					msg = queue.front();
					queue.pop_front();
				}
			}
			if (msg.type != NoMessage) {
				dequeueEvent.Set();
				break;
			}
			if (timeoutMs == -1)
				enqueueEvent.Wait();
			else if (!enqueueEvent.Wait(timeoutMs))
				break;
		}
		return msg;
	}

	bool execute(Message msg)
	{
		switch (msg.type)
		{
		case Render:
			render(msg.sourceGeneration);
			return true;
		case RenderFramebuffer:
			renderFramebuffer(msg.config, msg.sourceGeneration);
			return true;
		case Present:
			present(msg.source, msg.sourceGeneration);
			return true;
		case Stop:
		case NoMessage:
		default:
			return false;
		}
	}

	void render(u64 queuedRenderGeneration)
	{
		FC_PROFILE_SCOPE;

		TA_context *taContext = DequeueRender();
		if (taContext == nullptr)
			return;
		const u64 renderGeneration = taContext->rend.researchRenderGeneration;
		if (queuedRenderGeneration != 0
				&& queuedRenderGeneration != renderGeneration)
			throw RendererException("PowerVR render generation queue mismatch");
		const research::PvrRenderKind renderKind = taContext->rend.isRTT
				? research::PvrRenderKind::RenderToTexture
				: config::EmulateFramebuffer
						? research::PvrRenderKind::FramebufferEmulation
						: research::PvrRenderKind::Screen;
		research::ScopedPvrRenderObservation renderObservation(
				renderGeneration, renderKind);

		int width, height;
		getScaledFramebufferSize(taContext->rend, width, height);
		taContext->rend.framebufferWidth = width;
		taContext->rend.framebufferHeight = height;
		bool renderToScreen = !taContext->rend.isRTT && !config::EmulateFramebuffer;
#ifdef LIBRETRO
		if (renderToScreen)
			retro_resize_renderer(taContext->rend.framebufferWidth, taContext->rend.framebufferHeight,
					getOutputFramebufferAspectRatio());
#endif
		{
			FC_PROFILE_SCOPE_NAMED("Renderer::Process");
			try {
				renderer->Process(taContext);
			} catch (...) {
				renderEnd.Set();
				rend_allow_rollback();
				FinishRender(taContext);
				throw;
			}
		}

		if (renderToScreen)
			// If rendering to texture or in full framebuffer emulation, continue locking until the frame is rendered
			renderEnd.Set();
		rend_allow_rollback();
		bool renderSuccessful = false;
		{
			FC_PROFILE_SCOPE_NAMED("Renderer::Render");
			try {
				renderSuccessful = renderer->Render();
			} catch (...) {
				if (!renderToScreen)
					renderEnd.Set();
				FinishRender(taContext);
				throw;
			}
		}
		research::observePvrRenderCompleted(renderGeneration, renderKind,
				renderSuccessful, sh4_sched_now64());
		if (renderSuccessful && renderKind == research::PvrRenderKind::Screen)
		{
			lastScreenRenderGeneration = renderGeneration;
			lastScreenRenderWriteAddress = taContext->rend.fb_W_SOF1;
		}

		if (!renderToScreen)
			renderEnd.Set();
		else if (config::DelayFrameSwapping && fb_w_cur == FB_R_SOF1)
			present(research::PvrPresentationSource::Render, renderGeneration);

		//clear up & free data ..
		FinishRender(taContext);
	}

	void renderFramebuffer(const FramebufferInfo& config, u64 framebufferGeneration)
	{
		FC_PROFILE_SCOPE;

#ifdef LIBRETRO
		int w, h;
		getDCFramebufferReadSize(config, w, h);
		retro_resize_renderer(w, h, getDCFramebufferAspectRatio());
#endif
		renderer->RenderFramebuffer(config);
		research::observePvrRenderCompleted(framebufferGeneration,
				research::PvrRenderKind::DirectFramebuffer, true,
				sh4_sched_now64());
	}

	void present(research::PvrPresentationSource source, u64 sourceGeneration)
	{
		FC_PROFILE_SCOPE;

		const bool successful = renderer->Present();
		if (successful && source == research::PvrPresentationSource::Render
				&& research::pvrPresentationObservationBusActive())
		{
			std::vector<u8> rgb;
			int width = 0;
			int height = 0;
			if (renderer->GetLastFrame(rgb, width, height) && width > 0 && height > 0
					&& rgb.size() == static_cast<size_t>(width) * height * 3)
			{
				const research::PvrFramebufferConfig config {};
				const u64 framebufferGeneration =
						research::observePvrFramebufferCaptured(
								research::PvrFramebufferKind::PresentedRgb24,
								sourceGeneration, config, static_cast<u32>(width),
								static_cast<u32>(height), static_cast<u32>(width * 3),
								rgb.data(), rgb.size(), sh4_sched_now64());
				if (framebufferGeneration != 0)
				{
					source = research::PvrPresentationSource::Framebuffer;
					sourceGeneration = framebufferGeneration;
				}
			}
		}
		// The renderer may swap its empty bootstrap surface before the first
		// completed PVR render. It has no game frame or causal source to record.
		if (sourceGeneration == 0)
			return;
		research::observePvrPresentation(source, sourceGeneration, successful,
				sh4_sched_now64());
		if (successful)
		{
			presented = true;
			if (!config::ThreadedRendering && !ggpo::active())
				emu.getSh4Executor()->Stop();
#ifdef LIBRETRO
			retro_rend_present();
#endif
		}
	}

	std::mutex mutex;
	cResetEvent enqueueEvent;
	cResetEvent dequeueEvent;
	std::deque<Message> queue;
};

static PvrMessageQueue pvrQueue;

bool rend_single_frame(const bool& enabled)
{
	FC_PROFILE_SCOPE;

	const int timeout = SPG_CONTROL.isPAL() ? 23 : 20;
	presented = false;
	while (enabled && !presented)
		if (!pvrQueue.waitAndExecute(timeout))
			return false;
	return true;
}

class SwapIntervalDetector
{
public:
	SwapIntervalDetector() {
		EventManager::listen(Event::LoadState, eventHandler, this);
		reset();
	}
	~SwapIntervalDetector() {
		EventManager::unlisten(Event::LoadState, eventHandler, this);
	}

	void render()
	{
		u64 now = sh4_sched_now64();
		if (lastRender != 0)
			renderInterval = now - lastRender;
		lastRender = now;
		renders++;
	}

	void vblank()
	{
		avgRenderInterval = 0.1f * renderInterval + 0.9f * avgRenderInterval;

		// Force transition to 60 FPS if the game swap interval is 1 for 3 consecutive frames.
		// Displaying a 60 FPS game at 30 FPS makes the game run in slo-mo and breaks audio.
		if (renders != 0)
		{
			renders = 0;
			rendersFullSpeed++;
			if (rendersFullSpeed >= 3)
			{
				// force 60/50 FPS now
				lastInterval = 1;
				stability = std::max(stability, 10);
				return;
			}
		}
		else {
			rendersFullSpeed = 0;
		}

		const float refreshRate = SPG_CONTROL.isPAL() ? 20_sh4ms : 16667_sh4us;
		int interval = std::round(avgRenderInterval / refreshRate);
		float frac = std::abs(avgRenderInterval / refreshRate - interval);

		if (frac <= .05f || (interval == 1 && frac <= .2f))
		{
			if (lastInterval == (int)interval) {
				stability++;
			}
			else {
				stability = 0;
				lastInterval = interval;
			}
		}
		else {
			stability = 0;
		}
	}

	int swapInterval()
	{
		if (stability < 10)
			return -1;
		else
			return std::min(lastInterval, 2);
	}

	void reset()
	{
		lastInterval = 1;
		stability = 0;

		lastRender = 0;
		renderInterval = 0;
		avgRenderInterval = 0.f;
		renders = 0;
		rendersFullSpeed = 0;
	}

private:
	static void eventHandler(Event event, void *arg) {
		SwapIntervalDetector *self = (SwapIntervalDetector *)arg;
		self->lastRender = 0;
		self->lastInterval = 1;
	}

	int lastInterval;
	int stability;

	u64 lastRender;
	u64 renderInterval;
	float avgRenderInterval;
	int renders;
	int rendersFullSpeed;
};
static SwapIntervalDetector swapIntervalDetector;


Renderer* rend_GLES2();
Renderer* rend_GL4();
Renderer* rend_norend();
Renderer* rend_Vulkan();
Renderer* rend_OITVulkan();
Renderer* rend_DirectX9();
Renderer* rend_DirectX11();
Renderer* rend_OITDirectX11();

static void rend_create_renderer()
{
#ifdef NO_REND
	renderer	 = rend_norend();
#else
	switch (config::RendererType)
	{
	default:
#ifdef USE_OPENGL
	case RenderType::OpenGL:
		renderer = rend_GLES2();
		break;
#if !defined(GLES2) && !defined(__APPLE__)
	case RenderType::OpenGL_OIT:
		renderer = rend_GL4();
		break;
#endif
#endif
#ifdef USE_VULKAN
	case RenderType::Vulkan:
		renderer = rend_Vulkan();
		break;
	case RenderType::Vulkan_OIT:
		renderer = rend_OITVulkan();
		break;
#endif
#ifdef USE_DX9
	case RenderType::DirectX9:
		renderer = rend_DirectX9();
		break;
#endif
#ifdef USE_DX11
	case RenderType::DirectX11:
		renderer = rend_DirectX11();
		break;
	case RenderType::DirectX11_OIT:
		renderer = rend_OITDirectX11();
		break;
#endif
	}
#endif
}

bool rend_init_renderer()
{
	rendererEnabled = true;
	if (renderer == nullptr)
		rend_create_renderer();
	bool success = renderer != nullptr && renderer->Init();
	if (!success) {
		delete renderer;
		renderer = rend_norend();
		renderer->Init();
	}
	return success;
}

void rend_term_renderer()
{
	// Drain and stop the queue first so that any in-flight Render/Present
	// messages cannot be dispatched against a renderer that is about to be
	// destroyed. This is called from many libretro entry points (context
	// reset/destroy, deinit, content load, renderer switch) where the emu
	// thread may still be holding queued work; without cancelling first
	// we can race the consumer thread into a null renderer dereference and
	// also leave the producer side wedged, which manifests as audio/video
	// drift after a renderer switch.
	rend_cancel_emu_wait();

	if (renderer != nullptr)
	{
		renderer->Term();
		delete renderer;
		renderer = nullptr;
	}
}

void rend_reset()
{
	FinishRender(DequeueRender());
	render_called = false;
	pend_rend = false;
	FrameCount = 1;
	fb_w_cur = 1;
	pvrQueue.reset();
	rendererEnabled = true;
	fbAddrHistory[0] = 1;
	fbAddrHistory[1] = 1;
	lastScreenRenderGeneration = 0;
	lastScreenRenderWriteAddress = UINT32_MAX;
	swapIntervalDetector.reset();
}

u64 rend_start_render()
{
	render_called = true;
	pend_rend = false;

	TA_context *ctx = nullptr;
	u32 addresses[MAX_PASSES];
	bool contextAvailability[MAX_PASSES] {};
	research::PvrTaRenderSelectionTranscript selectionTranscript;
	research::PvrTaRenderSelectionTranscript *selectionTranscriptPtr =
			research::pvrTaObservationBusActive() ? &selectionTranscript : nullptr;
	int count = getTAContextAddresses(addresses, selectionTranscriptPtr);
	if (count > 0)
	{
		ctx = tactx_Pop(addresses[0]);
		contextAvailability[0] = ctx != nullptr;
		if (ctx != nullptr)
		{
			TA_context *linkedCtx = ctx;
			for (int i = 1; i < count; i++)
			{
				TA_context *nextContext = tactx_Pop(addresses[i]);
				contextAvailability[i] = nextContext != nullptr;
				linkedCtx->nextContext = nextContext;
				if (nextContext != nullptr)
					linkedCtx = nextContext;
				else
					INFO_LOG(PVR, "rend_start_render: Context%d @ %x not found", i, addresses[i]);
			}
		}
		else
			INFO_LOG(PVR, "rend_start_render: Context0 @ %x not found", addresses[0]);
	}
	else
		INFO_LOG(PVR, "rend_start_render: No context not found");
	const u64 renderGeneration = research::observePvrTaStartRender(
			addresses, contextAvailability,
			count > 0 ? static_cast<std::size_t>(count) : 0u,
			selectionTranscriptPtr,
			sh4_sched_now64());

	scheduleRenderDone(ctx);

	if (ctx == nullptr)
		return renderGeneration;
	ctx->rend.researchRenderGeneration = renderGeneration;

	FillBGP(ctx);

	ctx->rend.isRTT = (FB_W_SOF1 & 0x1000000) != 0;
	ctx->rend.fb_W_SOF1 = FB_W_SOF1;
	ctx->rend.fb_W_CTRL.full = FB_W_CTRL.full;

	ctx->rend.globClip.x = (TA_GLOB_TILE_CLIP.tile_x_num + 1) * 32;
	ctx->rend.globClip.y = (TA_GLOB_TILE_CLIP.tile_y_num + 1) * 32;
	ctx->rend.scaler_ctl = SCALER_CTL;
	ctx->rend.fbClip.origin.x = FB_X_CLIP.min;
	ctx->rend.fbClip.origin.y = FB_Y_CLIP.min;
	ctx->rend.fbClip.size.x = FB_X_CLIP.max - FB_X_CLIP.min + 1;
	ctx->rend.fbClip.size.y = FB_Y_CLIP.max - FB_Y_CLIP.min + 1;
	ctx->rend.fb_W_LINESTRIDE = FB_W_LINESTRIDE.stride;

	ctx->rend.fog_clamp_min = FOG_CLAMP_MIN;
	ctx->rend.fog_clamp_max = FOG_CLAMP_MAX;

	if (!ctx->rend.isRTT)
	{
		if (FB_W_SOF1 != fbAddrHistory[0] && FB_W_SOF1 != fbAddrHistory[1])
		{
			ctx->rend.clearFramebuffer = true;
			fbAddrHistory[0] = fbAddrHistory[1];
			fbAddrHistory[1] = FB_W_SOF1;
		}
		else {
			ctx->rend.clearFramebuffer = false;
		}
		ggpo::endOfFrame();
		swapIntervalDetector.render();
		if (!config::EmulateFramebuffer)
			ctx->rend.swapInterval = swapIntervalDetector.swapInterval();
		else
			ctx->rend.swapInterval = 1;
	}

	if (QueueRender(ctx))
	{
		const research::PvrRenderKind renderKind = ctx->rend.isRTT
				? research::PvrRenderKind::RenderToTexture
				: config::EmulateFramebuffer
						? research::PvrRenderKind::FramebufferEmulation
						: research::PvrRenderKind::Screen;
		research::observePvrRenderQueued(renderGeneration, renderKind,
				ctx->rend.fb_W_SOF1, sh4_sched_now64());
		palette_update();
		pend_rend = true;
		pvrQueue.enqueue(PvrMessageQueue::Render, FramebufferInfo {},
				research::PvrPresentationSource::Render, renderGeneration);
		if (!config::DelayFrameSwapping && !ctx->rend.isRTT && !config::EmulateFramebuffer)
			pvrQueue.enqueue(PvrMessageQueue::Present, FramebufferInfo {},
					research::PvrPresentationSource::Render, renderGeneration);
	}
	return renderGeneration;
}

int rend_end_render(int tag, int cycles, int jitter, void *arg)
{
	research::observePvrTaRenderDone(sh4_sched_now64());
	if (settings.platform.isNaomi2())
	{
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE);
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE_isp);
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE_vd);
	}
	else
	{
		asic_RaiseInterrupt(holly_RENDER_DONE);
		asic_RaiseInterrupt(holly_RENDER_DONE_isp);
		asic_RaiseInterrupt(holly_RENDER_DONE_vd);
	}
	if (pend_rend && config::ThreadedRendering)
		renderEnd.Wait();

	return 0;
}

void rend_vblank()
{
	if (config::EmulateFramebuffer
			|| (!render_called && fb_dirty && FB_R_CTRL.fb_enable))
	{
		if (rend_is_enabled())
		{
			FramebufferInfo fbInfo;
			fbInfo.update();
			const u64 framebufferGeneration = captureFramebufferObservation(fbInfo);
			research::observePvrRenderQueued(framebufferGeneration,
					research::PvrRenderKind::DirectFramebuffer,
					fbInfo.fb_r_sof1, sh4_sched_now64());
			pvrQueue.enqueue(PvrMessageQueue::RenderFramebuffer, fbInfo,
					research::PvrPresentationSource::Framebuffer,
					framebufferGeneration);
			pvrQueue.enqueue(PvrMessageQueue::Present, FramebufferInfo {},
					research::PvrPresentationSource::Framebuffer,
					framebufferGeneration);
			if (!config::EmulateFramebuffer)
				DEBUG_LOG(PVR, "Direct framebuffer write detected");
		}
		fb_dirty = false;
	}
	render_called = false;
	check_framebuffer_write();
	emu.vblank();
	swapIntervalDetector.vblank();
}

void check_framebuffer_write()
{
	u32 fb_size = (FB_R_SIZE.fb_y_size + 1) * (FB_R_SIZE.fb_x_size + FB_R_SIZE.fb_modulus) * 4;
	fb_watch_addr_start = (SPG_CONTROL.interlace ? FB_R_SOF2 : FB_R_SOF1) & VRAM_MASK;
	fb_watch_addr_end = fb_watch_addr_start + fb_size;
}

void rend_cancel_emu_wait()
{
	if (config::ThreadedRendering)
	{
		FinishRender(NULL);
		renderEnd.Set();
		rend_allow_rollback();
		pvrQueue.cancelEnqueue();
		// Needed for android where this function may be called
		// from a thread different from the UI one
		pvrQueue.enqueue(PvrMessageQueue::Stop);
	}
}

void rend_set_fb_write_addr(u32 fb_w_sof1)
{
	if (fb_w_sof1 & 0x1000000)
		// render to texture
		return;
	fb_w_cur = fb_w_sof1;
}

void rend_swap_frame(u32 fb_r_sof)
{
	if (!config::EmulateFramebuffer && fb_r_sof == fb_w_cur && rend_is_enabled())
	{
		const u64 generation = lastScreenRenderWriteAddress == fb_r_sof
				? lastScreenRenderGeneration : 0;
		pvrQueue.enqueue(PvrMessageQueue::Present, FramebufferInfo {},
				research::PvrPresentationSource::Render, generation);
	}
}

void rend_disable_rollback()
{
	vramRollback.Reset();
}

void rend_allow_rollback()
{
	vramRollback.Set();
}

void rend_start_rollback()
{
	if (config::ThreadedRendering)
		vramRollback.Wait();
}

void rend_enable_renderer(bool enabled) {
	rendererEnabled = enabled;
}

bool rend_is_enabled() {
	return rendererEnabled;
}

void rend_serialize(Serializer& ser)
{
	ser << fb_w_cur;
	ser << render_called;
	ser << fb_dirty;
	ser << fb_watch_addr_start;
	ser << fb_watch_addr_end;
}
void rend_deserialize(Deserializer& deser)
{
	deser >> fb_w_cur;
	if (deser.version() >= Deserializer::V20)
	{
		deser >> render_called;
		deser >> fb_dirty;
		deser >> fb_watch_addr_start;
		deser >> fb_watch_addr_end;
	}
	pend_rend = false;
	fbAddrHistory[0] = 1;
	fbAddrHistory[1] = 1;
}
