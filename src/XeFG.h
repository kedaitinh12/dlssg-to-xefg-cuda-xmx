// XeFG frame interpolation called directly, without XeFG's proxy swap chain.
//
// libxess_fg.dll only exports the xefgSwapChain* API, but internally the swap chain drives a lower level, unexported
// C API (xefgD3D12CreateContext / Init / Execute / ...). This locates those functions inside a loaded libxess_fg.dll.
// Struct layouts are reverse engineered from libxess_fg.dll 1.3.1 (XeSS SDK 3.0.2) and checked against 1.3.4.
#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>

// Same values as xefg_swapchain_result_t: 0 = success, > 0 warning, < 0 error
using xefgd_result_t = int32_t;

constexpr xefgd_result_t XEFGD_RESULT_SUCCESS = 0;
constexpr xefgd_result_t XEFGD_RESULT_ERROR_NOT_IMPLEMENTED = -7;

// {uint32 'DX12' magic, XefgContext*} allocated by libxess_fg.dll
using xefgd_handle_t = struct xefgd_context_t*;

// Same as xefg_swapchain_init_flags_t
constexpr uint32_t XEFGD_INIT_FLAG_NONE = 0;
constexpr uint32_t XEFGD_INIT_FLAG_INVERTED_DEPTH = 1u << 0;
constexpr uint32_t XEFGD_INIT_FLAG_JITTERED_MV = 1u << 4;

// Same as xefg_swapchain_ui_mode_t
constexpr uint32_t XEFGD_UI_MODE_AUTO = 0;
constexpr uint32_t XEFGD_UI_MODE_BACKBUFFER_HUDLESS = 4;

enum xefgd_logging_level_t : int32_t
{
	XEFGD_LOGGING_LEVEL_DEBUG = 0,
	XEFGD_LOGGING_LEVEL_INFO = 1,
	XEFGD_LOGGING_LEVEL_WARNING = 2,
	XEFGD_LOGGING_LEVEL_ERROR = 3
};

using xefgd_log_callback_t = void (*)(const char* message, xefgd_logging_level_t level, void* userData);

#pragma pack(push, 8)

struct xefgd_2d_t
{
	uint32_t x;
	uint32_t y;
};

struct xefgd_init_params_t
{
	// Size of the generated frames, normally the back buffer size
	xefgd_2d_t outputResolution;
	// DXGI_FORMAT of the outputs, must be usable as a typed UAV
	uint32_t backBufferFormat;
	// XEFGD_INIT_FLAG_*
	uint32_t initFlags;
	// Above 1 Init swaps the 2x network for its multi frame variant, unless a model is forced with OverrideModel
	uint32_t maxInterpolatedFrames;
	uint32_t creationNodeMask;
	uint32_t visibleNodeMask;
	uint32_t reserved0;
	ID3D12Heap* pTempBufferHeap;
	uint64_t bufferHeapOffset;
	ID3D12Heap* pTempTextureHeap;
	uint64_t textureHeapOffset;
	ID3D12PipelineLibrary* pPipelineLibrary;
	// XEFGD_UI_MODE_*, only applied when xefgd_exec_params_t::uiCompositionEnabled is set
	uint32_t uiMode;
	uint32_t reserved1;
};

// One input texture, in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE during Execute (XeFG transitions nothing)
struct xefgd_resource_t
{
	ID3D12Resource* pResource;
	xefgd_2d_t base; // top left of the valid region
	xefgd_2d_t size; // must be non zero
};

// One real frame
struct xefgd_frame_t
{
	xefgd_resource_t backBuffer;    // final color, with UI
	xefgd_resource_t depth;
	xefgd_resource_t motionVectors; // only next's are read
	xefgd_resource_t uiTexture;
	xefgd_resource_t hudlessColor;
	float viewMatrix[16];           // row major, all zero disables scene change detection
	float projectionMatrix[16];     // row major, no jitter
	float outputResolution[2];
	float jitterOffset[2];
	float motionVectorScale[2];     // must be non zero
	uint32_t resetHistory;
	float frameRenderTime;          // ms
};

// Generated frame i is at t = (i + 1) / (numInterpolatedFrames + 1) between prev and next.
// A call writes frames [firstFrameIndex, firstFrameIndex + frameCount), frameCount 0 meaning all of them. The
// expensive per pair work only runs in a call starting at 0, later calls reuse it and must pass the same frames and
// the same ppOutputs array.
struct xefgd_exec_params_t
{
	xefgd_frame_t prev;
	xefgd_frame_t next;
	// numInterpolatedFrames textures in the init format, ALLOW_UNORDERED_ACCESS, in UNORDERED_ACCESS
	ID3D12Resource* const* ppOutputs;
	const xefgd_2d_t* pOutputBases;
	uint8_t numInterpolatedFrames;
	uint8_t reserved0[3];
	uint32_t networkScales; // 0 = default
	ID3D12DescriptorHeap* pDescriptorHeap;
	uint32_t descriptorHeapOffsetInBytes;
	uint32_t uiCompositionEnabled;
	uint32_t firstFrameIndex;
	uint32_t frameCount;
};

#pragma pack(pop)

static_assert(sizeof(xefgd_init_params_t) == 80);
static_assert(offsetof(xefgd_init_params_t, pTempBufferHeap) == 32);
static_assert(offsetof(xefgd_init_params_t, uiMode) == 72);
static_assert(sizeof(xefgd_resource_t) == 24);
static_assert(sizeof(xefgd_frame_t) == 280);
static_assert(offsetof(xefgd_frame_t, viewMatrix) == 120);
static_assert(offsetof(xefgd_frame_t, jitterOffset) == 256);
static_assert(offsetof(xefgd_frame_t, motionVectorScale) == 264);
static_assert(offsetof(xefgd_exec_params_t, ppOutputs) == 560);
static_assert(offsetof(xefgd_exec_params_t, numInterpolatedFrames) == 576);
static_assert(offsetof(xefgd_exec_params_t, networkScales) == 580);
static_assert(offsetof(xefgd_exec_params_t, pDescriptorHeap) == 584);
static_assert(offsetof(xefgd_exec_params_t, uiCompositionEnabled) == 596);
static_assert(offsetof(xefgd_exec_params_t, firstFrameIndex) == 600);
static_assert(sizeof(xefgd_exec_params_t) == 608);

namespace XeFG
{
	// Picks a compatible libxess_fg.dll, loaded ones first, then the one next to this dll. Only searches once
	bool Load();

	// The module Load picked, null until one is resolved
	HMODULE Module();

	// Locates the hidden API in module, false when it isn't a build we can drive. Replaces the previous module's API
	bool Resolve(HMODULE module);

	// Process wide settings, they apply to contexts created afterwards
	void SetMaxInterpolatedFrames(uint32_t maxFrames); // the library's default cap is 3
	void OverrideModel(int32_t modelId);
	// XeFG draws its own tags into the generated frames, from the next Execute on. Nothing turns it on yet, it's here
	// for whatever ends up exposing the debug view
	void SetDebugTagFrames(bool enable);

	xefgd_result_t CreateContext(ID3D12Device* device, xefgd_handle_t* context);
	void SetLoggingCallback(xefgd_handle_t context, xefgd_logging_level_t level, xefgd_log_callback_t callback);
	xefgd_result_t Init(xefgd_handle_t context, const xefgd_init_params_t& params);
	// Leaves XeFG's descriptor heap bound
	xefgd_result_t Execute(xefgd_handle_t context, ID3D12GraphicsCommandList* cmdList, const xefgd_exec_params_t& params);
	xefgd_result_t Destroy(xefgd_handle_t context);
} // namespace XeFG
