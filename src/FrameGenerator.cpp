#include "FrameGenerator.h"
#include "Log.h"
#include "MfgKernels.h"
#include "Util.h"

#include <DirectXMath.h>

#include <cmath>
#include <numbers>
#include <string>
#include <utility>

using namespace DirectX;

// States of the DLSSG resources when EvaluateFeature is called (same as dlssg-to-fsr3 assumes)
constexpr auto BackbufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto InputState = D3D12_RESOURCE_STATE_COPY_DEST; // depth, mvecs, hudless
constexpr auto OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// What XeFG wants for its inputs, our copies always rest in this state
constexpr auto XeFGInputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// XeFG stock network. libxess_fg.dll does not ship its multi frame variant (16), which Init would switch to for
// more than one generated frame, so it gets forced. The XMX port publishes 16, and then Init loads the cheaper MFG
// model: one optical flow field for the whole batch instead of one per generated frame

FrameGenerator::Context::~Context()
{
	if (handle)
	{
		if (auto result = XeFG::Destroy(handle); result != XEFGD_RESULT_SUCCESS)
			Log::Warn("XeFG Destroy error: {}", result);
	}
}

static void XeFGLogCallback(const char* message, xefgd_logging_level_t level, void*)
{
	switch (level)
	{
	case XEFGD_LOGGING_LEVEL_ERROR:
		Log::Error("XeFG: {}", message);
		break;
	case XEFGD_LOGGING_LEVEL_WARNING:
		Log::Warn("XeFG: {}", message);
		break;
	default:
		Log::Info("XeFG: {}", message);
		break;
	}
}

static ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, D3D12_RESOURCE_DESC desc,
	D3D12_RESOURCE_STATES state, const wchar_t* name, const char* purpose)
{
	ComPtr<ID3D12Resource> texture;
	D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };

	if (HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
		IID_PPV_ARGS(&texture));
		FAILED(hr))
	{
		// The desc is built from the game's resource, so it has to be in the message to identify the culprit
		Log::Error("CreateCommittedResource error: {:X} for {}, {}x{}x{}, mips {}, format {}, flags {:X}, layout {}, samples {}/{}",
			(uint32_t)hr, purpose, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels,
			(uint32_t)desc.Format, (uint32_t)desc.Flags, (uint32_t)desc.Layout, desc.SampleDesc.Count,
			desc.SampleDesc.Quality);
		return nullptr;
	}

	texture->SetName(name);
	return texture;
}

// Depth copies become plain color textures where the format allows, anything can read those through SRVs
static void MakeReadableDepth(D3D12_RESOURCE_DESC& desc)
{
	switch (desc.Format)
	{
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
		desc.Format = TypedFormat(desc.Format);
		desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		break;
	default:
		break;
	}
}

// Copies source into copy, (re)created when the source changes, the copy is left in XeFGInputState
static bool Capture(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
	D3D12_RESOURCE_STATES sourceState, ComPtr<ID3D12Resource>& copy, const char* purpose, bool isDepth = false)
{
	if (!source)
	{
		copy.Reset();
		return false;
	}

	auto desc = source->GetDesc();

	// The copy only ever rests in XeFGInputState and gets read through an SRV, so none of the source's flags are
	// needed. Inheriting them can make CreateCommittedResource reject the desc: ALLOW_CROSS_ADAPTER needs a cross
	// adapter heap, ALLOW_SIMULTANEOUS_ACCESS can't be combined with a depth resource, and ALLOW_UNORDERED_ACCESS
	// can't be used with MSAA. A non committed layout (row major, 64KB swizzle) can be rejected the same way.
	desc.Flags &= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	if (isDepth)
		MakeReadableDepth(desc);

	if (copy)
	{
		auto copyDesc = copy->GetDesc();

		// The old one can still be in use by the GPU
		if (copyDesc.Dimension != desc.Dimension || copyDesc.Width != desc.Width || copyDesc.Height != desc.Height ||
			copyDesc.DepthOrArraySize != desc.DepthOrArraySize || copyDesc.MipLevels != desc.MipLevels ||
			copyDesc.Format != desc.Format || copyDesc.SampleDesc.Count != desc.SampleDesc.Count ||
			copyDesc.SampleDesc.Quality != desc.SampleDesc.Quality || copyDesc.Layout != desc.Layout ||
			copyDesc.Flags != desc.Flags)
		{
			DeferredDestroy::Add(std::make_shared<ComPtr<ID3D12Resource>>(std::move(copy)));
		}
	}

	if (!copy)
		copy = CreateTexture(device, desc, XeFGInputState, L"dlssg-to-xefg input copy", purpose);

	if (!copy)
		return false;

	Copy(cmdList, copy.Get(), XeFGInputState, source, sourceState);
	return true;
}

static xefgd_resource_t Subrect(const NGXParameter& params, const char* name, ID3D12Resource* resource)
{
	auto key = [name](const char* field) { return std::format("DLSSG.{}Subrect{}", name, field); };

	xefgd_resource_t subrect{};
	subrect.base = { params.GetOr<uint32_t>(key("BaseX").c_str(), 0), params.GetOr<uint32_t>(key("BaseY").c_str(), 0) };
	subrect.size = { params.GetOr<uint32_t>(key("Width").c_str(), 0), params.GetOr<uint32_t>(key("Height").c_str(), 0) };

	if (subrect.size.x == 0 || subrect.size.y == 0)
	{
		auto desc = resource->GetDesc();
		subrect.base = {};
		subrect.size = { (uint32_t)desc.Width, desc.Height };
	}

	return subrect;
}

static XMFLOAT3 GetVector(const NGXParameter& params, const char* name)
{
	auto key = [name](const char* axis) { return std::format("DLSSG.{}{}", name, axis); };
	return { params.GetOr(key("X").c_str(), 0.0f), params.GetOr(key("Y").c_str(), 0.0f),
			 params.GetOr(key("Z").c_str(), 0.0f) };
}

// XeFG's scene change detection uses the forward axis
static void GetViewMatrix(const NGXParameter& params, float* out)
{
	const XMFLOAT3 pos = GetVector(params, "CameraPos");
	const XMFLOAT3 right = GetVector(params, "CameraRight");
	const XMFLOAT3 up = GetVector(params, "CameraUp");
	const XMFLOAT3 forward = GetVector(params, "CameraFwd");

	// All zero disables XeFG's scene change detection
	if (forward.x == 0.0f && forward.y == 0.0f && forward.z == 0.0f)
		return;

	XMVECTOR p = XMLoadFloat3(&pos);
	XMMATRIX view = { XMVectorSet(right.x, up.x, forward.x, 0.0f), XMVectorSet(right.y, up.y, forward.y, 0.0f),
					  XMVectorSet(right.z, up.z, forward.z, 0.0f),
					  XMVectorSet(-XMVectorGetX(XMVector3Dot(p, XMLoadFloat3(&right))),
								  -XMVectorGetX(XMVector3Dot(p, XMLoadFloat3(&up))),
								  -XMVectorGetX(XMVector3Dot(p, XMLoadFloat3(&forward))), 1.0f) };

	memcpy(out, view.r, sizeof(view));
}

// Near/far/fov from a perspective view to clip matrix, dlssg-to-fsr3's approach. Near and far come out in depth
// buffer order
static bool CameraFromViewToClip(const NGXParameter& params, float& cameraNear, float& cameraFar, float& fov)
{
	float(*viewToClip)[4] = nullptr;
	params.Get("DLSSG.CameraViewToClip", reinterpret_cast<void**>(&viewToClip));

	if (params.GetOr<uint32_t>("DLSSG.OrthoProjection", 0) || !viewToClip)
		return false;

	// Some games (RTX Remix ones) pass zero or identity
	float m[4][4] = {};
	if (memcmp(viewToClip, m, sizeof(m)) == 0)
		return false;

	m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
	if (memcmp(viewToClip, m, sizeof(m)) == 0)
		return false;

	const double b = viewToClip[1][1];
	const double c = viewToClip[2][2];
	const double d = viewToClip[3][2];
	const double e = viewToClip[2][3];

	if (e < 0.0)
	{
		cameraNear = (float)(c == 0.0 ? 0.0 : d / c);
		cameraFar = (float)(d / (c + 1.0));
	}
	else
	{
		cameraNear = (float)(c == 0.0 ? 0.0 : -d / c);
		cameraFar = (float)(-d / (c - 1.0));
	}

	fov = (float)(2.0 * std::atan(1.0 / b));

	return cameraNear >= 0.0f && cameraFar >= 0.0f;
}

// Unreal's executables live in Binaries\<platform> and end with -Shipping
static bool IsUnrealEngine()
{
	static const bool isUnreal = []
		{
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			_wcslwr_s(path);

			std::wstring_view exe = path;
			return exe.find(L"\\binaries\\win64\\") != exe.npos || exe.find(L"\\binaries\\wingdk\\") != exe.npos ||
				exe.ends_with(L"-shipping.exe");
		}();

	return isUnreal;
}

// Left handed perspective matrix (row vectors) matching the depth buffer: inverted depth swaps near and far,
// cameraFar == 0 means an infinite far plane
static void GetProjectionMatrix(const NGXParameter& params, bool depthInverted, float aspectRatio, float* out)
{
	float cameraNear = 0.0f;
	float cameraFar = 0.0f;
	float fov = 0.0f;

	// Unreal doesn't pass a correct CameraViewToClip, its separate camera values are fine
	if (!IsUnrealEngine() && CameraFromViewToClip(params, cameraNear, cameraFar, fov))
	{
		if (depthInverted)
			std::swap(cameraNear, cameraFar);
	}
	else
	{
		cameraNear = params.GetOr("DLSSG.CameraNear", 0.0f);
		cameraFar = params.GetOr("DLSSG.CameraFar", 0.0f);
		fov = params.GetOr("DLSSG.CameraFOV", 0.0f);

		// Some games pass degrees, some radians
		if (fov > 10.0f)
			fov *= std::numbers::pi_v<float> / 180.0f;
	}

	if (float paramAspectRatio = params.GetOr("DLSSG.CameraAspectRatio", 0.0f); paramAspectRatio > 0.0f)
		aspectRatio = paramAspectRatio;

	if (cameraNear <= 0.0f || cameraFar < 0.0f || fov <= 0.0f || aspectRatio <= 0.0f)
		return;

	const bool infinite = cameraFar == 0.0f;
	const float h = 1.0f / std::tan(fov * 0.5f);
	const float w = h / aspectRatio;

	// z_ndc = (a * z + b) / z
	float a;
	float b;

	if (infinite)
	{
		a = depthInverted ? 0.0f : 1.0f;
		b = depthInverted ? cameraNear : -cameraNear;
	}
	else
	{
		if (XMScalarNearEqual(cameraNear, cameraFar, 0.00001f))
			cameraFar++;

		const float range = cameraFar - cameraNear;
		a = depthInverted ? -cameraNear / range : cameraFar / range;
		b = depthInverted ? cameraNear * cameraFar / range : -cameraNear * cameraFar / range;
	}

	const float projection[16] = { w, 0, 0, 0, 0, h, 0, 0, 0, 0, a, 1, 0, 0, b, 0 };
	memcpy(out, projection, sizeof(projection));
}

bool FrameGenerator::CreateContext()
{
	auto newContext = std::make_shared<Context>();

	// Publishing the ported model patches libxess_fg.dll process wide, so it runs before the first context
	const bool mfgModel = MfgKernels::Install(XeFG::Module());

	// Process wide, they also affect other XeFG contexts. Must happen before the context picks its model
	XeFG::SetMaxInterpolatedFrames(MaxFrames);
	XeFG::OverrideModel(mfgModel ? MfgKernels::MfgModel : MfgKernels::Dp4aModel);

	if (auto result = XeFG::CreateContext(device.Get(), &newContext->handle); result != XEFGD_RESULT_SUCCESS)
	{
		Log::Error("XeFG CreateContext error: {}", result);
		return false;
	}

	XeFG::SetLoggingCallback(newContext->handle, XEFGD_LOGGING_LEVEL_INFO, XeFGLogCallback);

	xefgd_init_params_t initParams{};
	initParams.outputResolution = { settings.width, settings.height };
	initParams.backBufferFormat = settings.format;
	initParams.initFlags = settings.initFlags;
	initParams.maxInterpolatedFrames = MaxFrames;
	initParams.uiMode = settings.uiMode;

	Log::Info("Creating XeFG context: {}x{}, format: {}, flags: {:X}, ui mode: {}, model: {}", settings.width, settings.height,
		(uint32_t)settings.format, settings.initFlags, settings.uiMode, mfgModel ? MfgKernels::MfgModel : MfgKernels::Dp4aModel);

	if (auto result = XeFG::Init(newContext->handle, initParams); result != XEFGD_RESULT_SUCCESS)
	{
		Log::Error("XeFG Init error: {}", result);
		return false;
	}

	context = std::move(newContext);
	return true;
}

void FrameGenerator::CaptureRealFrame(ID3D12GraphicsCommandList* cmdList, const NGXParameter& params)
{
	static bool warnedCameraMotion = false;
	if (params.GetOr<uint32_t>("DLSSG.CameraMotionIncluded", 1) == 0 && !std::exchange(warnedCameraMotion, true))
		Log::Warn("DLSSG.CameraMotionIncluded is 0, the camera's motion is missing from the motion vectors");

	auto backbuffer = params.GetOr<ID3D12Resource*>("DLSSG.Backbuffer", nullptr);
	auto hudless = params.GetOr<ID3D12Resource*>("DLSSG.HUDLess", nullptr);
	auto depth = params.GetOr<ID3D12Resource*>("DLSSG.Depth", nullptr);
	auto mvecs = params.GetOr<ID3D12Resource*>("DLSSG.MVecs", nullptr);
	auto output = params.GetOr<ID3D12Resource*>("DLSSG.OutputInterpolated", nullptr);

	const bool depthInverted = params.GetOr<uint32_t>("DLSSG.DepthInverted", 0) != 0;

	Settings wanted;
	auto outputDesc = output->GetDesc();
	wanted.width = (uint32_t)outputDesc.Width;
	wanted.height = outputDesc.Height;
	wanted.format = TypedFormat(outputDesc.Format);
	wanted.initFlags = (depthInverted ? XEFGD_INIT_FLAG_INVERTED_DEPTH : 0) |
		(params.GetOr<uint32_t>("DLSSG.MvecJittered", 0) ? XEFGD_INIT_FLAG_JITTERED_MV : 0);
	wanted.uiMode = hudless ? XEFGD_UI_MODE_BACKBUFFER_HUDLESS : XEFGD_UI_MODE_AUTO;

	if (wanted != settings)
	{
		settings = wanted;
		createFailed = false;

		if (context)
			DeferredDestroy::Add(std::move(context));
	}

	if (!context && !createFailed)
		createFailed = !CreateContext();

	if (!context)
		return;

	context->current ^= 1;
	auto& frame = context->frames[context->current];
	auto& previous = context->frames[context->current ^ 1];

	bool captured = Capture(device.Get(), cmdList, backbuffer, BackbufferState, frame.backbuffer, "Backbuffer");
	captured &= Capture(device.Get(), cmdList, depth, InputState, frame.depth, "Depth copy", true);
	captured &= Capture(device.Get(), cmdList, mvecs, InputState, frame.mvecs, "MVecs copy");
	Capture(device.Get(), cmdList, hudless, InputState, frame.hudless, "HUDLess copy");

	auto& data = frame.data;
	data = {};

	if (captured)
	{
		data.backBuffer = Subrect(params, "Backbuffer", backbuffer);
		data.depth = Subrect(params, "Depth", depth);
		data.motionVectors = Subrect(params, "MVecs", mvecs);

		if (hudless)
			data.hudlessColor = Subrect(params, "HUDLess", hudless);
	}

	// The outputs are full size, XeFG writes its region at the back buffer's offset to keep letterboxing aligned
	std::ranges::fill(context->outputBases, data.backBuffer.base);

	GetViewMatrix(params, data.viewMatrix);
	GetProjectionMatrix(params, depthInverted, (float)settings.width / (float)settings.height,
		data.projectionMatrix);

	data.outputResolution[0] = (float)settings.width;
	data.outputResolution[1] = (float)settings.height;
	data.jitterOffset[0] = params.GetOr("DLSSG.JitterOffsetX", 0.0f);
	data.jitterOffset[1] = params.GetOr("DLSSG.JitterOffsetY", 0.0f);

	// Unlike Streamline's constant, the NGX parameter already converts to pixels
	data.motionVectorScale[0] = params.GetOr("DLSSG.MvecScaleX", 1.0f);
	data.motionVectorScale[1] = params.GetOr("DLSSG.MvecScaleY", 1.0f);

	if (data.motionVectorScale[0] == 0.0f || data.motionVectorScale[1] == 0.0f)
		data.motionVectorScale[0] = data.motionVectorScale[1] = 1.0f;

	const bool reset = params.GetOr<uint32_t>("DLSSG.Reset", 0) != 0;
	data.resetHistory = reset;

	// XeFG needs both frames to have hudless or neither
	context->pairValid = captured && previous.valid && !reset && (frame.hudless != nullptr) == (previous.hudless != nullptr);
	frame.valid = captured;
}

FrameGenerator::InterpolateResult FrameGenerator::Interpolate(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_DESC outputDesc,
	uint32_t frameIndex, uint32_t frameCount)
{
	if (!context || !context->pairValid)
		return InterpolateResult::Skipped;

	outputDesc.Format = settings.format;
	outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	// The game's output can live in a layout a committed UAV texture can't use
	outputDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	for (uint32_t i = 0; i < frameCount; i++)
	{
		if (!context->outputs[i])
			context->outputs[i] = CreateTexture(device.Get(), outputDesc, OutputState, L"dlssg-to-xefg output",
				"XeFG output");

		if (!context->outputs[i])
			return InterpolateResult::Failed;
	}

	auto fill = [this](const Frame& frame, xefgd_frame_t& out)
		{
			out = frame.data;
			out.backBuffer.pResource = frame.backbuffer.Get();
			out.depth.pResource = frame.depth.Get();
			out.motionVectors.pResource = frame.mvecs.Get();
			out.hudlessColor.pResource = frame.hudless.Get();
		};

	xefgd_exec_params_t params{};
	fill(context->frames[context->current ^ 1], params.prev);
	fill(context->frames[context->current], params.next);

	// XeFG wants the same array for every frame of a pair
	static_assert(sizeof(ComPtr<ID3D12Resource>) == sizeof(ID3D12Resource*));
	params.ppOutputs = reinterpret_cast<ID3D12Resource* const*>(context->outputs);
	params.pOutputBases = context->outputBases;
	params.numInterpolatedFrames = (uint8_t)frameCount;
	params.uiCompositionEnabled = settings.uiMode != XEFGD_UI_MODE_AUTO;

	// Frame 1 runs the per pair work and generates its frame, the rest only generate theirs
	params.firstFrameIndex = frameIndex - 1;
	params.frameCount = 1;

	auto result = XeFG::Execute(context->handle, cmdList, params);

	if (result == XEFGD_RESULT_SUCCESS)
		return InterpolateResult::Interpolated;

	if (result > 0)
	{
		// Warnings, like prev and next areas differing after a dynamic resolution change
		Log::Debug("XeFG Execute warning: {}, frame {}/{}", result, frameIndex, frameCount);
		return InterpolateResult::Skipped;
	}

	Log::Error("XeFG Execute error: {}, frame {}/{}", result, frameIndex, frameCount);
	context->pairValid = false;
	return InterpolateResult::Failed;
}

NGXResult FrameGenerator::Evaluate(ID3D12GraphicsCommandList* cmdList, const NGXParameter& params)
{
	auto backbuffer = params.GetOr<ID3D12Resource*>("DLSSG.Backbuffer", nullptr);
	auto output = params.GetOr<ID3D12Resource*>("DLSSG.OutputInterpolated", nullptr);
	auto outputReal = params.GetOr<ID3D12Resource*>("DLSSG.OutputReal", nullptr);

	if (!backbuffer || !output)
	{
		Log::Error("Missing backbuffer or output");
		return NGX_FAIL_INVALID_PARAMETER;
	}

	if (outputReal)
		Copy(cmdList, outputReal, OutputState, backbuffer, BackbufferState);

	if (!params.GetOr<uint32_t>("DLSSG.EnableInterp", 1))
	{
		if (context)
			context->pairValid = false;

		return NGX_SUCCESS;
	}

	if (!params.GetOr<ID3D12Resource*>("DLSSG.Depth", nullptr) ||
		!params.GetOr<ID3D12Resource*>("DLSSG.MVecs", nullptr))
	{
		Log::Error("Missing depth or motion vectors");
		return NGX_FAIL_INVALID_PARAMETER;
	}

	// 1 based index of the generated frame and the number of generated frames between two real frames
	const uint32_t frameCount = std::clamp(params.GetOr<uint32_t>("DLSSG.MultiFrameCount", 1), 1u, MaxFrames);
	const uint32_t frameIndex = std::clamp(params.GetOr<uint32_t>("DLSSG.MultiFrameIndex", 1), 1u, frameCount);

	if (frameIndex == 1)
		CaptureRealFrame(cmdList, params);

	const auto result = Interpolate(cmdList, output->GetDesc(), frameIndex, frameCount);

	// Without a usable pair (first frame, reset) the real frame is shown again, that's not an error
	if (result == InterpolateResult::Interpolated)
		Copy(cmdList, output, OutputState, context->outputs[frameIndex - 1].Get(), OutputState);
	else
		Copy(cmdList, output, OutputState, backbuffer, BackbufferState);

	return result == InterpolateResult::Failed ? NGX_FAIL : NGX_SUCCESS;
}
