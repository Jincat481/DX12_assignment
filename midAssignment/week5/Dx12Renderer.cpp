#include "pch.h"
#include "Dx12Renderer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/MathHelper.h"
#include "../../Common/DDSTextureLoader.h"
#include <vector>
#include <cstdlib>
#include <cassert>

// 단위행렬 헬퍼
static XMFLOAT4X4 Identity4x4()
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixIdentity());
    return m;
}

// ═════════════════════════════════════════════════════════════════════
// Initialize / LoadPipeline / LoadAssets   (BlendDemo::Initialize 순서)
// ═════════════════════════════════════════════════════════════════════
bool Dx12Renderer::Initialize(HWND Hwnd, int Width, int Height)
{
    mHwnd = Hwnd; // FPS 타이틀 표시용

    // Waves 시뮬레이터 (BlendDemo 와 동일한 파라미터)
    mWaves = std::make_unique<Waves>(128, 128, 1.0f, 0.03f, 4.0f, 0.2f);

    // 델타타임 계산용 고해상도 카운터
    QueryPerformanceFrequency(&mPerfFreq);
    QueryPerformanceCounter(&mPrevCounter);

    if (!LoadPipeline(Hwnd, Width, Height)) return false;
    if (!LoadAssets())                      return false;
    return true;
}

bool Dx12Renderer::LoadPipeline(HWND Hwnd, int Width, int Height)
{
    width  = Width;
    height = Height;

#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount      = frameCount;
    scDesc.Width            = width;
    scDesc.Height           = height;
    scDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    factory->CreateSwapChainForHwnd(commandQueue.Get(), Hwnd,
        &scDesc, nullptr, nullptr, &swapChain1);
    swapChain1.As(&swapChain);
    frameIndex = swapChain->GetCurrentBackBufferIndex();

    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&commandAllocator));

    return true;
}

bool Dx12Renderer::LoadAssets()
{
    // 펜스를 먼저 생성 (텍스처 업로드 후 wait 에 사용)
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fenceValue = 1;
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // BlendDemo::Initialize 와 동일한 호출 순서
    // 단, 텍스처 업로드를 위해 command list 를 먼저 생성/오픈한다.
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocator.Get(), nullptr,
        IID_PPV_ARGS(&commandList));
    // 위 호출 직후 commandList 는 open 상태 (DDSTextureLoader 업로드 명령 기록 가능)

    LoadTextures();           // BlendDemo::LoadTextures
    BuildRootSignature();
    BuildDescriptorHeaps();   // RTV/DSV + SRV 힙
    BuildShadersAndInputLayout();
    BuildLandGeometry();
    BuildBoxGeometry();
    BuildWavesGeometry();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    BuildDepthStencil();

    // 텍스처 업로드 명령을 실행하고 GPU 가 끝나길 기다린다
    commandList->Close();
    ID3D12CommandList* cmdLists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);

    // 투영행렬
    float aspect = (float)width / (float)height;
    XMStoreFloat4x4(&mProj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 1.0f, 1000.0f));
    mView = Identity4x4();

    WaitForPreviousFrame();
    return true;
}

// ═════════════════════════════════════════════════════════════════════
// Update / UpdateCamera / UpdateObjectCBs / UpdateWaves
// (BlendDemo::Update 의 구조 그대로)
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::Update()
{
    // 델타타임 계산 (BlendDemo 는 GameTimer 를 사용)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - mPrevCounter.QuadPart) / (float)mPerfFreq.QuadPart;
    mPrevCounter = now;
    if (dt > 0.1f) dt = 0.1f;
    mTotalTime += dt;

    UpdateCamera();
    UpdateObjectCBs();
    UpdateWaves(dt);
    CalculateFrameStats();
}

void Dx12Renderer::UpdateCamera()
{
    // 구면좌표 → 카메라 위치
    float x = mRadius * sinf(mPhi) * cosf(mTheta);
    float z = mRadius * sinf(mPhi) * sinf(mTheta);
    float y = mRadius * cosf(mPhi);

    XMVECTOR pos    = XMVectorSet(x, y, z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMStoreFloat4x4(&mView, XMMatrixLookAtLH(pos, target, up));
}

void Dx12Renderer::UpdateObjectCBs()
{
    // 각 RenderItem 의 world * view * proj 를 ObjectCB 슬롯에 기록
    // (BlendDemo 는 world 와 texTransform 을 별도로 저장하지만 여기선 WVP 만 저장)
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    for (auto& e : mAllRitems)
    {
        XMMATRIX world = XMLoadFloat4x4(&e->World);
        XMMATRIX wvp   = XMMatrixTranspose(world * view * proj);

        ObjectConstants cb;
        XMStoreFloat4x4(&cb.WorldViewProj, wvp);
        memcpy(mObjectCBMapped + (SIZE_T)e->ObjCBIndex * mObjectCBByteSize,
               &cb, sizeof(ObjectConstants));
    }
}

void Dx12Renderer::UpdateWaves(float Dt)
{
    // BlendDemo::UpdateWaves 와 동일: 0.25 초마다 임의 위치 파동 생성
    // (가시성을 위해 disturb 강도를 BlendDemo 보다 키운다)
    if ((mTotalTime - mWaveDisturbBase) >= 0.25f)
    {
        mWaveDisturbBase += 0.25f;

        int i = 4 + MathHelper::Rand(0, mWaves->RowCount()    - 9);
        int j = 4 + MathHelper::Rand(0, mWaves->ColumnCount() - 9);
        float r = MathHelper::RandF(0.5f, 1.2f);

        mWaves->Disturb(i, j, r);
    }

    // 파동 시뮬레이션 진행
    mWaves->Update(Dt);

    // 현재 해(solution) 를 동적 VB 에 복사 (position 만 업데이트)
    // 색은 텍스처 색 그대로 보이게 white + 반투명, UV 는 BuildWavesGeometry 에서 한 번만 세팅
    for (int i = 0; i < mWaves->VertexCount(); ++i)
    {
        mWavesMappedVertices[i].Pos = mWaves->Position(i);
        // Color/TexC 는 변경하지 않음 (이미 BuildWavesGeometry 에서 세팅됨)
    }
}

// ═════════════════════════════════════════════════════════════════════
// CalculateFrameStats  (BlendDemo::d3dApp::CalculateFrameStats 와 동일)
// 1초마다 fps/mspf 를 계산해 윈도우 타이틀바에 표시한다.
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::CalculateFrameStats()
{
    mFrameCount++;

    // 1초가 지나면 갱신
    if ((mTotalTime - mTimeElapsed) >= 1.0f)
    {
        float fps  = (float)mFrameCount;        // 1초 동안의 프레임 수
        float mspf = 1000.0f / fps;             // 프레임당 밀리초

        wchar_t buf[256];
        swprintf_s(buf, L"week5    fps: %.2f    mspf: %.4f", fps, mspf);

        // mHwnd 는 MFC 뷰(자식 윈도우) → 타이틀바를 가진 최상위 프레임을 찾아야 한다
        HWND rootWnd = ::GetAncestor(mHwnd, GA_ROOT);
        SetWindowTextW(rootWnd ? rootWnd : mHwnd, buf);

        // 카운터 리셋
        mFrameCount  = 0;
        mTimeElapsed += 1.0f;
    }
}

// ═════════════════════════════════════════════════════════════════════
// LoadTextures  (BlendDemo::LoadTextures 와 동일)
// command list 가 open 상태에서 호출돼야 한다.
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::LoadTextures()
{
    auto grassTex = std::make_unique<Texture>();
    grassTex->Name     = "grassTex";
    grassTex->Filename = L"../../Textures/grass.dds";
    DirectX::CreateDDSTextureFromFile12(device.Get(), commandList.Get(),
        grassTex->Filename.c_str(),
        grassTex->Resource, grassTex->UploadHeap);

    auto waterTex = std::make_unique<Texture>();
    waterTex->Name     = "waterTex";
    waterTex->Filename = L"../../Textures/water1.dds";
    DirectX::CreateDDSTextureFromFile12(device.Get(), commandList.Get(),
        waterTex->Filename.c_str(),
        waterTex->Resource, waterTex->UploadHeap);

    auto fenceTex = std::make_unique<Texture>();
    fenceTex->Name     = "fenceTex";
    fenceTex->Filename = L"../../Textures/WireFence.dds";
    DirectX::CreateDDSTextureFromFile12(device.Get(), commandList.Get(),
        fenceTex->Filename.c_str(),
        fenceTex->Resource, fenceTex->UploadHeap);

    mTextures[grassTex->Name] = std::move(grassTex);
    mTextures[waterTex->Name] = std::move(waterTex);
    mTextures[fenceTex->Name] = std::move(fenceTex);
}

// BuildRootSignature
//   slot 0: SRV descriptor table  (t0) — diffuse texture
//   slot 1: CBV                    (b0) — ObjectCB (WVP)
//   static sampler s0              — Linear Wrap
void Dx12Renderer::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0; // t0
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParams[2] = {};

    // slot 0: SRV table (PS 에서 사용)
    rootParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges   = &srvRange;
    rootParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // slot 1: ObjectCB (VS 에서 사용)
    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 0; // b0
    rootParams[1].Descriptor.RegisterSpace  = 0;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    // 정적 샘플러: Linear Wrap (s0)
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias       = 0.0f;
    sampler.MaxAnisotropy    = 1;
    sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD           = 0.0f;
    sampler.MaxLOD           = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister   = 0; // s0
    sampler.RegisterSpace    = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters     = _countof(rootParams);
    desc.pParameters       = rootParams;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, error;
    D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &error);
    device->CreateRootSignature(0,
        serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature));
}

// BuildDescriptorHeaps  (RTV/DSV/SRV)
void Dx12Renderer::BuildDescriptorHeaps()
{
    // RTV 힙
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = frameCount;
    rtvDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
    rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < frameCount; i++)
    {
        swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i]));
        device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize;
    }

    // DSV 힙
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap));
    dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // SRV 힙 (텍스처 3개: 0=grass, 1=water, 2=fence)
    // BlendDemo 와 동일하게 SHADER_VISIBLE 힙
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 3;
    srvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvHeap));
    srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 각 텍스처에 대해 SRV 생성
    auto createSrv = [&](ID3D12Resource* res, UINT heapSlot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = res->GetDesc().Format;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip     = 0;
        srvDesc.Texture2D.MipLevels           = res->GetDesc().MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)heapSlot * srvDescriptorSize;
        device->CreateShaderResourceView(res, &srvDesc, handle);
    };
    createSrv(mTextures["grassTex"]->Resource.Get(), 0);
    createSrv(mTextures["waterTex"]->Resource.Get(), 1);
    createSrv(mTextures["fenceTex"]->Resource.Get(), 2);
}

// BuildShadersAndInputLayout
void Dx12Renderer::BuildShadersAndInputLayout()
{
    // 텍스처를 샘플하고 vertex color 와 곱하는 간이 셰이더
    const char* shaderSource = R"(
        cbuffer cbPerObject : register(b0)
        {
            float4x4 gWorldViewProj;
        };

        Texture2D    gDiffuseMap : register(t0);
        SamplerState gSampler    : register(s0);

        struct VertexIn
        {
            float3 PosL  : POSITION;
            float4 Color : COLOR;
            float2 TexC  : TEXCOORD;
        };
        struct VertexOut
        {
            float4 PosH  : SV_POSITION;
            float4 Color : COLOR;
            float2 TexC  : TEXCOORD;
        };

        VertexOut VS(VertexIn vin)
        {
            VertexOut vout;
            vout.PosH  = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
            vout.Color = vin.Color;
            vout.TexC  = vin.TexC;
            return vout;
        }
        float4 PS(VertexOut pin) : SV_Target
        {
            float4 tex = gDiffuseMap.Sample(gSampler, pin.TexC);
            return tex * pin.Color;
        }

        // BlendDemo::alphaTested PS: 알파가 0.1 미만인 픽셀을 버림
        float4 PS_AlphaTest(VertexOut pin) : SV_Target
        {
            float4 tex = gDiffuseMap.Sample(gSampler, pin.TexC);
            clip(tex.a - 0.1f);   // 울타리 구멍 제거
            return tex * pin.Color;
        }
    )";

    ComPtr<ID3DBlob> vs, ps, psAlpha, error;
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr,
        "VS", "vs_5_0", flags, 0, &vs, &error);
    D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr,
        "PS", "ps_5_0", flags, 0, &ps, &error);
    D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr,
        "PS_AlphaTest", "ps_5_0", flags, 0, &psAlpha, &error);

    mShaders["standardVS"]    = vs;
    mShaders["opaquePS"]      = ps;
    mShaders["alphaTestedPS"] = psAlpha;

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

// ═════════════════════════════════════════════════════════════════════
// Hills 높이/노멀 (BlendDemo 와 동일 공식)
// ═════════════════════════════════════════════════════════════════════
float Dx12Renderer::GetHillsHeight(float X, float Z) const
{
    return 0.3f * (Z * sinf(0.1f * X) + X * cosf(0.1f * Z));
}

XMFLOAT3 Dx12Renderer::GetHillsNormal(float X, float Z) const
{
    XMFLOAT3 n(
        -0.03f * Z * cosf(0.1f * X) - 0.3f * cosf(0.1f * Z),
        1.0f,
        -0.3f * sinf(0.1f * X) + 0.03f * X * sinf(0.1f * Z));
    XMVECTOR unitNormal = XMVector3Normalize(XMLoadFloat3(&n));
    XMStoreFloat3(&n, unitNormal);
    return n;
}

// BuildLandGeometry  (BlendDemo::BuildLandGeometry 와 동일: Hills 적용 grid)
void Dx12Renderer::BuildLandGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData grid = geoGen.CreateGrid(160.0f, 160.0f, 50, 50);

    std::vector<Vertex> vertices(grid.Vertices.size());
    for (size_t i = 0; i < grid.Vertices.size(); ++i)
    {
        auto& p = grid.Vertices[i].Position;
        vertices[i].Pos   = p;
        vertices[i].Pos.y = GetHillsHeight(p.x, p.z);
        // 풀 텍스처를 그대로 보여주려면 white 색
        vertices[i].Color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        // 텍스처 좌표는 GeometryGenerator 가 0~1 로 채워놨으니 그대로 + 반복
        vertices[i].TexC  = grid.Vertices[i].TexC;
    }

    std::vector<uint16_t> indices = grid.GetIndices16();

    const UINT vbSize = (UINT)(vertices.size() * sizeof(Vertex));
    const UINT ibSize = (UINT)(indices.size()  * sizeof(uint16_t));

    auto geo  = std::make_unique<MeshGeometry>();
    geo->Name = "landGeo";

    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    bufDesc.Width = vbSize;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&geo->VertexBufferGPU));
    {
        UINT8* p; D3D12_RANGE r = { 0, 0 };
        geo->VertexBufferGPU->Map(0, &r, reinterpret_cast<void**>(&p));
        memcpy(p, vertices.data(), vbSize);
        geo->VertexBufferGPU->Unmap(0, nullptr);
    }

    bufDesc.Width = ibSize;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&geo->IndexBufferGPU));
    {
        UINT8* p; D3D12_RANGE r = { 0, 0 };
        geo->IndexBufferGPU->Map(0, &r, reinterpret_cast<void**>(&p));
        memcpy(p, indices.data(), ibSize);
        geo->IndexBufferGPU->Unmap(0, nullptr);
    }

    geo->VertexByteStride     = sizeof(Vertex);
    geo->VertexBufferByteSize = vbSize;
    geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize  = ibSize;

    SubmeshGeometry submesh;
    submesh.IndexCount         = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    geo->DrawArgs["grid"] = submesh;

    mGeometries["landGeo"] = std::move(geo);
}

// BuildBoxGeometry  (BlendDemo 의 BuildBoxGeometry 와 동일한 구조)
void Dx12Renderer::BuildBoxGeometry()
{
    // BoxApp.cpp 와 동일한 정점/인덱스
    // 8 정점 박스 — vertex color 는 white 로 두고 텍스처가 그대로 보이게,
    // UV 는 [0..1] 큐브 좌표로 (정확한 박스 매핑은 아니지만 색 변화는 보임)
    std::array<Vertex, 8> vertices =
    {
        Vertex({ XMFLOAT3(-2.0f, -0.0f, -1.0f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,1) }),
        Vertex({ XMFLOAT3(-0.0f, +2.0f, -1.0f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,0) }),
        Vertex({ XMFLOAT3(+2.0f, +0.0f, -1.0f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,0) }),
        Vertex({ XMFLOAT3(+0.0f, -2.0f, -1.0f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,1) }),
        Vertex({ XMFLOAT3(-2.0f, -0.0f, +1.0f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,1) }),
        Vertex({ XMFLOAT3(-0.0f, +2.0f, +1.0f), XMFLOAT4(1,1,1,1), XMFLOAT2(0,0) }),
        Vertex({ XMFLOAT3(+2.0f, +0.0f, +1.0f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,0) }),
        Vertex({ XMFLOAT3(+0.0f, -2.0f, +1.0f), XMFLOAT4(1,1,1,1), XMFLOAT2(1,1) }),
    };
    std::array<uint16_t, 36> indices =
    {
        0,1,2,  0,2,3,
        4,6,5,  4,7,6,
        4,5,1,  4,1,0,
        3,2,6,  3,6,7,
        1,5,6,  1,6,2,
        4,0,3,  4,3,7
    };

    const UINT vbSize = (UINT)(vertices.size() * sizeof(Vertex));
    const UINT ibSize = (UINT)(indices.size()  * sizeof(uint16_t));

    auto geo  = std::make_unique<MeshGeometry>();
    geo->Name = "boxGeo";

    // midAssignment 의 기존 패턴 유지: upload heap 에 직접 VB/IB 생성
    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // VB
    bufDesc.Width = vbSize;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&geo->VertexBufferGPU));
    {
        UINT8* p; D3D12_RANGE r = { 0, 0 };
        geo->VertexBufferGPU->Map(0, &r, reinterpret_cast<void**>(&p));
        memcpy(p, vertices.data(), vbSize);
        geo->VertexBufferGPU->Unmap(0, nullptr);
    }

    // IB
    bufDesc.Width = ibSize;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&geo->IndexBufferGPU));
    {
        UINT8* p; D3D12_RANGE r = { 0, 0 };
        geo->IndexBufferGPU->Map(0, &r, reinterpret_cast<void**>(&p));
        memcpy(p, indices.data(), ibSize);
        geo->IndexBufferGPU->Unmap(0, nullptr);
    }

    geo->VertexByteStride     = sizeof(Vertex);
    geo->VertexBufferByteSize = vbSize;
    geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize  = ibSize;

    SubmeshGeometry submesh;
    submesh.IndexCount         = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    geo->DrawArgs["box"] = submesh;

    mGeometries["boxGeo"] = std::move(geo);
}

// BuildWavesGeometry  (BlendDemo::BuildWavesGeometry 와 동일한 인덱스 구성)
void Dx12Renderer::BuildWavesGeometry()
{
    const int m = mWaves->RowCount();
    const int n = mWaves->ColumnCount();
    std::vector<uint16_t> indices(3 * mWaves->TriangleCount());
    assert(mWaves->VertexCount() < 0x0000ffff);

    int k = 0;
    for (int i = 0; i < m - 1; ++i)
    {
        for (int j = 0; j < n - 1; ++j)
        {
            indices[k]     = i * n + j;
            indices[k + 1] = i * n + j + 1;
            indices[k + 2] = (i + 1) * n + j;

            indices[k + 3] = (i + 1) * n + j;
            indices[k + 4] = i * n + j + 1;
            indices[k + 5] = (i + 1) * n + j + 1;
            k += 6;
        }
    }

    const UINT vbSize = (UINT)(mWaves->VertexCount() * sizeof(Vertex));
    const UINT ibSize = (UINT)(indices.size() * sizeof(uint16_t));

    auto geo  = std::make_unique<MeshGeometry>();
    geo->Name = "waterGeo";

    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // 동적 VB (persistent map) — BlendDemo 는 FrameResource 당 하나씩 두지만
    // 프레임 리소스가 없으므로 단일 upload 버퍼 사용
    bufDesc.Width = vbSize;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mWavesDynamicVB));

    D3D12_RANGE readRange = { 0, 0 };
    mWavesDynamicVB->Map(0, &readRange, reinterpret_cast<void**>(&mWavesMappedVertices));

    // 초기 정점: position/color 외에 UV 도 채워둔다 (UV 는 매 프레임 변하지 않음)
    const int row = mWaves->RowCount();
    const int col = mWaves->ColumnCount();
    for (int i = 0; i < mWaves->VertexCount(); ++i)
    {
        mWavesMappedVertices[i].Pos   = mWaves->Position(i);
        mWavesMappedVertices[i].Color = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.6f); // 텍스처 그대로 + 반투명
        int r = i / col, c = i % col;
        mWavesMappedVertices[i].TexC  = XMFLOAT2((float)c / (col - 1), (float)r / (row - 1));
    }

    geo->VertexBufferGPU = mWavesDynamicVB;

    // 정적 IB
    bufDesc.Width = ibSize;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&geo->IndexBufferGPU));
    {
        UINT8* p;
        geo->IndexBufferGPU->Map(0, &readRange, reinterpret_cast<void**>(&p));
        memcpy(p, indices.data(), ibSize);
        geo->IndexBufferGPU->Unmap(0, nullptr);
    }

    geo->VertexByteStride     = sizeof(Vertex);
    geo->VertexBufferByteSize = vbSize;
    geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize  = ibSize;

    SubmeshGeometry submesh;
    submesh.IndexCount         = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    geo->DrawArgs["grid"] = submesh;

    mGeometries["waterGeo"] = std::move(geo);
}

// ═════════════════════════════════════════════════════════════════════
// BuildPSOs  (BlendDemo 의 opaque/transparent/alphaTested 중 opaque 만)
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc = {};
    opaquePsoDesc.InputLayout    = { mInputLayout.data(), (UINT)mInputLayout.size() };
    opaquePsoDesc.pRootSignature = mRootSignature.Get();
    opaquePsoDesc.VS = { mShaders["standardVS"]->GetBufferPointer(),
                        mShaders["standardVS"]->GetBufferSize() };
    opaquePsoDesc.PS = { mShaders["opaquePS"]->GetBufferPointer(),
                        mShaders["opaquePS"]->GetBufferSize() };

    D3D12_RASTERIZER_DESC rast = {};
    rast.FillMode              = D3D12_FILL_MODE_SOLID;
    rast.CullMode              = D3D12_CULL_MODE_BACK;
    rast.FrontCounterClockwise = FALSE;
    rast.DepthClipEnable       = TRUE;
    opaquePsoDesc.RasterizerState = rast;

    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    opaquePsoDesc.BlendState = blend;

    D3D12_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable    = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    ds.StencilEnable  = FALSE;
    opaquePsoDesc.DepthStencilState = ds;

    opaquePsoDesc.SampleMask            = UINT_MAX;
    opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    opaquePsoDesc.NumRenderTargets      = 1;
    opaquePsoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    opaquePsoDesc.DSVFormat             = DXGI_FORMAT_D24_UNORM_S8_UINT;
    opaquePsoDesc.SampleDesc.Count      = 1;

    ComPtr<ID3D12PipelineState> opaquePso;
    device->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&opaquePso));
    mPSOs["opaque"] = opaquePso;

    // ── Transparent PSO (BlendDemo::BuildPSOs 의 transparent 와 동일) ──
    // 알파 블렌딩: src*srcAlpha + dst*(1-srcAlpha)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = opaquePsoDesc;

    D3D12_RENDER_TARGET_BLEND_DESC rtBlend = {};
    rtBlend.BlendEnable           = TRUE;
    rtBlend.LogicOpEnable         = FALSE;
    rtBlend.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    rtBlend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp               = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    transparentPsoDesc.BlendState.RenderTarget[0] = rtBlend;

    ComPtr<ID3D12PipelineState> transparentPso;
    device->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&transparentPso));
    mPSOs["transparent"] = transparentPso;
    mPSOs["waves"]       = transparentPso;

    // ── AlphaTested PSO (BlendDemo::BuildPSOs 의 alphaTested 와 동일) ──
    // WireFence 처럼 알파 구멍이 있는 오브젝트: CullNone + alphaTestedPS + clip()
    D3D12_GRAPHICS_PIPELINE_STATE_DESC alphaTestPsoDesc = opaquePsoDesc;
    alphaTestPsoDesc.PS = { mShaders["alphaTestedPS"]->GetBufferPointer(),
                            mShaders["alphaTestedPS"]->GetBufferSize() };
    alphaTestPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // 양면 렌더링

    ComPtr<ID3D12PipelineState> alphaTestPso;
    device->CreateGraphicsPipelineState(&alphaTestPsoDesc, IID_PPV_ARGS(&alphaTestPso));
    mPSOs["alphaTested"] = alphaTestPso;
}

// ═════════════════════════════════════════════════════════════════════
// BuildFrameResources  (BlendDemo 대비: MainPassCB 없음, 단일 ObjectCB)
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::BuildFrameResources()
{
    // mAllRitems 크기만큼 슬롯 할당
    mObjectCBByteSize = (sizeof(ObjectConstants) + 255) & ~255;
    const UINT cbSize = mObjectCBByteSize * (UINT)mAllRitems.size();

    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = cbSize;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mObjectCB));

    D3D12_RANGE readRange = { 0, 0 };
    mObjectCB->Map(0, &readRange, reinterpret_cast<void**>(&mObjectCBMapped));
}

// ═════════════════════════════════════════════════════════════════════
// BuildRenderItems  (BlendDemo 와 동일: waves ritem + opaque ritems)
// midAssignment 의 maxShapes(3) 만큼 박스 ritem 을 미리 만들고
// 그리기는 shapeCount 만큼만 수행 — AddShape() 로직은 그대로 유지된다.
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::BuildRenderItems()
{
    UINT objCBIndex = 0;

    // BlendDemo 는 카메라 거리 200 정도에서 160x160 grid/waves 를 본다.
    // 우리 카메라 반경은 5~15 이므로 약 0.1배 스케일로 축소해서 배치한다.
    const float kWorldScale = 0.1f;

    // ── Waves RenderItem (RenderLayer::Waves) — water 텍스처 ──
    {
        auto wavesRitem = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&wavesRitem->World,
            XMMatrixScaling(kWorldScale, kWorldScale, kWorldScale));
        wavesRitem->ObjCBIndex        = objCBIndex++;
        wavesRitem->Geo               = mGeometries["waterGeo"].get();
        wavesRitem->PrimitiveType     = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        wavesRitem->IndexCount        = wavesRitem->Geo->DrawArgs["grid"].IndexCount;
        wavesRitem->StartIndexLocation= wavesRitem->Geo->DrawArgs["grid"].StartIndexLocation;
        wavesRitem->BaseVertexLocation= wavesRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
        wavesRitem->SrvHeapIndex      = 1; // water

        mWavesRitem = wavesRitem.get();
        mRitemLayer[(int)RenderLayer::Waves].push_back(wavesRitem.get());
        mAllRitems.push_back(std::move(wavesRitem));
    }

    // ── Land RenderItem (RenderLayer::Opaque, 항상 그려짐) — grass 텍스처 ──
    {
        auto landRitem = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&landRitem->World,
            XMMatrixScaling(kWorldScale, kWorldScale, kWorldScale));
        landRitem->ObjCBIndex        = objCBIndex++;
        landRitem->Geo               = mGeometries["landGeo"].get();
        landRitem->PrimitiveType     = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        landRitem->IndexCount        = landRitem->Geo->DrawArgs["grid"].IndexCount;
        landRitem->StartIndexLocation= landRitem->Geo->DrawArgs["grid"].StartIndexLocation;
        landRitem->BaseVertexLocation= landRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
        landRitem->SrvHeapIndex      = 0; // grass

        mRitemLayer[(int)RenderLayer::Opaque].push_back(landRitem.get());
        mAllRitems.push_back(std::move(landRitem));
    }

    // ── Box RenderItem × maxShapes (RenderLayer::Opaque) — fence 텍스처 ──
    XMFLOAT3 positions[] =
    {
        {  0.0f, 1.5f, 0.0f },
        { -3.0f, 1.5f, 0.0f },
        {  3.0f, 1.5f, 0.0f },
    };
    for (int i = 0; i < maxShapes; ++i)
    {
        auto boxRitem = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&boxRitem->World,
            XMMatrixTranslation(positions[i].x, positions[i].y, positions[i].z));
        boxRitem->ObjCBIndex        = objCBIndex++;
        boxRitem->Geo               = mGeometries["boxGeo"].get();
        boxRitem->PrimitiveType     = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        boxRitem->IndexCount        = boxRitem->Geo->DrawArgs["box"].IndexCount;
        boxRitem->StartIndexLocation= boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
        boxRitem->BaseVertexLocation= boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
        boxRitem->SrvHeapIndex      = 2; // fence

        // WireFence 텍스처는 alphaTested 레이어 (BlendDemo 와 동일)
        mRitemLayer[(int)RenderLayer::AlphaTested].push_back(boxRitem.get());
        mAllRitems.push_back(std::move(boxRitem));
    }
}

// ═════════════════════════════════════════════════════════════════════
// BuildDepthStencil
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::BuildDepthStencil()
{
    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width            = width;
    depthDesc.Height           = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels        = 1;
    depthDesc.Format           = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format               = DXGI_FORMAT_D24_UNORM_S8_UINT;
    optClear.DepthStencil.Depth   = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &optClear, IID_PPV_ARGS(&depthStencilBuffer));

    device->CreateDepthStencilView(depthStencilBuffer.Get(), nullptr,
        dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

// ═════════════════════════════════════════════════════════════════════
// DrawRenderItems  (BlendDemo::DrawRenderItems 와 동일 패턴)
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::DrawRenderItems(ID3D12GraphicsCommandList* Cmd,
                                   const std::vector<RenderItem*>& Ritems)
{
    for (auto ri : Ritems)
    {
        D3D12_VERTEX_BUFFER_VIEW vbv = ri->Geo->VertexBufferView();
        D3D12_INDEX_BUFFER_VIEW  ibv = ri->Geo->IndexBufferView();
        Cmd->IASetVertexBuffers(0, 1, &vbv);
        Cmd->IASetIndexBuffer(&ibv);
        Cmd->IASetPrimitiveTopology(ri->PrimitiveType);

        // slot 0: SRV descriptor table → ri->SrvHeapIndex 위치의 SRV
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
        srvHandle.ptr += (UINT64)ri->SrvHeapIndex * srvDescriptorSize;
        Cmd->SetGraphicsRootDescriptorTable(0, srvHandle);

        // slot 1: ObjectCB
        D3D12_GPU_VIRTUAL_ADDRESS cbAddress =
            mObjectCB->GetGPUVirtualAddress()
            + (UINT64)ri->ObjCBIndex * mObjectCBByteSize;
        Cmd->SetGraphicsRootConstantBufferView(1, cbAddress);

        Cmd->DrawIndexedInstanced(ri->IndexCount, 1,
            ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

// ═════════════════════════════════════════════════════════════════════
// PopulateCommandList / Render  (BlendDemo::Draw 순서)
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::PopulateCommandList()
{
    commandAllocator->Reset();
    commandList->Reset(commandAllocator.Get(), mPSOs["opaque"].Get());

    D3D12_VIEWPORT viewport = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT     scissorRect = { 0, 0, width, height };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    // Present → RenderTarget
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource   = renderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (SIZE_T)frameIndex * rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[] = { 0.69f, 0.77f, 0.87f, 1.0f }; // LightSteelBlue
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsvHandle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // SRV 힙을 셰이더에 바인딩
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    commandList->SetGraphicsRootSignature(mRootSignature.Get());

    // ① Opaque 레이어 (기본 PSO) — land 만
    commandList->SetPipelineState(mPSOs["opaque"].Get());
    DrawRenderItems(commandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    // ② AlphaTested 레이어 (CullNone + clip) — fence 박스 shapeCount 만큼
    commandList->SetPipelineState(mPSOs["alphaTested"].Get());
    {
        const auto& at = mRitemLayer[(int)RenderLayer::AlphaTested];
        std::vector<RenderItem*> active;
        for (int i = 0; i < shapeCount && i < (int)at.size(); ++i)
            active.push_back(at[i]);
        DrawRenderItems(commandList.Get(), active);
    }

    // ③ Waves 레이어 (transparent PSO)
    commandList->SetPipelineState(mPSOs["waves"].Get());
    DrawRenderItems(commandList.Get(), mRitemLayer[(int)RenderLayer::Waves]);

    // RenderTarget → Present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &barrier);
    commandList->Close();
}

void Dx12Renderer::Render()
{
    Update();
    PopulateCommandList();
    ID3D12CommandList* ppLists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(_countof(ppLists), ppLists);
    swapChain->Present(0, 0);
    WaitForPreviousFrame();
}

// ═════════════════════════════════════════════════════════════════════
// 마우스 입력 / AddShape (midAssignment 기존 로직 그대로)
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::OnMouseDown(int BtnState, int X, int Y)
{
    mLastMousePos.x = X;
    mLastMousePos.y = Y;
}

void Dx12Renderer::OnMouseUp(int BtnState, int X, int Y)
{
    // 특별한 처리 없음
}

void Dx12Renderer::OnMouseMove(int BtnState, int X, int Y)
{
    if (BtnState & MK_LBUTTON) // 왼쪽 버튼: 회전
    {
        float dx = XMConvertToRadians(0.25f * (float)(X - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * (float)(Y - mLastMousePos.y));
        mTheta += dx;
        mPhi   += dy;
        mPhi = mPhi < 0.1f ? 0.1f : (mPhi > XM_PI - 0.1f ? XM_PI - 0.1f : mPhi);
    }
    else if (BtnState & MK_RBUTTON) // 오른쪽 버튼: 줌
    {
        float dx = 0.005f * (float)(X - mLastMousePos.x);
        float dy = 0.005f * (float)(Y - mLastMousePos.y);
        mRadius += dx - dy;
        mRadius = mRadius < 3.0f ? 3.0f : (mRadius > 15.0f ? 15.0f : mRadius);
    }
    mLastMousePos.x = X;
    mLastMousePos.y = Y;
}

void Dx12Renderer::AddShape()
{
    if (shapeCount >= maxShapes) return;
    shapeCount++;
}

// ═════════════════════════════════════════════════════════════════════
// WaitForPreviousFrame / Cleanup
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::WaitForPreviousFrame()
{
    const UINT64 currentFence = fenceValue;
    commandQueue->Signal(fence.Get(), currentFence);
    fenceValue++;
    if (fence->GetCompletedValue() < currentFence)
    {
        fence->SetEventOnCompletion(currentFence, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    frameIndex = swapChain->GetCurrentBackBufferIndex();
}

void Dx12Renderer::Cleanup()
{
    WaitForPreviousFrame();
    if (mObjectCB)        mObjectCB->Unmap(0, nullptr);
    if (mWavesDynamicVB)  mWavesDynamicVB->Unmap(0, nullptr);
    if (fenceEvent)       CloseHandle(fenceEvent);
}
