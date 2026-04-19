#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "Waves.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib") // DDSTextureLoader 의 ID3D11 함수 참조

using namespace Microsoft::WRL;
using namespace DirectX;

// BlendDemo 와 동일 구조의 Vertex / ObjectConstants
// 텍스처를 사용하므로 UV 좌표를 추가
struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT4 Color;
    XMFLOAT2 TexC;
};

struct ObjectConstants
{
    XMFLOAT4X4 WorldViewProj;
};

// BlendDemo 의 SubmeshGeometry / MeshGeometry 와 동일한 역할
struct SubmeshGeometry
{
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    INT  BaseVertexLocation = 0;
};

struct MeshGeometry
{
    std::string Name;

    ComPtr<ID3D12Resource> VertexBufferGPU = nullptr;
    ComPtr<ID3D12Resource> IndexBufferGPU  = nullptr;

    UINT VertexByteStride     = 0;
    UINT VertexBufferByteSize = 0;
    DXGI_FORMAT IndexFormat   = DXGI_FORMAT_R16_UINT;
    UINT IndexBufferByteSize  = 0;

    std::unordered_map<std::string, SubmeshGeometry> DrawArgs;

    D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const
    {
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress();
        vbv.StrideInBytes  = VertexByteStride;
        vbv.SizeInBytes    = VertexBufferByteSize;
        return vbv;
    }

    D3D12_INDEX_BUFFER_VIEW IndexBufferView() const
    {
        D3D12_INDEX_BUFFER_VIEW ibv = {};
        ibv.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress();
        ibv.Format         = IndexFormat;
        ibv.SizeInBytes    = IndexBufferByteSize;
        return ibv;
    }
};

// BlendDemo 의 Texture 와 동일 구조
struct Texture
{
    std::string                     Name;
    std::wstring                    Filename;
    ComPtr<ID3D12Resource>          Resource;
    ComPtr<ID3D12Resource>          UploadHeap;
};

// BlendDemo 의 RenderItem 과 동일 구조 (Material/TexTransform 은 생략)
// 텍스처를 위해 SrvHeapIndex 를 추가 (BlendDemo 의 Material->DiffuseSrvHeapIndex 대응)
struct RenderItem
{
    RenderItem()
    {
        XMStoreFloat4x4(&World, XMMatrixIdentity());
    }

    XMFLOAT4X4 World;

    UINT ObjCBIndex = (UINT)-1;

    MeshGeometry* Geo = nullptr;

    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    UINT IndexCount         = 0;
    UINT StartIndexLocation = 0;
    INT  BaseVertexLocation = 0;

    // SRV 힙 안에서 이 ritem 이 사용할 텍스처의 인덱스
    int  SrvHeapIndex = 0;
};

// BlendDemo 와 동일하게 PSO 별 렌더 레이어 구분
// BlendDemo 와 동일: Opaque → AlphaTested → Transparent(Waves) 순서
enum class RenderLayer : int
{
    Opaque = 0,
    AlphaTested,
    Waves,
    Count
};

class Dx12Renderer
{
public:
    static const int frameCount = 2;

    bool Initialize(HWND Hwnd, int Width, int Height);
    void Update();
    void Render();
    void Cleanup();

    // 마우스 입력 (BoxApp 방식)
    void OnMouseDown(int BtnState, int X, int Y);
    void OnMouseUp(int BtnState, int X, int Y);
    void OnMouseMove(int BtnState, int X, int Y);

    // 도형 추가 (midAssignment 기존 로직 그대로)
    void AddShape();

private:
    // ─── 파이프라인 ───────────────────────────────────────────
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        commandQueue;
    ComPtr<IDXGISwapChain3>           swapChain;
    ComPtr<ID3D12DescriptorHeap>      rtvHeap;
    ComPtr<ID3D12DescriptorHeap>      dsvHeap;
    ComPtr<ID3D12DescriptorHeap>      mSrvHeap;       // CBV/SRV/UAV (텍스처용)
    ComPtr<ID3D12Resource>            renderTargets[frameCount];
    ComPtr<ID3D12Resource>            depthStencilBuffer;
    ComPtr<ID3D12CommandAllocator>    commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12RootSignature>       mRootSignature;

    // ─── BlendDemo 스타일의 지오메트리/쉐이더/PSO/Texture 맵 ───
    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, ComPtr<ID3DBlob>>              mShaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>>   mPSOs;
    std::unordered_map<std::string, std::unique_ptr<Texture>>      mTextures;
    std::vector<D3D12_INPUT_ELEMENT_DESC>                          mInputLayout;

    // ─── 렌더 아이템 (BlendDemo 와 동일 구조) ────────────────
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;
    std::vector<RenderItem*>                 mRitemLayer[(int)RenderLayer::Count];
    RenderItem*                              mWavesRitem = nullptr;

    // Waves
    std::unique_ptr<Waves>   mWaves;
    ComPtr<ID3D12Resource>   mWavesDynamicVB; // upload heap, persistent map
    Vertex*                  mWavesMappedVertices = nullptr;

    // ─── ObjectCB (BlendDemo 의 FrameResource::ObjectCB 대응) ─
    // 메인패스 CB 는 사용하지 않는다.
    ComPtr<ID3D12Resource> mObjectCB;
    BYTE*                  mObjectCBMapped   = nullptr;
    UINT                   mObjectCBByteSize = 0;

    // ─── 동기화 ──────────────────────────────────────────
    ComPtr<ID3D12Fence> fence;
    UINT64              fenceValue = 0;
    HANDLE              fenceEvent = nullptr;

    // ─── 카메라 (BoxApp/BlendDemo 동일) ────────────────────
    XMFLOAT4X4 mView;
    XMFLOAT4X4 mProj;
    float      mTheta  = 1.5f * XM_PI;
    float      mPhi    = XM_PIDIV4;
    float      mRadius = 5.0f;
    POINT      mLastMousePos;

    // ─── 도형 개수 (midAssignment 기존 로직 그대로) ────────
    int              shapeCount = 1; // 현재 도형 수
    static const int maxShapes  = 3; // 최대 도형 수

    // ─── 타이밍 (BlendDemo 의 GameTimer 대응) ──────────────
    float         mTotalTime       = 0.0f;
    float         mWaveDisturbBase = 0.0f;
    LARGE_INTEGER mPerfFreq   = {};
    LARGE_INTEGER mPrevCounter = {};

    // ─── FPS 측정 (BlendDemo::CalculateFrameStats 대응) ──
    HWND  mHwnd          = nullptr;
    int   mFrameCount    = 0;
    float mTimeElapsed   = 0.0f; // 마지막 측정 이후 누적 시간

    UINT rtvDescriptorSize = 0;
    UINT dsvDescriptorSize = 0;
    UINT srvDescriptorSize = 0;
    UINT frameIndex = 0;
    int  width  = 0;
    int  height = 0;

    // ─── 초기화 ──────────────────────────────────────────
    bool LoadPipeline(HWND Hwnd, int Width, int Height);
    bool LoadAssets();

    // ─── Update (BlendDemo 와 동일 분리) ───────────────────
    void UpdateCamera();
    void UpdateObjectCBs();
    void UpdateWaves(float Dt);
    void CalculateFrameStats(); // BlendDemo::CalculateFrameStats 와 동일

    // ─── Build* (BlendDemo 호출 순서 동일) ─────────────────
    void LoadTextures();           // BlendDemo::LoadTextures 와 동일
    void BuildRootSignature();
    void BuildDescriptorHeaps();   // RTV/DSV/SRV 모두 생성
    void BuildShadersAndInputLayout();
    void BuildLandGeometry();      // BlendDemo 와 동일: Hills 높이맵 적용한 grid
    void BuildBoxGeometry();
    void BuildWavesGeometry();
    void BuildPSOs();
    void BuildFrameResources();    // ObjectCB 단일 버퍼 생성
    void BuildRenderItems();
    void BuildDepthStencil();

    // BlendDemo 와 동일한 언덕 높이/노멀 함수
    float    GetHillsHeight(float X, float Z) const;
    XMFLOAT3 GetHillsNormal(float X, float Z) const;

    // ─── Draw ────────────────────────────────────────────
    void DrawRenderItems(ID3D12GraphicsCommandList* Cmd,
                         const std::vector<RenderItem*>& Ritems);
    void PopulateCommandList();
    void WaitForPreviousFrame();
};
