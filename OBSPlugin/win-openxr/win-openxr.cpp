//
// OpenXR API Layer Mirror Capture input plugin for OBS
// by Jabbah https://github.com/Jabbah
//
// This plugin is based on the OpenVR plugin for OBS written
// by Keijo "Kegetys" Ruotsalainen, http://www.kegetys.fi
// https://obsproject.com/forum/resources/openvr-input-plugin.534/
//

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <algorithm>
#include <limits>
#include <new>
#include <vector>

#include "dxgi_format_info.h"
#include "obs_mirror_ipc.h"

#pragma comment(lib, "d3d11.lib")

#include <tchar.h>

#define blog(log_level, message, ...) \
	blog(log_level, "[win_openxr_mirror] " message, ##__VA_ARGS__)

#define debug(message, ...)                                                    \
	blog(LOG_DEBUG, "[%s] " message, obs_source_get_name(context->source), \
	     ##__VA_ARGS__)
#define info(message, ...)                                                    \
	blog(LOG_INFO, "[%s] " message, obs_source_get_name(context->source), \
	     ##__VA_ARGS__)
#define warn(message, ...)                 \
	blog(LOG_WARNING, "[%s] " message, \
	     obs_source_get_name(context->source), ##__VA_ARGS__)

struct crop {
	double top;
	double left;
	double bottom;
	double right;
};

struct croppreset {
	char name[128];
	crop crop;
};

std::vector<croppreset> croppresets;

using obs_mirror_ipc::DxgiFormatInfo;
using obs_mirror_ipc::GetFormatInfo;
using obs_mirror_ipc::kMirrorTextureCount;

// Logged at load and published through the shared diagnostics block so the
// layer log records which plugin build it talked to.
static const char *const kPluginVersion = "0.3.0-beta.4";

struct win_openxrmirror {
	obs_source_t *source;
	HANDLE map_file = nullptr;
	obs_mirror_ipc::MirrorSurfaceData *surface = nullptr;
	// Null while disconnected or when attached to an older layer that only
	// provides the legacy 64-byte surface.
	obs_mirror_ipc::MirrorDiagnostics *diag = nullptr;
	bool legacy_surface = false;
	std::uint64_t shared_handle = 0;
	std::uint32_t surface_generation = 0;

	// Connection-progress logging state. Deliberately not reset by deinit so
	// waiting messages repeat every 30 s instead of on every 1 s init retry.
	int connect_stage = -1;
	ULONGLONG stage_log_tick = 0;
	LUID adapter_luid = {};

	// Stale-frame watchdog: notices when the layer stops publishing frames.
	std::uint32_t last_index = 0;
	ULONGLONG last_index_tick = 0;
	std::uint32_t heartbeat_baseline = 0;
	bool stall_warned = false;
	ULONGLONG last_health_log_tick = 0;
	ULONGLONG last_render_tick = 0;
	ULONGLONG last_adapter_mismatch_log_tick = 0;
	std::uint64_t render_count = 0;

	int captureeye = 1; // left = 0, right = 1, both = 2
	int croppreset;
	crop crop;

	float blend = 0.0f;
	float blendPos = 0.0f;
	float overlap = 0.0f;
	float smoothing = 0.0f;
	float smoothCrop = 8.0f;
	bool appSmoothingManaged = false;
	float appSmoothing = 0.0f;
	float appSmoothCrop = 8.0f;
	ULONGLONG lastAppSettingsCheckTick = 0;

	gs_texture_t *texture = nullptr;
	winrt::com_ptr<ID3D11Device> dev11 = nullptr;
	winrt::com_ptr<ID3D11DeviceContext> ctx11 = nullptr;
	std::vector<winrt::com_ptr<ID3D11Texture2D>> mirror_textures;

	winrt::com_ptr<ID3D11Texture2D> texCrop = nullptr;

	ULONGLONG lastCheckTick;

	// Set in win_openxrmirror_init, 0 until then.
	unsigned int device_width;
	unsigned int device_height;

	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;

	bool initialized;
	bool active;

};

static void publish_smoothing(win_openxrmirror *context)
{
	if (!context->surface)
		return;

	context->surface->smoothing = context->appSmoothingManaged
					      ? context->appSmoothing
					      : context->smoothing;
	context->surface->smoothCrop = context->appSmoothingManaged
					       ? context->appSmoothCrop
					       : context->smoothCrop;
}

static void refresh_app_smoothing(win_openxrmirror *context, bool force = false)
{
	const ULONGLONG now = GetTickCount64();
	if (!force && now - context->lastAppSettingsCheckTick < 250)
		return;
	context->lastAppSettingsCheckTick = now;

	DWORD managed = 0;
	DWORD smoothing = 0;
	DWORD cropTenths = 80;
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\OpenXR-OBSMirror", 0,
			  KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
		DWORD size = sizeof(DWORD);
		RegQueryValueExW(key, L"CameraSmoothingManaged", nullptr, nullptr,
				 reinterpret_cast<BYTE *>(&managed), &size);
		size = sizeof(DWORD);
		RegQueryValueExW(key, L"CameraSmoothing", nullptr, nullptr,
				 reinterpret_cast<BYTE *>(&smoothing), &size);
		size = sizeof(DWORD);
		RegQueryValueExW(key, L"SmoothingCropTenths", nullptr, nullptr,
				 reinterpret_cast<BYTE *>(&cropTenths), &size);
		RegCloseKey(key);
	}

	context->appSmoothingManaged = managed != 0;
	context->appSmoothing = static_cast<float>(std::clamp<DWORD>(smoothing, 0, 100));
	context->appSmoothCrop = static_cast<float>(std::clamp<DWORD>(cropTenths, 0, 250)) / 10.0f;
	publish_smoothing(context);
}

enum connect_stage_t {
	STAGE_NO_MAPPING = 0,
	STAGE_MAPPING_FAILED,
	STAGE_WAITING_HANDLES,
	// Mapped and handles published, but opening/creating textures failed.
	// Persistent (e.g. GPU mismatch), so its warnings must be throttled -
	// init retries every second.
	STAGE_INIT_FAILED,
	STAGE_CONNECTED,
};

// True when this stage should be logged now: always on a stage change, and
// every 30 s while stuck in a waiting stage (STAGE_CONNECTED never repeats).
static bool should_log_stage(win_openxrmirror *context, int stage)
{
	const ULONGLONG now = GetTickCount64();
	if (context->connect_stage != stage) {
		context->connect_stage = stage;
		context->stage_log_tick = now;
		return true;
	}
	if (stage != STAGE_CONNECTED && now - context->stage_log_tick >= 30000) {
		context->stage_log_tick = now;
		return true;
	}
	return false;
}

// Shared textures cannot cross GPU adapters; when both sides published their
// LUID and they differ, a blank source is expected - say so explicitly.
static void warn_on_adapter_mismatch(win_openxrmirror *context)
{
	if (!context->diag ||
	    context->diag->layerMagic != obs_mirror_ipc::kDiagnosticsMagic)
		return;

	const bool obs_luid_known = context->adapter_luid.LowPart != 0 ||
				    context->adapter_luid.HighPart != 0;
	const bool layer_luid_known = context->diag->layerAdapterLuidLow != 0 ||
				      context->diag->layerAdapterLuidHigh != 0;
	if (obs_luid_known && layer_luid_known &&
	    (context->diag->layerAdapterLuidLow !=
		     (std::uint32_t)context->adapter_luid.LowPart ||
	     context->diag->layerAdapterLuidHigh !=
		     context->adapter_luid.HighPart)) {
		const ULONGLONG now = GetTickCount64();
		if (context->last_adapter_mismatch_log_tick != 0 &&
		    now - context->last_adapter_mismatch_log_tick < 30000)
			return;
		context->last_adapter_mismatch_log_tick = now;
		warn("GPU MISMATCH: OBS renders on adapter LUID %08lX:%08lX but the VR game's mirror is on %08X:%08X. "
		     "Shared textures cannot cross GPUs, so this source will stay blank. Force OBS onto the game's GPU "
		     "(Windows Settings > System > Display > Graphics).",
		     (unsigned long)context->adapter_luid.HighPart,
		     (unsigned long)context->adapter_luid.LowPart,
		     context->diag->layerAdapterLuidHigh,
		     context->diag->layerAdapterLuidLow);
	}
}

static void win_openxrmirror_deinit(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	context->initialized = false;

	if (context->texture) {
		obs_enter_graphics();
		gs_texture_destroy(context->texture);
		obs_leave_graphics();
		context->texture = NULL;
	}

	context->texCrop = nullptr;
	context->mirror_textures.clear();
	context->ctx11 = nullptr;
	context->dev11 = nullptr;

	context->device_width = 0;
	context->device_height = 0;
	context->shared_handle = 0;
	context->surface_generation = 0;

	if (context->surface) {
		// Withdraw our identity so the layer's consumer log stays accurate
		// after OBS exits (the magic is re-stamped on the next init).
		if (context->diag)
			context->diag->pluginMagic = 0;
		UnmapViewOfFile(context->surface);
		context->surface = nullptr;
	}
	context->diag = nullptr;
	context->legacy_surface = false;
	if (context->map_file) {
		CloseHandle(context->map_file);
		context->map_file = nullptr;
	}
}

static void win_openxrmirror_init(void *data, bool forced = false)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	if (context->initialized)
		return;

	// Dont attempt to init too often
	if (GetTickCount64() - 1000 < context->lastCheckTick && !forced) {
		return;
	}

	// Make sure everything is reset
	win_openxrmirror_deinit(data);

	context->lastCheckTick = GetTickCount64();

	context->map_file = OpenFileMappingW(
		FILE_MAP_WRITE | FILE_MAP_READ, false,
		obs_mirror_ipc::kSharedMemoryName);

	if (context->map_file == nullptr) {
		const DWORD error = GetLastError();
		// The VR application normally creates this mapping after OBS
		// starts, so absence is the expected idle state; still, say so
		// periodically because "blank source" reports start here.
		if (error == ERROR_FILE_NOT_FOUND) {
			if (should_log_stage(context, STAGE_NO_MAPPING))
				info("waiting for a VR application: shared surface '%ls' not found yet (no game with the OpenXR mirror layer is running)",
				     obs_mirror_ipc::kSharedMemoryName);
		} else if (should_log_stage(context, STAGE_MAPPING_FAILED)) {
			warn("could not open the mirror shared surface (error %lu)%s",
			     error,
			     error == ERROR_ACCESS_DENIED
				     ? " - if OBS or the game runs as administrator, run both at the same elevation"
				     : "");
		}
		return;
	}

	context->surface =
		(obs_mirror_ipc::MirrorSurfaceData *)MapViewOfFile(
		context->map_file,              // handle to map object
		FILE_MAP_WRITE | FILE_MAP_READ, // read permission
		0, 0, sizeof(obs_mirror_ipc::MirrorSurfaceData));

	if (context->surface == nullptr) {
		// A mapping created by an older layer build can be smaller than
		// the current struct; fall back to the legacy prefix so capture
		// still works, just without shared diagnostics.
		context->surface =
			(obs_mirror_ipc::MirrorSurfaceData *)MapViewOfFile(
			context->map_file, FILE_MAP_WRITE | FILE_MAP_READ,
			0, 0, obs_mirror_ipc::kLegacySurfaceSize);
		if (context->surface) {
			context->legacy_surface = true;
			info("connected to an older mirror layer (legacy 64-byte surface); update the OpenXR layer for full diagnostics");
		}
	}

	if (context->surface == nullptr) {
		warn("win_openxrmirror_init: could not map the mirror shared surface (error %lu)",
		     GetLastError());
		CloseHandle(context->map_file);
                context->map_file = nullptr;
                return;
        }

	context->diag = context->legacy_surface ? nullptr
						: &context->surface->diagnostics;
	if (context->diag) {
		context->diag->pluginDiagVersion =
			obs_mirror_ipc::kDiagnosticsVersion;
		context->diag->pluginPid = GetCurrentProcessId();
		strncpy_s(context->diag->pluginVersionString, kPluginVersion,
			  _TRUNCATE);
		context->diag->pluginMagic = obs_mirror_ipc::kDiagnosticsMagic;
	}

        context->surface->eyeIndex = context->captureeye;
        context->surface->blend = context->blend;
        context->surface->blendPos = context->blendPos;
        context->surface->overlap = context->overlap;
	refresh_app_smoothing(context, true);

        HRESULT hr;
        obs_enter_graphics();
        if (gs_get_device_type() == GS_DEVICE_DIRECT3D_11) {
            context->dev11.copy_from(static_cast<ID3D11Device*>(gs_get_device_obj()));
        }
        obs_leave_graphics();
        if (!context->dev11) {
            warn("win_openxrmirror_init: OBS is not using a D3D11 graphics device");
            return;
        }
        context->dev11->GetImmediateContext(context->ctx11.put());
        if (!context->ctx11) {
            warn("win_openxrmirror_init: Could not get OBS's D3D11 immediate context");
            return;
        }

        // Publish OBS's adapter identity so both logs can prove whether OBS
        // and the game render on the same GPU.
        if (auto dxgiDevice = context->dev11.try_as<IDXGIDevice>()) {
            winrt::com_ptr<IDXGIAdapter> dxgiAdapter;
            DXGI_ADAPTER_DESC adapterDesc{};
            if (SUCCEEDED(dxgiDevice->GetAdapter(dxgiAdapter.put())) &&
                SUCCEEDED(dxgiAdapter->GetDesc(&adapterDesc))) {
                context->adapter_luid = adapterDesc.AdapterLuid;
                if (context->diag) {
                    context->diag->pluginAdapterLuidLow =
                        (std::uint32_t)adapterDesc.AdapterLuid.LowPart;
                    context->diag->pluginAdapterLuidHigh =
                        adapterDesc.AdapterLuid.HighPart;
                }
            }
        }
	warn_on_adapter_mismatch(context);

        context->mirror_textures = std::vector<winrt::com_ptr<ID3D11Texture2D>>();
        const std::uint32_t published_generation =
            context->surface->surfaceGeneration.load(std::memory_order_acquire);
        const std::uint64_t published_handle = context->surface->sharedHandle[0];
        if (published_generation == 0 || published_handle == 0) {
            // The layer publishes handles after the application's first usable
            // swapchain image. This is an expected transient state, not an
            // error, but it is the state a "source stays blank" report usually
            // sits in - so describe it periodically with what we know.
            if (should_log_stage(context, STAGE_WAITING_HANDLES)) {
                if (context->diag &&
                    context->diag->layerMagic == obs_mirror_ipc::kDiagnosticsMagic) {
                    info("layer connected: app '%s', layer %s, pid %u, heartbeat %u - waiting for mirror textures "
                         "(published once the game renders while this source is active)",
                         context->diag->applicationName[0] ? context->diag->applicationName : "unknown",
                         context->diag->layerVersionString[0] ? context->diag->layerVersionString : "unknown",
                         context->diag->layerPid,
                         context->diag->layerHeartbeat.load());
                } else {
                    info("shared surface open - waiting for the layer to publish mirror textures");
                }
                warn_on_adapter_mismatch(context);
            }
            return;
        }
        MemoryBarrier();

        for (UINT i = 0; i < kMirrorTextureCount; ++i) {
            const std::uint64_t shared_handle = i == 0 ? published_handle : context->surface->sharedHandle[i];

            if (shared_handle == 0) {
                if (should_log_stage(context, STAGE_INIT_FAILED))
                    warn("win_openxrmirror_init: Mirror surface handle is null");
                return;
            }

            winrt::com_ptr<IDXGIResource> copy_tex_resource_mirror = nullptr;
            hr =
                context->dev11->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(shared_handle)),
                                                   __uuidof(IDXGIResource),
                                                   copy_tex_resource_mirror.put_void());
            if (FAILED(hr) || !copy_tex_resource_mirror) {
                if (should_log_stage(context, STAGE_INIT_FAILED)) {
                    warn("win_openxrmirror_init: OpenSharedResource failed (hr 0x%08lX) for handle %llu - OBS cannot open the game's shared texture",
                         (unsigned long)hr, (unsigned long long)shared_handle);
                    warn_on_adapter_mismatch(context);
                }
                return;
            }

            winrt::com_ptr<ID3D11Texture2D> mirror_texture;
            hr = copy_tex_resource_mirror->QueryInterface(__uuidof(ID3D11Texture2D), mirror_texture.put_void());
            if (FAILED(hr) || !mirror_texture) {
                if (should_log_stage(context, STAGE_INIT_FAILED))
                    warn("win_openxrmirror_init: shared resource is not a Texture2D (hr 0x%08lX)",
                         (unsigned long)hr);
                return;
            }
            context->mirror_textures.push_back(mirror_texture);
        }
        MemoryBarrier();
        if (context->surface->sharedHandle[0] != published_handle ||
            context->surface->surfaceGeneration.load(std::memory_order_acquire) != published_generation) {
            warn("win_openxrmirror_init: Mirror surface changed during initialization");
            return;
        }
        context->shared_handle = published_handle;
        context->surface_generation = published_generation;

        D3D11_TEXTURE2D_DESC desc;
        context->mirror_textures[0]->GetDesc(&desc);
        if (desc.Width == 0 || desc.Height == 0) {
            if (should_log_stage(context, STAGE_INIT_FAILED))
                warn("win_openxrmirror_init: device width or height is 0");
            return;
        }
        context->device_width = desc.Width;
	context->device_height = desc.Height;

	// Apply wanted cropping to size
	const crop &crop = context->crop;
	context->x = std::clamp((uint32_t)(crop.left / 100.0 * desc.Width), 0u, desc.Width - 1);
	context->y = std::clamp((uint32_t)(crop.top / 100.0 * desc.Height), 0u, desc.Height - 1);
	const unsigned int remainingWidth = desc.Width - context->x;
	const unsigned int remainingHeight = desc.Height - context->y;
	desc.Width = remainingWidth -
		     std::clamp((uint32_t)(crop.right / 100.0 * remainingWidth), 0u, remainingWidth - 1);
	desc.Height = remainingHeight -
		      std::clamp((uint32_t)(crop.bottom / 100.0 * remainingHeight), 0u, remainingHeight - 1);

	context->width = desc.Width;
	context->height = desc.Height;

	// Create cropped, linear texture
	// Using linear here will cause correct sRGB gamma to be applied
	DxgiFormatInfo info{};
	if (!GetFormatInfo(desc.Format, info)) {
		if (should_log_stage(context, STAGE_INIT_FAILED))
			warn("win_openxrmirror_init: Unsupported DXGI texture format: %d",
			     desc.Format);
		return;
	}
	desc.Format = info.linear;
	hr = context->dev11->CreateTexture2D(&desc, NULL, context->texCrop.put());
	if (FAILED(hr)) {
		if (should_log_stage(context, STAGE_INIT_FAILED))
			warn("win_openxrmirror_init: CreateTexture2D failed (hr 0x%08lX) for %ux%u format %d",
			     (unsigned long)hr, desc.Width, desc.Height,
			     (int)desc.Format);
		return;
	}

	// Get IDXGIResource, then share handle, and open it in OBS device
	IDXGIResource *res;
	hr = context->texCrop->QueryInterface(__uuidof(IDXGIResource),
					      (void **)&res);
	if (FAILED(hr)) {
		if (should_log_stage(context, STAGE_INIT_FAILED))
			warn("win_openxrmirror_init: QueryInterface failed (hr 0x%08lX)",
			     (unsigned long)hr);
		return;
	}

	HANDLE handle = NULL;
	hr = res->GetSharedHandle(&handle);
	res->Release();
	if (FAILED(hr)) {
		if (should_log_stage(context, STAGE_INIT_FAILED))
			warn("win_openxrmirror_init: GetSharedHandle failed (hr 0x%08lX)",
			     (unsigned long)hr);
		return;
	}

	const std::uintptr_t handle_value = reinterpret_cast<std::uintptr_t>(handle);
	if (handle_value > std::numeric_limits<std::uint32_t>::max()) {
		if (should_log_stage(context, STAGE_INIT_FAILED))
			warn("win_openxrmirror_init: Shared texture handle does not fit OBS's 32-bit handle API");
		return;
	}

	obs_enter_graphics();
	gs_texture_destroy(context->texture);
	context->texture =
		gs_texture_open_shared(static_cast<std::uint32_t>(handle_value));
	obs_leave_graphics();
	if (!context->texture) {
		if (should_log_stage(context, STAGE_INIT_FAILED)) {
			warn("win_openxrmirror_init: OBS could not open the shared crop texture (handle 0x%08X)",
			     (unsigned)handle_value);
			warn_on_adapter_mismatch(context);
		}
		return;
	}

	if (context->diag &&
	    context->diag->layerMagic == obs_mirror_ipc::kDiagnosticsMagic) {
		info("connected to app '%s' (pid %u, layer %s); game adapter LUID %08X:%08X, OBS adapter LUID %08lX:%08lX",
		     context->diag->applicationName[0]
			     ? context->diag->applicationName
			     : "unknown",
		     context->diag->layerPid,
		     context->diag->layerVersionString[0]
			     ? context->diag->layerVersionString
			     : "unknown",
		     context->diag->layerAdapterLuidHigh,
		     context->diag->layerAdapterLuidLow,
		     (unsigned long)context->adapter_luid.HighPart,
		     (unsigned long)context->adapter_luid.LowPart);
	}
	info("mirror capture initialized: source %ux%u, cropped output %ux%u format %d, generation %u",
	     context->device_width, context->device_height, context->width,
	     context->height, (int)desc.Format, context->surface_generation);
	info("capture settings: eye %d, crop %.1f%% top / %.1f%% right / %.1f%% bottom / %.1f%% left, overlap %.1f%%, blend %.1f%% at %.1f%%",
	     context->captureeye, context->crop.top, context->crop.right,
	     context->crop.bottom, context->crop.left, context->overlap,
	     context->blend, context->blendPos);
	should_log_stage(context, STAGE_CONNECTED);

	context->last_index = context->surface->lastProcessedIndex.load();
	context->last_index_tick = GetTickCount64();
	context->heartbeat_baseline =
		context->diag ? context->diag->layerHeartbeat.load() : 0;
	context->stall_warned = false;
	context->last_health_log_tick = GetTickCount64();
	context->last_render_tick = 0;
	context->render_count = 0;

	context->initialized = true;

}

static const char *win_openxrmirror_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("OpenXRMirrorCapture");
}

static void win_openxrmirror_update(void *data, obs_data_t *settings)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;
	const int captureeye = std::clamp(
		static_cast<int>(obs_data_get_int(settings, "captureeye")), 0, 2);
	const double crop_left =
		std::clamp(obs_data_get_double(settings, "cropleft"), 0.0, 100.0);
	const double crop_right =
		std::clamp(obs_data_get_double(settings, "cropright"), 0.0, 100.0);

	crop newCrop;
	if (captureeye == 1) {
		newCrop.left = crop_left;
		newCrop.right = crop_right;
	} else {
		newCrop.left = crop_right;
		newCrop.right = crop_left;
	}
	newCrop.top =
		std::clamp(obs_data_get_double(settings, "croptop"), 0.0, 100.0);
	newCrop.bottom =
		std::clamp(obs_data_get_double(settings, "cropbottom"), 0.0, 100.0);

	// Eye selection and crop change the texture layout and need a rebuild;
	// the blend parameters are consumed live by the layer every frame.
	const bool needsReinit = captureeye != context->captureeye ||
				 newCrop.top != context->crop.top ||
				 newCrop.left != context->crop.left ||
				 newCrop.bottom != context->crop.bottom ||
				 newCrop.right != context->crop.right;

	context->captureeye = captureeye;
	context->crop = newCrop;

	context->overlap = static_cast<float>(
		std::clamp(obs_data_get_double(settings, "eyeoverlap"), 0.0, 100.0));
	context->blend = static_cast<float>(
		std::clamp(obs_data_get_double(settings, "eyeblend"), 0.0, 100.0));
	context->blendPos = static_cast<float>(
		std::clamp(obs_data_get_double(settings, "eyeblendpos"), 0.0, 100.0));
	context->smoothing = static_cast<float>(
		std::clamp(obs_data_get_double(settings, "camerasmoothing"), 0.0, 100.0));
	context->smoothCrop = static_cast<float>(
		std::clamp(obs_data_get_double(settings, "smoothingcrop"), 0.0, 25.0));
	refresh_app_smoothing(context, true);

	if (context->initialized && context->surface && !needsReinit) {
		context->surface->blend = context->blend;
		context->surface->blendPos = context->blendPos;
		context->surface->overlap = context->overlap;
		publish_smoothing(context);
		return;
	}

	if (context->initialized) {
		win_openxrmirror_deinit(data);
		win_openxrmirror_init(data);
	}
}

static void win_openxrmirror_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "captureeye", 1);
	obs_data_set_default_double(settings, "eyeoverlap", 50);
	obs_data_set_default_double(settings, "eyeblend", 50);
	obs_data_set_default_double(settings, "eyeblendpos", 50);
	obs_data_set_default_double(settings, "camerasmoothing", 0);
	obs_data_set_default_double(settings, "smoothingcrop", 8);
	obs_data_set_default_double(settings, "cropleft", 0);
	obs_data_set_default_double(settings, "cropright", 0);
	obs_data_set_default_double(settings, "croptop", 0);
	obs_data_set_default_double(settings, "cropbottom", 0);
}

static uint32_t win_openxrmirror_getwidth(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;
	return context->width;
}

static uint32_t win_openxrmirror_getheight(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;
	return context->height;
}

static void win_openxrmirror_show(void *data)
{
	win_openxrmirror_init(data,
		true); // When showing do forced init without delay
}

static void win_openxrmirror_hide(void *data)
{
	win_openxrmirror_deinit(data);
}

static void *win_openxrmirror_create(obs_data_t *settings, obs_source_t *source)
{
	struct win_openxrmirror *context =
		new (std::nothrow) win_openxrmirror{};
	if (!context)
		return nullptr;
	context->source = source;

	context->initialized = false;

	context->ctx11 = nullptr;
	context->dev11 = nullptr;
	context->texture = nullptr;
	context->texCrop = nullptr;
	context->mirror_textures.clear();

	context->width = context->height = 100;

	win_openxrmirror_update(context, settings);
	return context;
}

static void win_openxrmirror_destroy(void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	win_openxrmirror_deinit(data);
	delete context;
}

static void win_openxrmirror_render(void *data, gs_effect_t *effect)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	if (context->initialized && context->surface &&
	    (context->shared_handle != context->surface->sharedHandle[0] ||
	     context->surface_generation !=
	         context->surface->surfaceGeneration.load(std::memory_order_acquire))) {
		info("Mirror surface changed; reconnecting to generation %u",
		     context->surface->surfaceGeneration.load(std::memory_order_relaxed));
		win_openxrmirror_deinit(data);
	}

	if (context->active && !context->initialized) {
		// Active & want to render but not initialized - attempt to init
		win_openxrmirror_init(data);
	}

	if (!context->texture || !context->active ||
	    context->mirror_textures.size() != kMirrorTextureCount) {
		return;
	}

	// Crop from the newest fully-copied mirror texture in the ring
	// This step is required even without cropping as the full res mirror texture is in sRGB space
	D3D11_BOX poksi = {
		context->x,
		context->y,
		0,
		context->x + context->width,
		context->y + context->height,
		1,
	};

	const uint32_t latestFrame =
		context->surface ? context->surface->lastProcessedIndex.load() : 0;

	context->ctx11->CopySubresourceRegion(
		context->texCrop.get(), 0, 0, 0, 0,
		context->mirror_textures[latestFrame % kMirrorTextureCount].get(),
		0, &poksi);
	context->ctx11->Flush();

	// Draw from shared mirror texture
	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	while (gs_effect_loop(effect, "Draw")) {
		obs_source_draw(context->texture, 0, 0, 0, 0, false);
	}
	context->last_render_tick = GetTickCount64();
	context->render_count++;
}

static void win_openxrmirror_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct win_openxrmirror *context = (win_openxrmirror *)data;

	context->active = obs_source_active(context->source);
	refresh_app_smoothing(context);

	// Heartbeat: tells the layer the plugin is alive even on frames where
	// the source itself is not rendered.
	if (context->surface)
		context->surface->frameNumber++;

	// Watchdog: a frozen or blank source should explain itself, so report
	// when the layer stops publishing new frames (and when it resumes).
	if (context->initialized && context->surface) {
		const ULONGLONG now = GetTickCount64();
		const std::uint32_t idx =
			context->surface->lastProcessedIndex.load();
		if (idx != context->last_index) {
			if (context->stall_warned)
				info("mirror frames resumed (frame index %u)",
				     idx);
			context->last_index = idx;
			context->last_index_tick = now;
			context->heartbeat_baseline =
				context->diag
					? context->diag->layerHeartbeat.load()
					: 0;
			context->stall_warned = false;
		} else if (!context->stall_warned &&
			   now - context->last_index_tick > 10000) {
			context->stall_warned = true;
			const char *cause =
				"no layer diagnostics available (older layer)";
			if (context->diag) {
				cause = context->diag->layerHeartbeat.load() !=
						context->heartbeat_baseline
					? "the VR app is alive but not feeding frames (headset idle, mirroring paused, or see the layer log)"
					: "the VR app appears to have exited or stopped rendering";
			}
			warn("no new mirror frames for 10 s (frame index stuck at %u): %s",
			     idx, cause);
		}

		if (now - context->last_health_log_tick >= 30000) {
			context->last_health_log_tick = now;
			const ULONGLONG render_age = context->last_render_tick == 0
				? 0
				: now - context->last_render_tick;
			const std::uint32_t layer_heartbeat =
				context->diag ? context->diag->layerHeartbeat.load() : 0;
			info("mirror health: active=%s, render calls=%llu, last render=%s, producer frame=%u (age %.1fs), "
			     "generation=%u, layer heartbeat=%u. Advancing counters prove transport activity, not non-black pixels; "
			     "use the Control Center Preview diagnostics log for pixel sampling",
			     context->active ? "yes" : "no",
			     (unsigned long long)context->render_count,
			     context->last_render_tick == 0 ? "never" : "recent",
			     idx,
			     (now - context->last_index_tick) / 1000.0,
			     context->surface_generation,
			     layer_heartbeat);
			if (context->last_render_tick != 0 && render_age > 5000)
				warn("OBS has not rendered this source for %.1fs even though it is initialized; check scene/source visibility",
				     render_age / 1000.0);
		}
	}
}

static bool crop_preset_changed(obs_properties_t *props, obs_property_t *p,
				obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	int sel = (int)obs_data_get_int(s, "croppreset") - 1;

	if (sel < 0 || sel >= (int)croppresets.size())
		return false;

	const crop &crop = croppresets[sel].crop;
	obs_data_set_double(s, "cropleft", std::clamp(crop.left, 0.0, 100.0));
	obs_data_set_double(s, "cropright", std::clamp(crop.right, 0.0, 100.0));
	obs_data_set_double(s, "croptop", std::clamp(crop.top, 0.0, 100.0));
	obs_data_set_double(s, "cropbottom", std::clamp(crop.bottom, 0.0, 100.0));

	return true;
}

static bool crop_preset_manual(obs_properties_t *props, obs_property_t *p,
			       obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	if (obs_data_get_int(s, "croppreset") != 0) {
		// Slider moved manually, disable preset
		obs_data_set_int(s, "croppreset", 0);
		return true;
	}
	return false;
}

static bool crop_preset_flip(obs_properties_t *props, obs_property_t *p,
			     obs_data_t *s)
{
	bool flip = obs_data_get_int(s, "captureeye") == 1;
	obs_property_set_description(obs_properties_get(props, "cropleft"),
		flip ? obs_module_text("CropLeftPercentage")
		     : obs_module_text("CropRightPercentage"));
	obs_property_set_description(obs_properties_get(props, "cropright"),
		flip ? obs_module_text("CropRightPercentage")
		     : obs_module_text("CropLeftPercentage"));
	return true;
}

static bool button_reset_callback(obs_properties_t *props, obs_property_t *p,
				  void *data)
{
	struct win_openxrmirror *context = (win_openxrmirror *)data;

	if (GetTickCount64() - 2000 < context->lastCheckTick) {
		return false;
	}

	context->lastCheckTick = GetTickCount64();
	context->initialized = false;
	win_openxrmirror_deinit(data);
	return false;
}

static obs_properties_t *win_openxrmirror_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "captureeye",
				    obs_module_text("EyeCapture"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("EyeLeft"), 0);
	obs_property_list_add_int(p, obs_module_text("EyeRight"), 1);
	obs_property_list_add_int(p, obs_module_text("EyeBoth"), 2);

	obs_property_set_modified_callback(p, crop_preset_flip);

	p = obs_properties_add_float_slider(props, "eyeoverlap",
					    obs_module_text("EyeOverlap"), 0.0,
					    100.0, 0.1);

	p = obs_properties_add_float_slider(props, "eyeblend",
					    obs_module_text("EyeBlend"), 0.0,
					    100.0, 0.1);

	p = obs_properties_add_float_slider(props, "eyeblendpos",
					    obs_module_text("EyeBlendPosition"), 0.0,
					    100.0, 0.1);

	p = obs_properties_add_float_slider(props, "camerasmoothing",
					    obs_module_text("CameraSmoothing"), 0.0,
					    100.0, 1.0);

	p = obs_properties_add_float_slider(props, "smoothingcrop",
					    obs_module_text("SmoothingCrop"), 0.0,
					    25.0, 0.5);

	p = obs_properties_add_list(props, "croppreset",
				    obs_module_text("CropPreset"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("CropPresetNone"), 0);
	int i = 1;
	for (const auto &c : croppresets) {
		obs_property_list_add_int(p, c.name, i++);
	}
	obs_property_set_modified_callback(p, crop_preset_changed);

	p = obs_properties_add_float_slider(
		props, "croptop", obs_module_text("CropTopPercentage"), 0.0, 100.0, 0.1);
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(
		props, "cropbottom", obs_module_text("CropBottomPercentage"), 0.0, 100.0, 0.1);
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(
		props, "cropleft", obs_module_text("CropLeftPercentage"), 0.0, 100.0, 0.1);
	obs_property_set_modified_callback(p, crop_preset_manual);
	p = obs_properties_add_float_slider(
		props, "cropright", obs_module_text("CropRightPercentage"), 0.0, 100.0, 0.1);
	obs_property_set_modified_callback(p, crop_preset_manual);

	p = obs_properties_add_button(props, "resetsteamvr",
				      obs_module_text("ReinitializeSource"),
				      button_reset_callback);

	return props;
}

static void load_presets(void)
{
	croppresets.clear();
	char *presets_file = NULL;
	presets_file = obs_module_file("win_openxrmirror-presets.ini");
	if (presets_file) {
		FILE *f = fopen(presets_file, "rb");
		if (f) {
			char line[512];
			unsigned int line_number = 0;
			while (fgets(line, sizeof(line), f)) {
				line_number++;
				croppreset p = {};
				if (sscanf(line, "%lf,%lf,%lf,%lf,%127[^\r\n]",
					   &p.crop.top, &p.crop.bottom, &p.crop.left,
					   &p.crop.right, p.name) == 5) {
					croppresets.push_back(p);
				} else {
					blog(LOG_WARNING,
					     "Ignoring malformed crop preset on line %u",
					     line_number);
				}
			}
			fclose(f);
		} else {
			blog(LOG_WARNING,
			     "Failed to load presets file 'win_openxrmirror-presets.ini' not found!");
		}
		bfree(presets_file);
	} else {
		blog(LOG_WARNING,
		     "Failed to load presets file 'win_openxrmirror-presets.ini' not found!");
	}
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win_openxrmirror", "en-US")

bool obs_module_load(void)
{
	blog(LOG_INFO, "plugin version %s (IPC diagnostics v%u)",
	     kPluginVersion, obs_mirror_ipc::kDiagnosticsVersion);

	obs_source_info info = {};
	info.id = "openxrmirror_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = win_openxrmirror_get_name;
	info.create = win_openxrmirror_create;
	info.destroy = win_openxrmirror_destroy;
	info.update = win_openxrmirror_update;
	info.get_defaults = win_openxrmirror_defaults;
	info.show = win_openxrmirror_show;
	info.hide = win_openxrmirror_hide;
	info.get_width = win_openxrmirror_getwidth;
	info.get_height = win_openxrmirror_getheight;
	info.video_render = win_openxrmirror_render;
	info.video_tick = win_openxrmirror_tick;
	info.get_properties = win_openxrmirror_properties;
	obs_register_source(&info);
	load_presets();
	return true;
}
