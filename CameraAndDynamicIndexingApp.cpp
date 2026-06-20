//***************************************************************************************
// CameraAndDynamicIndexingApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/Camera.h"
#include "FrameResource.h"
#include "LoadM3d.h"
// FBX 파일을 읽기 위해 Assimp 기반 로더를 추가한다.
#include "AssimpLoader.h"

// FBX 로딩 실패 메시지를 그대로 예외로 넘길 때 사용한다.
#include <stdexcept>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <limits>
#include <vector>
#include <wincodec.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

const int gNumFrameResources = 3;
// 셰이더의 gDiffuseMap 배열 크기와 맞춘 고정 SRV 개수이다.
// FBX마다 material 개수가 달라도 PSO 생성이 실패하지 않게 넉넉하게 잡는다.
const UINT gNumTextureDescriptors = 64;

namespace
{
	HRESULT GetWicFactory(ComPtr<IWICImagingFactory>& factory)
	{
		static ComPtr<IWICImagingFactory> sFactory;
		static bool sComInitialized = false;

		if(sFactory == nullptr)
		{
			if(!sComInitialized)
			{
				const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				sComInitialized = true;

				if(FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
					return initResult;
			}

			const HRESULT hr = CoCreateInstance(
				CLSID_WICImagingFactory,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(sFactory.GetAddressOf()));

			if(FAILED(hr))
				return hr;
		}

		factory = sFactory;
		return S_OK;
	}

	bool HasPathSeparator(const std::string& path)
	{
		return path.find('/') != std::string::npos || path.find('\\') != std::string::npos;
	}

	std::string TextureNameFromMapName(const std::string& mapName)
	{
		const size_t slash = mapName.find_last_of("/\\");
		const size_t dot = mapName.find_last_of('.');

		if(dot == std::string::npos || (slash != std::string::npos && dot < slash))
			return mapName;

		return mapName.substr(0, dot);
	}

	bool IsDdsTextureName(const std::string& filename)
	{
		const size_t slash = filename.find_last_of("/\\");
		const size_t dot = filename.find_last_of('.');

		if(dot == std::string::npos || (slash != std::string::npos && dot < slash))
			return false;

		std::string extension = filename.substr(dot);
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char ch)
			{
				return (char)std::tolower(ch);
			});

		return extension == ".dds";
	}

	std::wstring ResolveTextureFilename(const std::string& mapName)
	{
		const std::filesystem::path sourcePath(AnsiToWString(mapName));

		if(sourcePath.is_absolute() || HasPathSeparator(mapName))
			return sourcePath.wstring();

		const std::filesystem::path texturesPath = std::filesystem::path(L"../../Textures") / sourcePath;
		std::error_code error;
		if(std::filesystem::exists(texturesPath, error))
			return texturesPath.wstring();

		const std::filesystem::path modelsPath = std::filesystem::path(L"Models") / sourcePath;
		error.clear();
		if(std::filesystem::exists(modelsPath, error))
			return modelsPath.wstring();

		return texturesPath.wstring();
	}

	HRESULT CreateWICTextureFromFile12(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* commandList,
		const wchar_t* filename,
		ComPtr<ID3D12Resource>& texture,
		ComPtr<ID3D12Resource>& textureUploadHeap)
	{
		if(device == nullptr || commandList == nullptr || filename == nullptr)
			return E_INVALIDARG;

		ComPtr<IWICImagingFactory> factory;
		HRESULT hr = GetWicFactory(factory);
		if(FAILED(hr))
			return hr;

		ComPtr<IWICBitmapDecoder> decoder;
		hr = factory->CreateDecoderFromFilename(
			filename,
			nullptr,
			GENERIC_READ,
			WICDecodeMetadataCacheOnLoad,
			decoder.GetAddressOf());
		if(FAILED(hr))
			return hr;

		ComPtr<IWICBitmapFrameDecode> frame;
		hr = decoder->GetFrame(0, frame.GetAddressOf());
		if(FAILED(hr))
			return hr;

		UINT width = 0;
		UINT height = 0;
		hr = frame->GetSize(&width, &height);
		if(FAILED(hr))
			return hr;

		if(width == 0 || height == 0)
			return E_FAIL;

		ComPtr<IWICFormatConverter> converter;
		hr = factory->CreateFormatConverter(converter.GetAddressOf());
		if(FAILED(hr))
			return hr;

		hr = converter->Initialize(
			frame.Get(),
			GUID_WICPixelFormat32bppRGBA,
			WICBitmapDitherTypeNone,
			nullptr,
			0.0,
			WICBitmapPaletteTypeCustom);
		if(FAILED(hr))
			return hr;

		const UINT64 rowPitch64 = static_cast<UINT64>(width) * 4;
		const UINT64 imageSize64 = rowPitch64 * height;

		if(rowPitch64 > (std::numeric_limits<UINT>::max)() ||
			imageSize64 > (std::numeric_limits<UINT>::max)())
		{
			return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
		}

		const UINT rowPitch = static_cast<UINT>(rowPitch64);
		const UINT imageSize = static_cast<UINT>(imageSize64);
		std::vector<std::uint8_t> pixels(imageSize);

		hr = converter->CopyPixels(nullptr, rowPitch, imageSize, pixels.data());
		if(FAILED(hr))
			return hr;

		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		textureDesc.Width = width;
		textureDesc.Height = height;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.SampleDesc.Quality = 0;
		textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		hr = device->CreateCommittedResource(
			&defaultHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(texture.GetAddressOf()));
		if(FAILED(hr))
			return hr;

		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.Get(), 0, 1);

		auto uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
		hr = device->CreateCommittedResource(
			&uploadHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&uploadBufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(textureUploadHeap.GetAddressOf()));
		if(FAILED(hr))
			return hr;

		D3D12_SUBRESOURCE_DATA textureData = {};
		textureData.pData = pixels.data();
		textureData.RowPitch = rowPitch;
		textureData.SlicePitch = imageSize;

		UpdateSubresources(commandList, texture.Get(), textureUploadHeap.Get(), 0, 0, 1, &textureData);
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			texture.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

		return S_OK;
	}
}

struct SkinnedModelInstance
{
	// 하나의 FBX 모델 인스턴스가 현재 재생 중인 animation과 최종 bone transform을 들고 있다.
	SkinnedData* SkinnedInfo = nullptr;
	std::vector<XMFLOAT4X4> FinalTransforms;
	std::string ClipName;
	float TimePos = 0.0f;

	void UpdateSkinnedAnimation(float dt)
	{
		// 매 프레임 시간을 누적하고 clip 끝에 도달하면 처음부터 다시 재생한다.
		TimePos += dt;

		if(TimePos > SkinnedInfo->GetClipEndTime(ClipName))
			TimePos = 0.0f;

		// CPU에서 이번 프레임의 최종 bone matrix를 계산해서 SkinnedCB에 복사할 준비를 한다.
		SkinnedInfo->GetFinalTransforms(ClipName, TimePos, FinalTransforms);
	}
};

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
	RenderItem() = default;
    RenderItem(const RenderItem& rhs) = delete;

	bool Visible = true;

	BoundingBox Bounds; // 바운딩 박스는 피킹을 위해 필요

    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    XMFLOAT4X4 World = MathHelper::Identity4x4();

	XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

	// Dirty flag indicating the object data has changed and we need to update the constant buffer.
	// Because we have an object cbuffer for each FrameResource, we have to apply the
	// update to each FrameResource.  Thus, when we modify obect data we should set 
	// NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
	int NumFramesDirty = gNumFrameResources;

	// Index into GPU constant buffer corresponding to the ObjectCB for this render item.
	UINT ObjCBIndex = -1;

	Material* Mat = nullptr;
	MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

	// DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;

	// 스키닝 모델만 사용하는 constant buffer index와 animation instance 포인터이다.
	UINT SkinnedCBIndex = -1;
	SkinnedModelInstance* SkinnedModelInst = nullptr;
};

enum class RenderLayer : int
{
	Opaque = 0,
	// FBX 스키닝 모델은 별도 input layout/PSO를 사용하므로 render layer도 따로 둔다.
	SkinnedOpaque,
	Highlight,
	// 스키닝 모델에서 피킹한 삼각형은 skinned highlight PSO로 다시 그린다.
	SkinnedHighlight,
	Count
};

static BoundingBox CalculateBounds(const GeometryGenerator::MeshData& MeshData)
{
	XMFLOAT3 minPoint(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
	XMFLOAT3 maxPoint(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

	XMVECTOR minVector = XMLoadFloat3(&minPoint);
	XMVECTOR maxVector = XMLoadFloat3(&maxPoint);

	for(const auto& vertex : MeshData.Vertices)
	{
		XMVECTOR position = XMLoadFloat3(&vertex.Position);
		minVector = XMVectorMin(minVector, position);
		maxVector = XMVectorMax(maxVector, position);
	}

	BoundingBox bounds;
	XMStoreFloat3(&bounds.Center, 0.5f * (minVector + maxVector));
	XMStoreFloat3(&bounds.Extents, 0.5f * (maxVector - minVector));

	return bounds;
}

static BoundingBox CalculateBounds(
	const std::vector<M3DLoader::SkinnedVertex>& vertices,
	const M3DLoader::Subset& subset)
{
	// FBX subset별 local-space bounding box를 만들어 피킹 전 빠른 충돌 검사에 사용한다.
	XMFLOAT3 minPoint(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
	XMFLOAT3 maxPoint(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

	XMVECTOR minVector = XMLoadFloat3(&minPoint);
	XMVECTOR maxVector = XMLoadFloat3(&maxPoint);

	for(UINT i = 0; i < subset.VertexCount; ++i)
	{
		const auto& vertex = vertices[subset.VertexStart + i];
		XMVECTOR position = XMLoadFloat3(&vertex.Pos);
		minVector = XMVectorMin(minVector, position);
		maxVector = XMVectorMax(maxVector, position);
	}

	BoundingBox bounds;
	XMStoreFloat3(&bounds.Center, 0.5f * (minVector + maxVector));
	XMStoreFloat3(&bounds.Extents, 0.5f * (maxVector - minVector));

	return bounds;
}

static XMVECTOR SkinPosition(
	const M3DLoader::SkinnedVertex& vertex,
	const std::vector<XMFLOAT4X4>& finalTransforms)
{
	// 피킹은 CPU에서 삼각형과 ray를 검사하므로 GPU와 같은 방식으로 정점을 먼저 스키닝한다.
	float weights[4] =
	{
		vertex.BoneWeights.x,
		vertex.BoneWeights.y,
		vertex.BoneWeights.z,
		1.0f - vertex.BoneWeights.x - vertex.BoneWeights.y - vertex.BoneWeights.z
	};

	XMVECTOR sourcePosition = XMLoadFloat3(&vertex.Pos);
	XMVECTOR skinnedPosition = XMVectorZero();

	for(int i = 0; i < 4; ++i)
	{
		// FinalTransforms는 셰이더에 맞춰 transpose된 상태라 CPU 계산에서는 다시 transpose해서 쓴다.
		XMMATRIX boneTransform = XMMatrixTranspose(XMLoadFloat4x4(&finalTransforms[vertex.BoneIndices[i]]));
		skinnedPosition += weights[i] * XMVector3TransformCoord(sourcePosition, boneTransform);
	}

	return skinnedPosition;
}

static float HitDistanceFromViewRay(
	FXMVECTOR rayOriginLocal,
	FXMVECTOR rayDirLocal,
	float localT,
	const XMMATRIX& world,
	const XMMATRIX& view)
{
	// Problem: TriangleTests::Intersects returns t in each mesh's local ray space, so the
	// 0.05-scaled FBX model could not be compared fairly against unscaled scene geometry.
	XMVECTOR hitLocal = rayOriginLocal + localT * rayDirLocal;
	XMVECTOR hitWorld = XMVector3TransformCoord(hitLocal, world);
	XMVECTOR hitView = XMVector3TransformCoord(hitWorld, view);

	return XMVectorGetX(XMVector3Length(hitView));
}

class CameraAndDynamicIndexingApp : public D3DApp
{
public:
    CameraAndDynamicIndexingApp(HINSTANCE hInstance);
    CameraAndDynamicIndexingApp(const CameraAndDynamicIndexingApp& rhs) = delete;
    CameraAndDynamicIndexingApp& operator=(const CameraAndDynamicIndexingApp& rhs) = delete;
    ~CameraAndDynamicIndexingApp();

    virtual bool Initialize()override;

	UINT pickedTriangle = -1;
private:
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
	void AnimateMaterials(const GameTimer& gt);
	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateSkinnedCBs(const GameTimer& gt);
	void UpdateMaterialBuffer(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);

	void LoadTextures();
    void BuildRootSignature();
	void BuildDescriptorHeaps();
    void BuildShadersAndInputLayout();
    void BuildShapeGeometry();
	void LoadSkinnedModel();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
	void Pick(int Sx, int Sy);

	std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    UINT mCbvSrvDescriptorSize = 0;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;

	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
	std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
	// FBX 스키닝 정점은 tangent/weight/bone index가 추가되므로 별도 input layout을 쓴다.
	std::vector<D3D12_INPUT_ELEMENT_DESC> mSkinnedInputLayout;
 
	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

	RenderItem* mPickedRitem = nullptr;
	// 정적 mesh 피킹 highlight와 별도로, 스키닝 mesh highlight용 render item을 보관한다.
	RenderItem* mPickedSkinnedRitem = nullptr;

	// 현재 적용할 스키닝 모델 파일. Capoeira.fbx를 Model1.fbx 이름으로 복사해서 사용한다.
	std::string mSkinnedModelFilename = "Models\\Model1.fbx";
	// FBX 모델의 animation 재생 상태와 SkinnedData, subset/material 변환 결과를 보관한다.
	std::unique_ptr<SkinnedModelInstance> mSkinnedModelInst;
	SkinnedData mSkinnedInfo;
	std::vector<M3DLoader::Subset> mSkinnedSubsets;
	std::vector<M3DLoader::M3dMaterial> mSkinnedMats;

    PassConstants mMainPassCB;

	Camera mCamera;

    POINT mLastMousePos;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        CameraAndDynamicIndexingApp theApp(hInstance);
        if(!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch(DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
	catch(std::exception& e)
	{
		// FBX 로딩이나 애니메이션 이름 문제처럼 HRESULT가 아닌 오류를 메시지 박스로 보여준다.
		MessageBoxA(nullptr, e.what(), "Runtime Error", MB_OK);
		return 0;
	}
}

CameraAndDynamicIndexingApp::CameraAndDynamicIndexingApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
}

CameraAndDynamicIndexingApp::~CameraAndDynamicIndexingApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool CameraAndDynamicIndexingApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    // Reset the command list to prep for initialization commands.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    // Get the increment size of a descriptor in this heap type.  This is hardware specific, 
	// so we have to query this information.
    mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	mCamera.LookAt(
		XMFLOAT3(5.0f, 4.0f, -15.0f),
		XMFLOAT3(0.0f, 1.0f, 0.0f),
		XMFLOAT3(0.0f, 1.0f, 0.0f));
 
	// FBX material 개수를 알아야 texture descriptor table 크기 검사를 할 수 있으므로 먼저 로드한다.
	LoadSkinnedModel();
	LoadTextures();
    BuildRootSignature();
	BuildDescriptorHeaps();
    BuildShadersAndInputLayout();
    BuildShapeGeometry();
	BuildMaterials();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    // Execute the initialization commands.
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait until initialization is complete.
    FlushCommandQueue();

    return true;
}
 
void CameraAndDynamicIndexingApp::OnResize()
{
    D3DApp::OnResize();

	mCamera.SetLens(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
}

void CameraAndDynamicIndexingApp::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);

    // Cycle through the circular frame resource array.
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    // Has the GPU finished processing the commands of the current frame resource?
    // If not, wait until the GPU has completed commands up to this fence point.
    if(mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

	AnimateMaterials(gt);
	UpdateObjectCBs(gt);
	UpdateSkinnedCBs(gt);
	UpdateMaterialBuffer(gt);
	UpdateMainPassCB(gt);
}

void CameraAndDynamicIndexingApp::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(cmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

	auto passCB = mCurrFrameResource->PassCB->Resource();
	mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

	// Bind all the materials used in this scene.  For structured buffers, we can bypass the heap and 
	// set as a root descriptor.
	auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
	mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

	// Bind all the textures used in this scene.  Observe
    // that we only have to specify the first descriptor in the table.  
    // The root signature knows how many descriptors are expected in the table.
	mCommandList->SetGraphicsRootDescriptorTable(3, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

	mCommandList->SetPipelineState(mPSOs["opaque"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

	mCommandList->SetPipelineState(mPSOs["highlight"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Highlight]);

	// FBX 모델은 bone 입력이 있는 skinned vertex shader/PSO로 그린다.
	mCommandList->SetPipelineState(mPSOs["skinnedOpaque"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::SkinnedOpaque]);

	// FBX 모델에서 피킹된 삼각형은 같은 skinned vertex shader에 highlight blend만 적용한다.
	mCommandList->SetPipelineState(mPSOs["skinnedHighlight"].Get());
	DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::SkinnedHighlight]);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Swap the back and front buffers
    ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // Advance the fence value to mark commands up to this fence point.
    mCurrFrameResource->Fence = ++mCurrentFence;

    // Add an instruction to the command queue to set a new fence point. 
    // Because we are on the GPU timeline, the new fence point won't be 
    // set until the GPU finishes processing all the commands prior to this Signal().
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void CameraAndDynamicIndexingApp::OnMouseDown(WPARAM btnState, int x, int y)
{
	if ((btnState & MK_LBUTTON) != 0)
	{
		mLastMousePos.x = x;
		mLastMousePos.y = y;

		SetCapture(mhMainWnd);
	}
	else if ((btnState & MK_RBUTTON) != 0)
	{
		// 우클릭하면 정적 mesh와 FBX skinned mesh를 모두 대상으로 피킹한다.
		Pick(x, y);
	}
}

void CameraAndDynamicIndexingApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void CameraAndDynamicIndexingApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if((btnState & MK_LBUTTON) != 0)
    {
		// Make each pixel correspond to a quarter of a degree.
		float dx = XMConvertToRadians(0.25f*static_cast<float>(x - mLastMousePos.x));
		float dy = XMConvertToRadians(0.25f*static_cast<float>(y - mLastMousePos.y));

		mCamera.Pitch(dy);
		mCamera.RotateY(dx);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}
 
void CameraAndDynamicIndexingApp::OnKeyboardInput(const GameTimer& gt)
{
	const float dt = gt.DeltaTime();

	if(GetAsyncKeyState('W') & 0x8000)
		mCamera.Walk(10.0f*dt);

	if(GetAsyncKeyState('S') & 0x8000)
		mCamera.Walk(-10.0f*dt);

	if(GetAsyncKeyState('A') & 0x8000)
		mCamera.Strafe(-10.0f*dt);

	if(GetAsyncKeyState('D') & 0x8000)
		mCamera.Strafe(10.0f*dt);

	mCamera.UpdateViewMatrix();
}
 
void CameraAndDynamicIndexingApp::AnimateMaterials(const GameTimer& gt)
{
	
}

void CameraAndDynamicIndexingApp::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for(auto& e : mAllRitems)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if(e->NumFramesDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&e->World);
			XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
			objConstants.MaterialIndex = e->Mat->MatCBIndex;

			currObjectCB->CopyData(e->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			e->NumFramesDirty--;
		}
	}
}

void CameraAndDynamicIndexingApp::UpdateSkinnedCBs(const GameTimer& gt)
{
	if(mSkinnedModelInst == nullptr)
		return;

	auto currSkinnedCB = mCurrFrameResource->SkinnedCB.get();

	// animation 시간을 갱신한 뒤 이번 프레임에 사용할 bone transform을 얻는다.
	mSkinnedModelInst->UpdateSkinnedAnimation(gt.DeltaTime());

	SkinnedConstants skinnedConstants;
	for(UINT i = 0; i < _countof(skinnedConstants.BoneTransforms); ++i)
		skinnedConstants.BoneTransforms[i] = MathHelper::Identity4x4();

	// 사용하지 않는 bone 슬롯은 identity로 남겨 두고 실제 bone 개수만 복사한다.
	// FBX 본 개수가 셰이더 배열 크기보다 커지는 경우를 대비해 복사 범위를 한 번 더 제한한다.
	const UINT boneTransformCount = MathHelper::Min(
		(UINT)mSkinnedModelInst->FinalTransforms.size(),
		(UINT)_countof(skinnedConstants.BoneTransforms));

	for(UINT i = 0; i < boneTransformCount; ++i)
		skinnedConstants.BoneTransforms[i] = mSkinnedModelInst->FinalTransforms[i];

	currSkinnedCB->CopyData(0, skinnedConstants);
}

void CameraAndDynamicIndexingApp::UpdateMaterialBuffer(const GameTimer& gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
	for(auto& e : mMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		Material* mat = e.second.get();
		if(mat->NumFramesDirty > 0)
		{
			XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

			MaterialData matData;
			matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			matData.FresnelR0 = mat->FresnelR0;
			matData.Roughness = mat->Roughness;
			XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
			matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;

			currMaterialBuffer->CopyData(mat->MatCBIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void CameraAndDynamicIndexingApp::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = mCamera.GetView();
	XMMATRIX proj = mCamera.GetProj();

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mMainPassCB.EyePosW = mCamera.GetPosition3f();
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();
	mMainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
	mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
	mMainPassCB.Lights[0].Strength = { 0.8f, 0.8f, 0.8f };
	mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
	mMainPassCB.Lights[1].Strength = { 0.4f, 0.4f, 0.4f };
	mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
	mMainPassCB.Lights[2].Strength = { 0.2f, 0.2f, 0.2f };

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void CameraAndDynamicIndexingApp::LoadTextures()
{
	auto bricksTex = std::make_unique<Texture>();
	bricksTex->Name = "bricksTex";
	bricksTex->Filename = L"../../Textures/bricks.dds";
	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
		mCommandList.Get(), bricksTex->Filename.c_str(),
		bricksTex->Resource, bricksTex->UploadHeap));

	auto stoneTex = std::make_unique<Texture>();
	stoneTex->Name = "stoneTex";
	stoneTex->Filename = L"../../Textures/stone.dds";
	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
		mCommandList.Get(), stoneTex->Filename.c_str(),
		stoneTex->Resource, stoneTex->UploadHeap));

	auto tileTex = std::make_unique<Texture>();
	tileTex->Name = "tileTex";
	tileTex->Filename = L"../../Textures/tile.dds";
	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
		mCommandList.Get(), tileTex->Filename.c_str(),
		tileTex->Resource, tileTex->UploadHeap));

	auto crateTex = std::make_unique<Texture>();
	crateTex->Name = "crateTex";
	crateTex->Filename = L"../../Textures/WoodCrate01.dds";
	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
		mCommandList.Get(), crateTex->Filename.c_str(),
		crateTex->Resource, crateTex->UploadHeap));

	auto defaultDiffuseTex = std::make_unique<Texture>();
	defaultDiffuseTex->Name = "defaultDiffuseTex";
	defaultDiffuseTex->Filename = L"../../Textures/white1x1.dds";
	ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
		mCommandList.Get(), defaultDiffuseTex->Filename.c_str(),
		defaultDiffuseTex->Resource, defaultDiffuseTex->UploadHeap));

	mTextures[bricksTex->Name] = std::move(bricksTex);
	mTextures[stoneTex->Name] = std::move(stoneTex);
	mTextures[tileTex->Name] = std::move(tileTex);
	mTextures[crateTex->Name] = std::move(crateTex);
	mTextures[defaultDiffuseTex->Name] = std::move(defaultDiffuseTex);

	// FBX material이 참조하는 DDS diffuse texture를 기본 texture들과 같은 SRV heap에 넣는다.
	for(const auto& skinnedMat : mSkinnedMats)
	{
		std::string diffuseName = skinnedMat.DiffuseMapName;
		if(diffuseName.empty())
			diffuseName = "white1x1.dds";

		std::string texName = TextureNameFromMapName(diffuseName);

		if(mTextures.find(texName) != std::end(mTextures))
			continue;

		auto texMap = std::make_unique<Texture>();
		texMap->Name = texName;
		texMap->Filename = ResolveTextureFilename(diffuseName);

		if(IsDdsTextureName(diffuseName))
		{
			ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
				mCommandList.Get(), texMap->Filename.c_str(),
				texMap->Resource, texMap->UploadHeap));
		}
		else
		{
			ThrowIfFailed(CreateWICTextureFromFile12(md3dDevice.Get(),
				mCommandList.Get(), texMap->Filename.c_str(),
				texMap->Resource, texMap->UploadHeap));
		}

		mTextures[texMap->Name] = std::move(texMap);
	}
}

void CameraAndDynamicIndexingApp::BuildRootSignature()
{
	// 기본 텍스처 5개와 FBX material 텍스처가 고정 descriptor table 크기를 넘지 않는지 확인한다.
	if(5 + (UINT)mSkinnedMats.size() > gNumTextureDescriptors)
		throw std::runtime_error("Too many skinned model materials for the texture descriptor table.");

	CD3DX12_DESCRIPTOR_RANGE texTable[1];
	// HLSL의 gDiffuseMap 배열 크기와 descriptor table 크기는 서로 맞아야 PSO 생성이 실패하지 않는다.
	texTable[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, gNumTextureDescriptors, 0, 0);

    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER slotRootParameter[5];

	// Perfomance TIP: Order from most frequent to least frequent.
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstantBufferView(1);
    slotRootParameter[2].InitAsShaderResourceView(0, 1);
	slotRootParameter[3].InitAsDescriptorTable(_countof(texTable), texTable, D3D12_SHADER_VISIBILITY_PIXEL);
	// b2는 skinned vertex shader가 읽는 bone transform constant buffer이다.
	slotRootParameter[4].InitAsConstantBufferView(2);


	auto staticSamplers = GetStaticSamplers();

    // A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
		(UINT)staticSamplers.size(), staticSamplers.data(),
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if(errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void CameraAndDynamicIndexingApp::BuildDescriptorHeaps()
{
	//
	// Create the SRV heap.
	//
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	// root signature와 HLSL 배열 크기에 맞춰 고정 개수로 만든다.
	srvHeapDesc.NumDescriptors = gNumTextureDescriptors;
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

	//
	// Fill out the heap with actual descriptors.
	//
	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	auto bricksTex = mTextures["bricksTex"]->Resource;
	auto stoneTex = mTextures["stoneTex"]->Resource;
	auto tileTex = mTextures["tileTex"]->Resource;
	auto crateTex = mTextures["crateTex"]->Resource;
	auto defaultDiffuseTex = mTextures["defaultDiffuseTex"]->Resource;

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = bricksTex->GetDesc().Format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = bricksTex->GetDesc().MipLevels;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	md3dDevice->CreateShaderResourceView(bricksTex.Get(), &srvDesc, hDescriptor);

	// next descriptor
	hDescriptor.Offset(1, mCbvSrvDescriptorSize);

	srvDesc.Format = stoneTex->GetDesc().Format;
	srvDesc.Texture2D.MipLevels = stoneTex->GetDesc().MipLevels;
	md3dDevice->CreateShaderResourceView(stoneTex.Get(), &srvDesc, hDescriptor);

	// next descriptor
	hDescriptor.Offset(1, mCbvSrvDescriptorSize);

	srvDesc.Format = tileTex->GetDesc().Format;
	srvDesc.Texture2D.MipLevels = tileTex->GetDesc().MipLevels;
	md3dDevice->CreateShaderResourceView(tileTex.Get(), &srvDesc, hDescriptor);

	// next descriptor
	hDescriptor.Offset(1, mCbvSrvDescriptorSize);

	srvDesc.Format = crateTex->GetDesc().Format;
	srvDesc.Texture2D.MipLevels = crateTex->GetDesc().MipLevels;
	md3dDevice->CreateShaderResourceView(crateTex.Get(), &srvDesc, hDescriptor);

	hDescriptor.Offset(1, mCbvSrvDescriptorSize);

	srvDesc.Format = defaultDiffuseTex->GetDesc().Format;
	srvDesc.Texture2D.MipLevels = defaultDiffuseTex->GetDesc().MipLevels;
	md3dDevice->CreateShaderResourceView(defaultDiffuseTex.Get(), &srvDesc, hDescriptor);

	// 기본 5개 texture 뒤에 FBX material texture들을 이어서 넣는다.
	for(const auto& skinnedMat : mSkinnedMats)
	{
		hDescriptor.Offset(1, mCbvSrvDescriptorSize);

		std::string diffuseName = skinnedMat.DiffuseMapName;
		if(diffuseName.empty())
			diffuseName = "white1x1.dds";

		std::string texName = TextureNameFromMapName(diffuseName);
		auto diffuseTex = mTextures[texName]->Resource;

		srvDesc.Format = diffuseTex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = diffuseTex->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(diffuseTex.Get(), &srvDesc, hDescriptor);
	}

	// 남는 SRV 슬롯은 흰색 텍스처로 채워 둔다. 비어 있으면 디버그 레이어가 descriptor 문제를 잡아낼 수 있다.
	const UINT usedDescriptorCount = 5 + (UINT)mSkinnedMats.size();
	for(UINT i = usedDescriptorCount; i < gNumTextureDescriptors; ++i)
	{
		hDescriptor.Offset(1, mCbvSrvDescriptorSize);

		srvDesc.Format = defaultDiffuseTex->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = defaultDiffuseTex->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(defaultDiffuseTex.Get(), &srvDesc, hDescriptor);
	}
}

void CameraAndDynamicIndexingApp::BuildShadersAndInputLayout()
{
	const D3D_SHADER_MACRO alphaTestDefines[] =
	{
		"ALPHA_TEST", "1",
		NULL, NULL
	};

	const D3D_SHADER_MACRO skinnedDefines[] =
	{
		// SKINNED가 켜지면 HLSL에서 bone weight/index 입력과 skinning 계산을 사용한다.
		"SKINNED", "1",
		NULL, NULL
	};

	mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
	// skinnedVS는 같은 HLSL 파일을 SKINNED 매크로만 켜서 컴파일한다.
	mShaders["skinnedVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", skinnedDefines, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_1");
	
    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

	// M3DLoader::SkinnedVertex와 HLSL VertexIn(SKINNED)의 메모리 오프셋을 맞춘다.
	mSkinnedInputLayout =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "WEIGHTS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BONEINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};
}

void CameraAndDynamicIndexingApp::BuildShapeGeometry()
{
    GeometryGenerator geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);
	GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
	GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);

	//
	// We are concatenating all the geometry into one big vertex/index buffer.  So
	// define the regions in the buffer each submesh covers.
	//

	// Cache the vertex offsets to each object in the concatenated vertex buffer.
	UINT boxVertexOffset = 0;
	UINT gridVertexOffset = (UINT)box.Vertices.size();
	UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
	UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();

	// Cache the starting index for each object in the concatenated index buffer.
	UINT boxIndexOffset = 0;
	UINT gridIndexOffset = (UINT)box.Indices32.size();
	UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
	UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();

	SubmeshGeometry boxSubmesh;
	boxSubmesh.IndexCount = (UINT)box.Indices32.size();
	boxSubmesh.StartIndexLocation = boxIndexOffset;
	boxSubmesh.BaseVertexLocation = boxVertexOffset;
	boxSubmesh.Bounds = CalculateBounds(box);

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;
	gridSubmesh.Bounds = CalculateBounds(grid);

	SubmeshGeometry sphereSubmesh;
	sphereSubmesh.IndexCount = (UINT)sphere.Indices32.size();
	sphereSubmesh.StartIndexLocation = sphereIndexOffset;
	sphereSubmesh.BaseVertexLocation = sphereVertexOffset;
	sphereSubmesh.Bounds = CalculateBounds(sphere);

	SubmeshGeometry cylinderSubmesh;
	cylinderSubmesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
	cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;
	cylinderSubmesh.Bounds = CalculateBounds(cylinder);

	//
	// Extract the vertex elements we are interested in and pack the
	// vertices of all the meshes into one vertex buffer.
	//

	auto totalVertexCount =
		box.Vertices.size() +
		grid.Vertices.size() +
		sphere.Vertices.size() +
		cylinder.Vertices.size();

	std::vector<Vertex> vertices(totalVertexCount);

	UINT k = 0;
	for(size_t i = 0; i < box.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = box.Vertices[i].Position;
		vertices[k].Normal = box.Vertices[i].Normal;
		vertices[k].TexC = box.Vertices[i].TexC;
	}

	for(size_t i = 0; i < grid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = grid.Vertices[i].Position;
		vertices[k].Normal = grid.Vertices[i].Normal;
		vertices[k].TexC = grid.Vertices[i].TexC;
	}

	for(size_t i = 0; i < sphere.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = sphere.Vertices[i].Position;
		vertices[k].Normal = sphere.Vertices[i].Normal;
		vertices[k].TexC = sphere.Vertices[i].TexC;
	}

	for(size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder.Vertices[i].Position;
		vertices[k].Normal = cylinder.Vertices[i].Normal;
		vertices[k].TexC = cylinder.Vertices[i].TexC;
	}

	std::vector<std::uint16_t> indices;
	indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
	indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "shapeGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	geo->DrawArgs["box"] = boxSubmesh;
	geo->DrawArgs["grid"] = gridSubmesh;
	geo->DrawArgs["sphere"] = sphereSubmesh;
	geo->DrawArgs["cylinder"] = cylinderSubmesh;

	mGeometries[geo->Name] = std::move(geo);
}

void CameraAndDynamicIndexingApp::LoadSkinnedModel()
{
	std::vector<M3DLoader::SkinnedVertex> vertices;
	// Capoeira처럼 정점이 65536개를 넘는 FBX를 위해 스키닝 모델은 32비트 인덱스를 사용한다.
	std::vector<std::uint32_t> indices;

	// 기존 M3D 로딩 코드. 다시 soldier.m3d를 테스트할 때를 위해 주석으로 남겨둔다.
	//M3DLoader m3dLoader;
	//m3dLoader.LoadM3d(mSkinnedModelFilename, vertices, indices,
	//	mSkinnedSubsets, mSkinnedMats, mSkinnedInfo);

	// FBX는 AssimpLoader가 읽어서 기존 M3D와 같은 vertex/index/skinInfo 구조로 변환한다.
	AssimpLoader fbxLoader;
	if(!fbxLoader.LoadFbx(mSkinnedModelFilename, vertices, indices,
		mSkinnedSubsets, mSkinnedMats, mSkinnedInfo))
	{
		// 파일 경로, FBX 파싱, bone 개수 제한 같은 실패 원인을 로더 메시지로 확인한다.
		throw std::runtime_error(fbxLoader.GetLastError());
	}

	mSkinnedModelInst = std::make_unique<SkinnedModelInstance>();
	mSkinnedModelInst->SkinnedInfo = &mSkinnedInfo;
	// animation 계산 결과는 bone 개수만큼 보관하고 매 프레임 SkinnedCB로 복사한다.
	mSkinnedModelInst->FinalTransforms.resize(mSkinnedInfo.BoneCount());
	if(!mSkinnedInfo.HasAnimation("Take1"))
		throw std::runtime_error("FBX loaded, but Take1 animation clip was not generated.");

	// AssimpLoader가 FBX의 첫 animation 또는 bind pose를 Take1 이름으로 맞춰 주므로 여기서는 Take1을 재생한다.
	mSkinnedModelInst->ClipName = "Take1";
	mSkinnedModelInst->TimePos = 0.0f;

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(M3DLoader::SkinnedVertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = mSkinnedModelFilename;

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(M3DLoader::SkinnedVertex);
	geo->VertexBufferByteSize = vbByteSize;
	// Capoeira FBX처럼 정점이 많은 모델을 위해 skinned geometry는 32비트 index buffer를 사용한다.
	geo->IndexFormat = DXGI_FORMAT_R32_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	for(UINT i = 0; i < (UINT)mSkinnedSubsets.size(); ++i)
	{
		// FBX mesh/material 단위로 나뉜 subset을 DrawArgs에 등록해서 material별로 따로 그린다.
		SubmeshGeometry submesh;
		std::string name = "sm_" + std::to_string(i);

		submesh.IndexCount = (UINT)mSkinnedSubsets[i].FaceCount * 3;
		submesh.StartIndexLocation = mSkinnedSubsets[i].FaceStart * 3;
		submesh.BaseVertexLocation = 0;
		submesh.Bounds = CalculateBounds(vertices, mSkinnedSubsets[i]);

		geo->DrawArgs[name] = submesh;
	}

	mGeometries[geo->Name] = std::move(geo);
}

void CameraAndDynamicIndexingApp::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

	//
	// PSO for opaque objects.
	//
    ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	opaquePsoDesc.pRootSignature = mRootSignature.Get();
	opaquePsoDesc.VS = 
	{ 
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()), 
		mShaders["standardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS = 
	{ 
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets = 1;
	opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
	opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	opaquePsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC skinnedOpaquePsoDesc = opaquePsoDesc;
	// skinned PSO는 bone 입력이 있는 layout과 SKINNED vertex shader를 사용한다.
	skinnedOpaquePsoDesc.InputLayout = { mSkinnedInputLayout.data(), (UINT)mSkinnedInputLayout.size() };
	skinnedOpaquePsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["skinnedVS"]->GetBufferPointer()),
		mShaders["skinnedVS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skinnedOpaquePsoDesc, IID_PPV_ARGS(&mPSOs["skinnedOpaque"])));

	//
	// PSO for highlight objects.
	//
	D3D12_GRAPHICS_PIPELINE_STATE_DESC highlightPsoDesc = opaquePsoDesc;
	highlightPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

	D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
	transparencyBlendDesc.BlendEnable = true;
	transparencyBlendDesc.LogicOpEnable = false;
	transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
	transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
	transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
	transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
	transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	highlightPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&highlightPsoDesc, IID_PPV_ARGS(&mPSOs["highlight"])));

	D3D12_GRAPHICS_PIPELINE_STATE_DESC skinnedHighlightPsoDesc = highlightPsoDesc;
	// 피킹 highlight도 animation 자세를 유지해야 하므로 skinned vertex shader를 그대로 사용한다.
	skinnedHighlightPsoDesc.InputLayout = { mSkinnedInputLayout.data(), (UINT)mSkinnedInputLayout.size() };
	skinnedHighlightPsoDesc.VS =
	{
		reinterpret_cast<BYTE*>(mShaders["skinnedVS"]->GetBufferPointer()),
		mShaders["skinnedVS"]->GetBufferSize()
	};
	ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skinnedHighlightPsoDesc, IID_PPV_ARGS(&mPSOs["skinnedHighlight"])));
}

void CameraAndDynamicIndexingApp::BuildFrameResources()
{
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1, (UINT)mAllRitems.size(), 1, (UINT)mMaterials.size()));
    }
}

void CameraAndDynamicIndexingApp::BuildMaterials()
{
	auto bricks0 = std::make_unique<Material>();
	bricks0->Name = "bricks0";
	bricks0->MatCBIndex = 0;
	bricks0->DiffuseSrvHeapIndex = 0;
	bricks0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    bricks0->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
    bricks0->Roughness = 0.1f;

	auto stone0 = std::make_unique<Material>();
	stone0->Name = "stone0";
	stone0->MatCBIndex = 1;
	stone0->DiffuseSrvHeapIndex = 1;
	stone0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    stone0->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
    stone0->Roughness = 0.3f;
 
	auto tile0 = std::make_unique<Material>();
	tile0->Name = "tile0";
	tile0->MatCBIndex = 2;
	tile0->DiffuseSrvHeapIndex = 2;
	tile0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    tile0->FresnelR0 = XMFLOAT3(0.02f, 0.02f, 0.02f);
    tile0->Roughness = 0.3f;

	auto crate0 = std::make_unique<Material>();
	crate0->Name = "crate0";
	crate0->MatCBIndex = 3;
	crate0->DiffuseSrvHeapIndex = 3;
	crate0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    crate0->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
    crate0->Roughness = 0.2f;

	auto highlight0 = std::make_unique<Material>();
	highlight0->Name = "highlight0";
	highlight0->MatCBIndex = 4;
	highlight0->DiffuseSrvHeapIndex = 4;
	highlight0->DiffuseAlbedo = XMFLOAT4(1.0f, 0.0f, 0.0f, 0.6f);
	highlight0->FresnelR0 = XMFLOAT3(0.06f, 0.06f, 0.06f);
	highlight0->Roughness = 0.0f;
	
	mMaterials["bricks0"] = std::move(bricks0);
	mMaterials["stone0"] = std::move(stone0);
	mMaterials["tile0"] = std::move(tile0);
	mMaterials["crate0"] = std::move(crate0);
	mMaterials["highlight0"] = std::move(highlight0);

	UINT matCBIndex = 5;
	UINT diffuseSrvHeapIndex = 5;
	// 기본 material 5개 뒤에 FBX material들을 이어서 등록한다.
	for(const auto& skinnedMat : mSkinnedMats)
	{
		auto mat = std::make_unique<Material>();
		mat->Name = skinnedMat.Name;
		mat->MatCBIndex = matCBIndex++;
		mat->DiffuseSrvHeapIndex = diffuseSrvHeapIndex++;
		mat->DiffuseAlbedo = skinnedMat.DiffuseAlbedo;
		mat->FresnelR0 = skinnedMat.FresnelR0;
		mat->Roughness = skinnedMat.Roughness;

		mMaterials[mat->Name] = std::move(mat);
	}
}

void CameraAndDynamicIndexingApp::BuildRenderItems()
{
	auto boxRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(2.0f, 2.0f, 2.0f)*XMMatrixTranslation(0.0f, 1.0f, 0.0f));
	XMStoreFloat4x4(&boxRitem->TexTransform, XMMatrixScaling(1.0f, 1.0f, 1.0f));
	boxRitem->ObjCBIndex = 0;
	boxRitem->Mat = mMaterials["crate0"].get();
	boxRitem->Geo = mGeometries["shapeGeo"].get();
	boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
	boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
	boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
	boxRitem->Bounds = boxRitem->Geo->DrawArgs["box"].Bounds;
	mAllRitems.push_back(std::move(boxRitem));

    auto gridRitem = std::make_unique<RenderItem>();
    gridRitem->World = MathHelper::Identity4x4();
	XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
	gridRitem->ObjCBIndex = 1;
	gridRitem->Mat = mMaterials["tile0"].get();
	gridRitem->Geo = mGeometries["shapeGeo"].get();
	gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	gridRitem->Bounds = gridRitem->Geo->DrawArgs["grid"].Bounds;
	mAllRitems.push_back(std::move(gridRitem));

	XMMATRIX brickTexTransform = XMMatrixScaling(1.0f, 1.0f, 1.0f);
	UINT objCBIndex = 2;
	for(int i = 0; i < 5; ++i)
	{
		auto leftCylRitem = std::make_unique<RenderItem>();
		auto rightCylRitem = std::make_unique<RenderItem>();
		auto leftSphereRitem = std::make_unique<RenderItem>();
		auto rightSphereRitem = std::make_unique<RenderItem>();

		XMMATRIX leftCylWorld = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i*5.0f);
		XMMATRIX rightCylWorld = XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i*5.0f);

		XMMATRIX leftSphereWorld = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i*5.0f);
		XMMATRIX rightSphereWorld = XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i*5.0f);

		XMStoreFloat4x4(&leftCylRitem->World, rightCylWorld);
		XMStoreFloat4x4(&leftCylRitem->TexTransform, brickTexTransform);
		leftCylRitem->ObjCBIndex = objCBIndex++;
		leftCylRitem->Mat = mMaterials["bricks0"].get();
		leftCylRitem->Geo = mGeometries["shapeGeo"].get();
		leftCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
		leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
		leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
		leftCylRitem->Bounds = leftCylRitem->Geo->DrawArgs["cylinder"].Bounds;

		XMStoreFloat4x4(&rightCylRitem->World, leftCylWorld);
		XMStoreFloat4x4(&rightCylRitem->TexTransform, brickTexTransform);
		rightCylRitem->ObjCBIndex = objCBIndex++;
		rightCylRitem->Mat = mMaterials["bricks0"].get();
		rightCylRitem->Geo = mGeometries["shapeGeo"].get();
		rightCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		rightCylRitem->IndexCount = rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
		rightCylRitem->StartIndexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
		rightCylRitem->BaseVertexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;
		rightCylRitem->Bounds = rightCylRitem->Geo->DrawArgs["cylinder"].Bounds;

		XMStoreFloat4x4(&leftSphereRitem->World, leftSphereWorld);
		leftSphereRitem->TexTransform = MathHelper::Identity4x4();
		leftSphereRitem->ObjCBIndex = objCBIndex++;
		leftSphereRitem->Mat = mMaterials["stone0"].get();
		leftSphereRitem->Geo = mGeometries["shapeGeo"].get();
		leftSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		leftSphereRitem->IndexCount = leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
		leftSphereRitem->StartIndexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
		leftSphereRitem->BaseVertexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
		leftSphereRitem->Bounds = leftSphereRitem->Geo->DrawArgs["sphere"].Bounds;

		XMStoreFloat4x4(&rightSphereRitem->World, rightSphereWorld);
		rightSphereRitem->TexTransform = MathHelper::Identity4x4();
		rightSphereRitem->ObjCBIndex = objCBIndex++;
		rightSphereRitem->Mat = mMaterials["stone0"].get();
		rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
		rightSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
		rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
		rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
		rightSphereRitem->Bounds = rightSphereRitem->Geo->DrawArgs["sphere"].Bounds;

		mAllRitems.push_back(std::move(leftCylRitem));
		mAllRitems.push_back(std::move(rightCylRitem));
		mAllRitems.push_back(std::move(leftSphereRitem));
		mAllRitems.push_back(std::move(rightSphereRitem));
	}

	// All shape render items are opaque.
	for(auto& e : mAllRitems)
		mRitemLayer[(int)RenderLayer::Opaque].push_back(e.get());

	auto pickedRitem = std::make_unique<RenderItem>();
	pickedRitem->World = MathHelper::Identity4x4();
	pickedRitem->TexTransform = MathHelper::Identity4x4();
	pickedRitem->ObjCBIndex = objCBIndex++;
	pickedRitem->Mat = mMaterials["highlight0"].get();
	pickedRitem->Geo = mGeometries["shapeGeo"].get();
	pickedRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	pickedRitem->Visible = false;
	pickedRitem->IndexCount = 0;
	pickedRitem->StartIndexLocation = 0;
	pickedRitem->BaseVertexLocation = 0;

	mPickedRitem = pickedRitem.get();
	mRitemLayer[(int)RenderLayer::Highlight].push_back(pickedRitem.get());
	mAllRitems.push_back(std::move(pickedRitem));

	// FBX material/subset마다 render item을 하나씩 만들어 같은 모델을 material별로 그린다.
	for(UINT i = 0; i < (UINT)mSkinnedMats.size(); ++i)
	{
		std::string submeshName = "sm_" + std::to_string(i);

		auto ritem = std::make_unique<RenderItem>();

		// Problem: Assimp already converted the FBX to left-handed coordinates, so a negative Z scale flipped
		// the triangle winding again and made some back-side faces look like they were showing through.
		XMMATRIX modelScale = XMMatrixScaling(0.05f, 0.05f, 0.05f);
		XMMATRIX modelRot = XMMatrixRotationY(MathHelper::Pi);
		XMMATRIX modelOffset = XMMatrixTranslation(0.0f, 0.0f, -5.0f);
		XMStoreFloat4x4(&ritem->World, modelScale * modelRot * modelOffset);

		ritem->TexTransform = MathHelper::Identity4x4();
		ritem->ObjCBIndex = objCBIndex++;
		ritem->Mat = mMaterials[mSkinnedMats[i].Name].get();
		ritem->Geo = mGeometries[mSkinnedModelFilename].get();
		ritem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		ritem->IndexCount = ritem->Geo->DrawArgs[submeshName].IndexCount;
		ritem->StartIndexLocation = ritem->Geo->DrawArgs[submeshName].StartIndexLocation;
		ritem->BaseVertexLocation = ritem->Geo->DrawArgs[submeshName].BaseVertexLocation;
		ritem->Bounds = ritem->Geo->DrawArgs[submeshName].Bounds;
		// 이 render item은 스키닝 대상이므로 DrawRenderItems에서 b2 SkinnedCB를 바인딩한다.
		ritem->SkinnedCBIndex = 0;
		ritem->SkinnedModelInst = mSkinnedModelInst.get();

		mRitemLayer[(int)RenderLayer::SkinnedOpaque].push_back(ritem.get());
		mAllRitems.push_back(std::move(ritem));
	}

	// FBX 모델에서 피킹한 삼각형만 빨간색으로 다시 그리기 위한 전용 render item이다.
	auto pickedSkinnedRitem = std::make_unique<RenderItem>();
	pickedSkinnedRitem->World = MathHelper::Identity4x4();
	pickedSkinnedRitem->TexTransform = MathHelper::Identity4x4();
	pickedSkinnedRitem->ObjCBIndex = objCBIndex++;
	pickedSkinnedRitem->Mat = mMaterials["highlight0"].get();
	pickedSkinnedRitem->Geo = mGeometries[mSkinnedModelFilename].get();
	pickedSkinnedRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	pickedSkinnedRitem->Visible = false;
	pickedSkinnedRitem->IndexCount = 0;
	pickedSkinnedRitem->StartIndexLocation = 0;
	pickedSkinnedRitem->BaseVertexLocation = 0;
	pickedSkinnedRitem->SkinnedCBIndex = 0;
	pickedSkinnedRitem->SkinnedModelInst = mSkinnedModelInst.get();

	mPickedSkinnedRitem = pickedSkinnedRitem.get();
	mRitemLayer[(int)RenderLayer::SkinnedHighlight].push_back(pickedSkinnedRitem.get());
	mAllRitems.push_back(std::move(pickedSkinnedRitem));
}

void CameraAndDynamicIndexingApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
	UINT skinnedCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(SkinnedConstants));
 
	auto objectCB = mCurrFrameResource->ObjectCB->Resource();
	auto skinnedCB = mCurrFrameResource->SkinnedCB->Resource();

    // For each render item...
    for(size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

		if(ri->Visible == false)
			continue;

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex*objCBByteSize;

		// CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		// tex.Offset(ri->Mat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);

		cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

		if(ri->SkinnedModelInst != nullptr)
		{
			// skinned render item만 bone transform constant buffer(b2)를 같이 바인딩한다.
			D3D12_GPU_VIRTUAL_ADDRESS skinnedCBAddress =
				skinnedCB->GetGPUVirtualAddress() + ri->SkinnedCBIndex * skinnedCBByteSize;
			cmdList->SetGraphicsRootConstantBufferView(4, skinnedCBAddress);
		}

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

void CameraAndDynamicIndexingApp::Pick(int Sx, int Sy)
{
	// 화면 좌표를 view-space ray로 바꿔 정적 mesh와 skinned mesh 모두에 대해 삼각형 교차를 검사한다.
	XMFLOAT4X4 proj = mCamera.GetProj4x4f();

	float vx = (+2.0f * Sx / mClientWidth - 1.0f) / proj(0, 0);
	float vy = (-2.0f * Sy / mClientHeight + 1.0f) / proj(1, 1);

	XMVECTOR rayOriginView = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
	XMVECTOR rayDirView = XMVectorSet(vx, vy, 1.0f, 0.0f);

	XMMATRIX view = mCamera.GetView();
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);

	mPickedRitem->Visible = false;
	mPickedSkinnedRitem->Visible = false;
	pickedTriangle = -1;

	// Problem: local-space hit t is scale-dependent, so compare camera/view-space distance instead.
	float nearestDistance = MathHelper::Infinity;
	RenderItem* pickedSourceRitem = nullptr;
	UINT pickedStartIndexLocation = 0;
	int pickedBaseVertexLocation = 0;
	bool pickedSkinned = false;

	// 기존 정적 geometry는 CPU vertex buffer의 local-space 정점으로 바로 피킹한다.
	for(auto ri : mRitemLayer[(int)RenderLayer::Opaque])
	{
		if(ri->Visible == false)
			continue;

		auto geo = ri->Geo;

		XMMATRIX world = XMLoadFloat4x4(&ri->World);
		XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(world), world);

		XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);

		XMVECTOR rayOriginLocal = XMVector3TransformCoord(rayOriginView, toLocal);
		XMVECTOR rayDirLocal = XMVector3TransformNormal(rayDirView, toLocal);
		rayDirLocal = XMVector3Normalize(rayDirLocal);

		float tmin = 0.0f;
		if(ri->Bounds.Intersects(rayOriginLocal, rayDirLocal, tmin))
		{
			auto vertices = reinterpret_cast<Vertex*>(geo->VertexBufferCPU->GetBufferPointer());
			auto indices = reinterpret_cast<std::uint16_t*>(geo->IndexBufferCPU->GetBufferPointer());
			UINT triCount = ri->IndexCount / 3;

			for(UINT i = 0; i < triCount; ++i)
			{
				UINT indexBase = ri->StartIndexLocation + i * 3;
				UINT i0 = static_cast<UINT>(indices[indexBase + 0]) + static_cast<UINT>(ri->BaseVertexLocation);
				UINT i1 = static_cast<UINT>(indices[indexBase + 1]) + static_cast<UINT>(ri->BaseVertexLocation);
				UINT i2 = static_cast<UINT>(indices[indexBase + 2]) + static_cast<UINT>(ri->BaseVertexLocation);

				XMVECTOR v0 = XMLoadFloat3(&vertices[i0].Pos);
				XMVECTOR v1 = XMLoadFloat3(&vertices[i1].Pos);
				XMVECTOR v2 = XMLoadFloat3(&vertices[i2].Pos);

				float t = 0.0f;
				if(TriangleTests::Intersects(rayOriginLocal, rayDirLocal, v0, v1, v2, t))
				{
					float hitDistance = HitDistanceFromViewRay(rayOriginLocal, rayDirLocal, t, world, view);
					if(hitDistance < nearestDistance)
					{
						nearestDistance = hitDistance;
						pickedTriangle = i;
						pickedSourceRitem = ri;
						pickedStartIndexLocation = ri->StartIndexLocation + 3 * i;
						pickedBaseVertexLocation = ri->BaseVertexLocation;
						pickedSkinned = false;
					}
				}
			}
		}
	}

	// FBX skinned geometry는 현재 animation 자세로 정점을 변형한 뒤 피킹해야 화면과 결과가 맞는다.
	for(auto ri : mRitemLayer[(int)RenderLayer::SkinnedOpaque])
	{
		if(ri->Visible == false)
			continue;

		auto geo = ri->Geo;

		XMMATRIX world = XMLoadFloat4x4(&ri->World);
		XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(world), world);

		XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);

		XMVECTOR rayOriginLocal = XMVector3TransformCoord(rayOriginView, toLocal);
		XMVECTOR rayDirLocal = XMVector3TransformNormal(rayDirView, toLocal);
		rayDirLocal = XMVector3Normalize(rayDirLocal);

		auto vertices = reinterpret_cast<M3DLoader::SkinnedVertex*>(geo->VertexBufferCPU->GetBufferPointer());
		// skinned geometry는 32비트 index buffer이므로 uint32_t로 읽는다.
		auto indices = reinterpret_cast<std::uint32_t*>(geo->IndexBufferCPU->GetBufferPointer());
		UINT triCount = ri->IndexCount / 3;

		for(UINT i = 0; i < triCount; ++i)
		{
			UINT indexBase = ri->StartIndexLocation + i * 3;
			UINT i0 = static_cast<UINT>(indices[indexBase + 0]) + static_cast<UINT>(ri->BaseVertexLocation);
			UINT i1 = static_cast<UINT>(indices[indexBase + 1]) + static_cast<UINT>(ri->BaseVertexLocation);
			UINT i2 = static_cast<UINT>(indices[indexBase + 2]) + static_cast<UINT>(ri->BaseVertexLocation);

			// GPU vertex shader와 같은 bone transform을 CPU에서 적용해서 현재 자세의 삼각형을 만든다.
			XMVECTOR v0 = SkinPosition(vertices[i0], ri->SkinnedModelInst->FinalTransforms);
			XMVECTOR v1 = SkinPosition(vertices[i1], ri->SkinnedModelInst->FinalTransforms);
			XMVECTOR v2 = SkinPosition(vertices[i2], ri->SkinnedModelInst->FinalTransforms);

			float t = 0.0f;
			if(TriangleTests::Intersects(rayOriginLocal, rayDirLocal, v0, v1, v2, t))
			{
				float hitDistance = HitDistanceFromViewRay(rayOriginLocal, rayDirLocal, t, world, view);
				if(hitDistance < nearestDistance)
				{
					nearestDistance = hitDistance;
					pickedTriangle = i;
					pickedSourceRitem = ri;
					pickedStartIndexLocation = ri->StartIndexLocation + 3 * i;
					pickedBaseVertexLocation = ri->BaseVertexLocation;
					pickedSkinned = true;
				}
			}
		}
	}

	if(pickedSourceRitem != nullptr)
	{
		if(pickedSkinned)
		{
			// skinned highlight는 원본 모델의 world/geometry/skinnedCB를 그대로 공유하고 index 3개만 바꾼다.
			mPickedSkinnedRitem->Visible = true;
			mPickedSkinnedRitem->IndexCount = 3;
			mPickedSkinnedRitem->StartIndexLocation = pickedStartIndexLocation;
			mPickedSkinnedRitem->BaseVertexLocation = pickedBaseVertexLocation;
			mPickedSkinnedRitem->World = pickedSourceRitem->World;
			mPickedSkinnedRitem->TexTransform = pickedSourceRitem->TexTransform;
			mPickedSkinnedRitem->Geo = pickedSourceRitem->Geo;
			mPickedSkinnedRitem->SkinnedCBIndex = pickedSourceRitem->SkinnedCBIndex;
			mPickedSkinnedRitem->SkinnedModelInst = pickedSourceRitem->SkinnedModelInst;
			mPickedSkinnedRitem->NumFramesDirty = gNumFrameResources;
		}
		else
		{
			// 정적 highlight는 기존 shape geometry에서 선택된 삼각형 index 3개만 다시 그린다.
			mPickedRitem->Visible = true;
			mPickedRitem->IndexCount = 3;
			mPickedRitem->StartIndexLocation = pickedStartIndexLocation;
			mPickedRitem->BaseVertexLocation = pickedBaseVertexLocation;
			mPickedRitem->World = pickedSourceRitem->World;
			mPickedRitem->TexTransform = pickedSourceRitem->TexTransform;
			mPickedRitem->Geo = pickedSourceRitem->Geo;
			mPickedRitem->NumFramesDirty = gNumFrameResources;
		}
	}
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> CameraAndDynamicIndexingApp::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	return { 
		pointWrap, pointClamp,
		linearWrap, linearClamp, 
		anisotropicWrap, anisotropicClamp };
}

