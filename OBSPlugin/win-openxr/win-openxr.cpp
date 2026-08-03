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

struct win_openxrmirror {
	obs_source_t *source;
	HANDLE map_file = nullptr;
	obs_mirror_ipc::MirrorSurfaceData *surface = nullptr;
	std::uint64_t shared_handle = 0;

	int captureeye = 1; // left = 0, right = 1, both = 2
	int croppreset;
	crop crop;

	float blend = 0.0f;
	float blendPos = 0.0f;
	float overlap = 0.0f;

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

	if (context->surface) {
		UnmapViewOfFile(context->surface);
		context->surface = nullptr;
	}
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
		warn("win_openxrmirror_init: Could not open file mapping object:  %d",
		     GetLastError());
		return;
	}

	context->surface =
		(obs_mirror_ipc::MirrorSurfaceData *)MapViewOfFile(
		context->map_file,              // handle to map object
		FILE_MAP_WRITE | FILE_MAP_READ, // read permission
		0, 0, sizeof(obs_mirror_ipc::MirrorSurfaceData));

	if (context->surface == nullptr) {
		warn("win_openxrmirror_init: Could not map view of file.");
		CloseHandle(context->map_file);
                context->map_file = nullptr;
                return;
        }

        context->surface->eyeIndex = context->captureeye;
        context->surface->blend = context->blend;
        context->surface->blendPos = context->blendPos;
        context->surface->overlap = context->overlap;

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

        context->mirror_textures = std::vector<winrt::com_ptr<ID3D11Texture2D>>();
        const std::uint64_t published_handle = context->surface->sharedHandle[0];
        if (published_handle == 0) {
            warn("win_openxrmirror_init: Mirror surface is not ready");
            return;
        }
        MemoryBarrier();

        for (UINT i = 0; i < kMirrorTextureCount; ++i) {
            const std::uint64_t shared_handle = i == 0 ? published_handle : context->surface->sharedHandle[i];

            if (shared_handle == 0) {
                warn("win_openxrmirror_init: Mirror surface handle is null");
                return;
            }

            winrt::com_ptr<IDXGIResource> copy_tex_resource_mirror = nullptr;
            hr =
                context->dev11->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(shared_handle)),
                                                   __uuidof(IDXGIResource),
                                                   copy_tex_resource_mirror.put_void());
            if (FAILED(hr) || !copy_tex_resource_mirror) {
                warn("win_openxrmirror_init: OpenSharedResource failed");
                return;
            }

            winrt::com_ptr<ID3D11Texture2D> mirror_texture;
            hr = copy_tex_resource_mirror->QueryInterface(__uuidof(ID3D11Texture2D), mirror_texture.put_void());
            if (FAILED(hr) || !mirror_texture) {
                warn("win_openxrmirror_init: copy_tex_resource_mirror->QueryInterface failed");
                return;
            }
            context->mirror_textures.push_back(mirror_texture);
        }
        MemoryBarrier();
        if (context->surface->sharedHandle[0] != published_handle) {
            warn("win_openxrmirror_init: Mirror surface changed during initialization");
            return;
        }
        context->shared_handle = published_handle;

        D3D11_TEXTURE2D_DESC desc;
        context->mirror_textures[0]->GetDesc(&desc);
        if (desc.Width == 0 || desc.Height == 0) {
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
		warn("win_openxrmirror_init: Unsupported DXGI texture format: %d",
		     desc.Format);
		return;
	}
	desc.Format = info.linear;
	info("Texture format: %d", desc.Format);
	info("Texture width: %d", desc.Width);
	info("Texture height: %d", desc.Height);
	hr = context->dev11->CreateTexture2D(&desc, NULL, context->texCrop.put());
	if (FAILED(hr)) {
		warn("win_openxrmirror_init: CreateTexture2D failed");
		return;
	}

	// Get IDXGIResource, then share handle, and open it in OBS device
	IDXGIResource *res;
	hr = context->texCrop->QueryInterface(__uuidof(IDXGIResource),
					      (void **)&res);
	if (FAILED(hr)) {
		warn("win_openxrmirror_init: QueryInterface failed");
		return;
	}

	HANDLE handle = NULL;
	hr = res->GetSharedHandle(&handle);
	res->Release();
	if (FAILED(hr)) {
		warn("win_openxrmirror_init: GetSharedHandle failed");
		return;
	}

	const std::uintptr_t handle_value = reinterpret_cast<std::uintptr_t>(handle);
	if (handle_value > std::numeric_limits<std::uint32_t>::max()) {
		warn("win_openxrmirror_init: Shared texture handle does not fit OBS's 32-bit handle API");
		return;
	}

	obs_enter_graphics();
	gs_texture_destroy(context->texture);
	context->texture =
		gs_texture_open_shared(static_cast<std::uint32_t>(handle_value));
	obs_leave_graphics();
	if (!context->texture) {
		warn("win_openxrmirror_init: OBS could not open the shared texture");
		return;
	}

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

	if (context->initialized && context->surface && !needsReinit) {
		context->surface->blend = context->blend;
		context->surface->blendPos = context->blendPos;
		context->surface->overlap = context->overlap;
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
	    context->shared_handle != context->surface->sharedHandle[0]) {
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
}

static void win_openxrmirror_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct win_openxrmirror *context = (win_openxrmirror *)data;

	context->active = obs_source_active(context->source);

	// Heartbeat: tells the layer the plugin is alive even on frames where
	// the source itself is not rendered.
	if (context->surface)
		context->surface->frameNumber++;
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
