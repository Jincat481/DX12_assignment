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
// 텍스처 + 라이팅(낮/밤)을 위해 Normal 추가
struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT4 Color;
    XMFLOAT3 Normal;   // 람베르트 라이팅용 월드(=로컬, 균등 스케일) 노멀
    XMFLOAT2 TexC;
};

struct ObjectConstants
{
    XMFLOAT4X4 WorldViewProj;
};

// 패스(프레임) 단위 상수 — 태양/앰비언트 색
// HLSL 의 cbuffer 정렬을 위해 각 float3 뒤에 padding 1개씩 (총 48 byte → CB 256-align 처리)
struct PassConstants
{
    XMFLOAT3 SunDir;       float pad0;   // 태양으로 향하는 방향 (정규화)
    XMFLOAT3 SunColor;     float pad1;   // 직접광 색
    XMFLOAT3 AmbientColor; float pad2;   // 환경광 색
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

// 나무/풀 전용 정점 — TexC.z 에 Texture2DArray 슬라이스 인덱스를 담는다
struct TreeVertex
{
    XMFLOAT3 Pos;
    XMFLOAT4 Color;
    XMFLOAT3 TexC; // x=u, y=v, z=array_slice (float cast)
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

    // 나무 심기 / 저장 / 불러오기
    void AddTree();
    void SaveTrees(const CString& Filename);
    bool LoadTrees(const CString& Filename);

    // 낮/밤 토글 — 1초 동안 부드럽게 보간된다
    void ToggleDayNight();

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
    std::vector<D3D12_INPUT_ELEMENT_DESC>                          mTreeInputLayout; // TreeVertex 전용

    // ─── 렌더 아이템 (BlendDemo 와 동일 구조) ────────────────
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;
    std::vector<RenderItem*>                 mRitemLayer[(int)RenderLayer::Count];
    RenderItem*                              mWavesRitem = nullptr;

    // Waves
    std::unique_ptr<Waves>   mWaves;
    ComPtr<ID3D12Resource>   mWavesDynamicVB;
    Vertex*                  mWavesMappedVertices = nullptr;

    // ─── 동적 나무 (버튼 클릭마다 1그루씩 추가) ──────────────
    static const int         kMaxTrees = 500;
    std::vector<XMFLOAT3>   mTreeCandidates;       // 심을 수 있는 후보 위치
    std::vector<XMFLOAT3>   mTreePositions;         // 현재 심긴 나무 위치
    ComPtr<ID3D12Resource>   mTreeDynamicVB;         // persistent mapped VB
    TreeVertex*              mTreeMappedVertices = nullptr;
    int                      mTreeCurrentCount   = 0;
    UINT                     mTreeArraySlices    = 4; // treeArray2.dds 슬라이스 수
    RenderItem*              mTreeRitem          = nullptr;

    // ─── ObjectCB (BlendDemo 의 FrameResource::ObjectCB 대응) ─
    ComPtr<ID3D12Resource> mObjectCB;
    BYTE*                  mObjectCBMapped   = nullptr;
    UINT                   mObjectCBByteSize = 0;

    // ─── PassCB (낮/밤 라이팅 전용) ─
    ComPtr<ID3D12Resource> mPassCB;
    BYTE*                  mPassCBMapped   = nullptr;
    UINT                   mPassCBByteSize = 0;

    // ─── 낮/밤 상태 ──────────────────────────────────────────
    bool   mIsNight  = false; // false=낮 / true=밤
    float  mDayBlend = 1.0f;  // 1=낮, 0=밤 (목표값으로 dt 마다 보간)
    XMFLOAT4 mClearColor = { 0.69f, 0.77f, 0.87f, 1.0f }; // 매 프레임 갱신

    // ─── 동기화 ──────────────────────────────────────────
    ComPtr<ID3D12Fence> fence;
    UINT64              fenceValue = 0;
    HANDLE              fenceEvent = nullptr;

    // ─── 카메라 (Chapter 15 FPS 방식) ──────────────────────
    XMFLOAT4X4 mView;
    XMFLOAT4X4 mProj;
    // FPS 카메라 기저 벡터 (Camera::UpdateViewMatrix 와 동일 구조)
    XMFLOAT3   mEyePos   = {  0.0f, 2.0f, -10.0f };
    XMFLOAT3   mCamRight = {  1.0f, 0.0f,   0.0f };
    XMFLOAT3   mCamUp    = {  0.0f, 1.0f,   0.0f };
    XMFLOAT3   mCamLook  = {  0.0f, 0.0f,   1.0f };
    POINT      mLastMousePos;

    // (AddShape/shapeCount 는 제거 — 나무 심기로 대체)

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
    void OnKeyboardInput(float Dt);  // WASD 이동 (Chapter 15)
    void UpdateCamera();
    void UpdateObjectCBs();
    void UpdateWaves(float Dt);
    void UpdatePassCB(float Dt);     // 낮/밤 보간 + PassConstants 기록
    void CalculateFrameStats(); // BlendDemo::CalculateFrameStats 와 동일

    // ─── Build* (BlendDemo 호출 순서 동일) ─────────────────
    void LoadTextures();           // BlendDemo::LoadTextures 와 동일
    void BuildRootSignature();
    void BuildDescriptorHeaps();   // RTV/DSV/SRV 모두 생성
    void BuildShadersAndInputLayout();
    void BuildLandGeometry();          // BlendDemo 와 동일: Hills 높이맵 적용한 grid
    void BuildTreeSpritesGeometry();   // 동적 VB 준비 + 후보 위치 계산
    void BuildWavesGeometry();
    void BuildPSOs();
    void BuildFrameResources();    // ObjectCB 단일 버퍼 생성
    void BuildRenderItems();
    void BuildDepthStencil();

    // BlendDemo 와 동일한 언덕 높이/노멀 함수
    float    GetHillsHeight(float X, float Z) const;
    XMFLOAT3 GetHillsNormal(float X, float Z) const;

    // ─── Draw ────────────────────────────────────────────
    // 나무 정점 하나 기록 (AddTree / LoadTrees 공용)
    void WriteTreeVertex(int Index, const XMFLOAT3& Pos);

    void DrawRenderItems(ID3D12GraphicsCommandList* Cmd,
                         const std::vector<RenderItem*>& Ritems);
    void PopulateCommandList();
    void WaitForPreviousFrame();
};
