#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <array>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace Microsoft::WRL;
using namespace DirectX;

struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT4 color;
};

struct ObjectConstants
{
    XMFLOAT4X4 worldViewProj;
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

    // 도형 추가
    void AddShape();
private:
    // 파이프라인
    ComPtr<ID3D12Device>                device;
    ComPtr<ID3D12CommandQueue>          commandQueue;
    ComPtr<IDXGISwapChain3>             swapChain;
    ComPtr<ID3D12DescriptorHeap>        rtvHeap;
    ComPtr<ID3D12DescriptorHeap>        dsvHeap;        // 깊이버퍼 힙
    ComPtr<ID3D12DescriptorHeap>        cbvHeap;
    ComPtr<ID3D12Resource>              renderTargets[frameCount];
    ComPtr<ID3D12Resource>              depthStencilBuffer;
    ComPtr<ID3D12CommandAllocator>      commandAllocator;
    ComPtr<ID3D12GraphicsCommandList>   commandList;
    ComPtr<ID3D12RootSignature>         rootSignature;
    ComPtr<ID3D12PipelineState>         pipelineState;

    // 도형 리소스
    ComPtr<ID3D12Resource>              vertexBuffer;
    ComPtr<ID3D12Resource>              indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW            vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW             indexBufferView;
    UINT                                indexCount;

    int shapeCount = 1; // 현재 도형 수
    static const int maxShapes = 3; // 최대 도형 수
    // 상수버퍼
    ComPtr<ID3D12Resource>  constantBuffer;
    ObjectConstants* cbMappedData = nullptr;
    UINT                    cbElementSize;  // 256바이트 정렬된 1개 크기

    // 동기화
    ComPtr<ID3D12Fence>                 fence;
    UINT64                              fenceValue;
    HANDLE                              fenceEvent;

    // 카메라 (BoxApp 구면좌표)
    XMFLOAT4X4  worldMatrix;
    XMFLOAT4X4  viewMatrix;
    XMFLOAT4X4  projMatrix;
    float       mTheta = 1.5f * XM_PI;
    float       mPhi = XM_PIDIV4;
    float       mRadius = 5.0f;
    POINT       mLastMousePos;

    UINT   rtvDescriptorSize;
    UINT   dsvDescriptorSize;
    UINT   frameIndex;
    int    width;
    int    height;

    bool LoadPipeline(HWND Hwnd, int Width, int Height);
    bool LoadAssets();
    bool BuildDescriptorHeaps();
    bool BuildConstantBuffers();
    bool BuildRootSignature();
    bool BuildShadersAndInputLayout(ComPtr<ID3DBlob>& VsByteCode,
        ComPtr<ID3DBlob>& PsByteCode);
    bool BuildBoxGeometry();
    bool BuildPSO(ComPtr<ID3DBlob>& VsByteCode,
        ComPtr<ID3DBlob>& PsByteCode);
    bool BuildDepthStencil();
    void WaitForPreviousFrame();
    void PopulateCommandList();
};