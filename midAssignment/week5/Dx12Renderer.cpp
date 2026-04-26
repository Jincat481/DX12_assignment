#include "pch.h"
#include "Dx12Renderer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/MathHelper.h"
#include "../../Common/DDSTextureLoader.h"
#include <vector>
#include <cstdlib>
#include <cassert>
#include <algorithm>

// 단위행렬 헬퍼
static XMFLOAT4X4 Identity4x4()
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixIdentity());
    return m;
}

// Initialize / LoadPipeline / LoadAssets   (BlendDemo::Initialize 순서)
bool Dx12Renderer::Initialize(HWND Hwnd, int Width, int Height)
{
    mHwnd = Hwnd; // FPS 타이틀 표시용

    // Waves 시뮬레이터 (BlendDemo 와 동일한 파라미터)
    mWaves = std::make_unique<Waves>(128, 128, 1.0f, 0.03f, 4.0f, 0.2f);

    // 델타타임 계산용 고해상도 카운터
    QueryPerformanceFrequency(&mPerfFreq);
    QueryPerformanceCounter(&mPrevCounter);

    // FPS 카메라 초기 방향 설정: (0,2,-10) → 원점 방향 (Camera::LookAt 동일 계산)
    {
        XMVECTOR P  = XMLoadFloat3(&mEyePos);
        XMVECTOR T  = XMVectorZero();               // 바라볼 타겟 (원점)
        XMVECTOR wu = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR L  = XMVector3Normalize(XMVectorSubtract(T, P));
        XMVECTOR R  = XMVector3Normalize(XMVector3Cross(wu, L));
        XMVECTOR U  = XMVector3Cross(L, R);
        XMStoreFloat3(&mCamLook,  L);
        XMStoreFloat3(&mCamRight, R);
        XMStoreFloat3(&mCamUp,    U);
    }

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
    BuildTreeSpritesGeometry();   // 박스 대신 나무 지오메트리
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

    OnKeyboardInput(dt);  // WASD 키 입력 처리 (Chapter 15)
    UpdateCamera();
    UpdateObjectCBs();
    UpdateWaves(dt);
    UpdatePassCB(dt);     // 낮/밤 보간 + 태양/앰비언트/하늘색 갱신
    CalculateFrameStats();
}

// ─── OnKeyboardInput (Chapter 15::OnKeyboardInput 동일) ───────────────
// WASD: Walk / Strafe
void Dx12Renderer::OnKeyboardInput(float Dt)
{
    const float speed = 8.0f; // 단위/초 (지형 16×16 기준)

    // W/S: Camera::Walk — mEyePos += ±speed * mCamLook
    if (GetAsyncKeyState('W') & 0x8000)
    {
        XMVECTOR p = XMLoadFloat3(&mEyePos);
        XMVECTOR l = XMLoadFloat3(&mCamLook);
        XMStoreFloat3(&mEyePos, XMVectorMultiplyAdd(XMVectorReplicate( speed * Dt), l, p));
    }
    if (GetAsyncKeyState('S') & 0x8000)
    {
        XMVECTOR p = XMLoadFloat3(&mEyePos);
        XMVECTOR l = XMLoadFloat3(&mCamLook);
        XMStoreFloat3(&mEyePos, XMVectorMultiplyAdd(XMVectorReplicate(-speed * Dt), l, p));
    }
    // A/D: Camera::Strafe — mEyePos += ±speed * mCamRight
    if (GetAsyncKeyState('A') & 0x8000)
    {
        XMVECTOR p = XMLoadFloat3(&mEyePos);
        XMVECTOR r = XMLoadFloat3(&mCamRight);
        XMStoreFloat3(&mEyePos, XMVectorMultiplyAdd(XMVectorReplicate(-speed * Dt), r, p));
    }
    if (GetAsyncKeyState('D') & 0x8000)
    {
        XMVECTOR p = XMLoadFloat3(&mEyePos);
        XMVECTOR r = XMLoadFloat3(&mCamRight);
        XMStoreFloat3(&mEyePos, XMVectorMultiplyAdd(XMVectorReplicate( speed * Dt), r, p));
    }
}

// ─── UpdateCamera (Camera::UpdateViewMatrix 와 동일 로직) ─────────────
void Dx12Renderer::UpdateCamera()
{
    XMVECTOR R = XMLoadFloat3(&mCamRight);
    XMVECTOR U = XMLoadFloat3(&mCamUp);
    XMVECTOR L = XMLoadFloat3(&mCamLook);
    XMVECTOR P = XMLoadFloat3(&mEyePos);

    // 기저 벡터를 직교정규화 (누적 오차 방지)
    L = XMVector3Normalize(L);
    U = XMVector3Normalize(XMVector3Cross(L, R));
    R = XMVector3Cross(U, L);

    float x = -XMVectorGetX(XMVector3Dot(P, R));
    float y = -XMVectorGetX(XMVector3Dot(P, U));
    float z = -XMVectorGetX(XMVector3Dot(P, L));

    XMStoreFloat3(&mCamRight, R);
    XMStoreFloat3(&mCamUp,    U);
    XMStoreFloat3(&mCamLook,  L);

    // 뷰 행렬 직접 기록 (Camera.cpp::UpdateViewMatrix 와 동일)
    mView(0,0) = XMVectorGetX(R); mView(1,0) = XMVectorGetY(R); mView(2,0) = XMVectorGetZ(R); mView(3,0) = x;
    mView(0,1) = XMVectorGetX(U); mView(1,1) = XMVectorGetY(U); mView(2,1) = XMVectorGetZ(U); mView(3,1) = y;
    mView(0,2) = XMVectorGetX(L); mView(1,2) = XMVectorGetY(L); mView(2,2) = XMVectorGetZ(L); mView(3,2) = z;
    mView(0,3) = 0.0f;            mView(1,3) = 0.0f;            mView(2,3) = 0.0f;            mView(3,3) = 1.0f;
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

    auto treeTex = std::make_unique<Texture>();
    treeTex->Name     = "treeArrayTex";
    treeTex->Filename = L"../../Textures/treeArray2.dds";
    DirectX::CreateDDSTextureFromFile12(device.Get(), commandList.Get(),
        treeTex->Filename.c_str(),
        treeTex->Resource, treeTex->UploadHeap);

    mTextures[grassTex->Name] = std::move(grassTex);
    mTextures[waterTex->Name] = std::move(waterTex);
    mTextures[fenceTex->Name] = std::move(fenceTex);
    mTextures[treeTex->Name]  = std::move(treeTex);
}

// BuildRootSignature
//   slot 0: SRV descriptor table  (t0) — diffuse texture
//   slot 1: CBV                    (b0) — ObjectCB (WVP)
//   slot 2: CBV                    (b1) — PassCB   (태양 방향/색, 앰비언트)
//   static sampler s0              — Linear Wrap
void Dx12Renderer::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = 1;
    srvRange.BaseShaderRegister                = 0; // t0
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParams[3] = {};

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

    // slot 2: PassCB (PS 에서 라이팅 계산에 사용)
    rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 1; // b1
    rootParams[2].Descriptor.RegisterSpace  = 0;
    rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

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

    // SRV 힙 (텍스처 4개: 0=grass, 1=water, 2=fence, 3=treeArray)
    // BlendDemo 와 동일하게 SHADER_VISIBLE 힙
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 4;
    srvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvHeap));
    srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Texture2D SRV (slot 0~2)
    auto createSrv2D = [&](ID3D12Resource* res, UINT heapSlot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                        = res->GetDesc().Format;
        srvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip     = 0;
        srvDesc.Texture2D.MipLevels           = res->GetDesc().MipLevels;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)heapSlot * srvDescriptorSize;
        device->CreateShaderResourceView(res, &srvDesc, handle);
    };
    createSrv2D(mTextures["grassTex"]->Resource.Get(), 0);
    createSrv2D(mTextures["waterTex"]->Resource.Get(), 1);
    createSrv2D(mTextures["fenceTex"]->Resource.Get(), 2);

    // Texture2DArray SRV (slot 3) — treeArray2.dds
    {
        auto* res = mTextures["treeArrayTex"]->Resource.Get();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping              = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                               = res->GetDesc().Format;
        srvDesc.ViewDimension                        = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip       = 0;
        srvDesc.Texture2DArray.MipLevels             = res->GetDesc().MipLevels;
        srvDesc.Texture2DArray.FirstArraySlice       = 0;
        srvDesc.Texture2DArray.ArraySize             = res->GetDesc().DepthOrArraySize;
        srvDesc.Texture2DArray.ResourceMinLODClamp   = 0.0f;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)3 * srvDescriptorSize;
        device->CreateShaderResourceView(res, &srvDesc, handle);
    }
}

// BuildShadersAndInputLayout
void Dx12Renderer::BuildShadersAndInputLayout()
{
    // 텍스처를 샘플하고 람베르트 라이팅(태양+앰비언트) 적용
    const char* shaderSource = R"(
        cbuffer cbPerObject : register(b0)
        {
            float4x4 gWorldViewProj;
        };
        cbuffer cbPass : register(b1)
        {
            float3 gSunDir;       float gPad0;
            float3 gSunColor;     float gPad1;
            float3 gAmbient;      float gPad2;
        };

        Texture2D    gDiffuseMap : register(t0);
        SamplerState gSampler    : register(s0);

        struct VertexIn
        {
            float3 PosL    : POSITION;
            float4 Color   : COLOR;
            float3 NormalL : NORMAL;
            float2 TexC    : TEXCOORD;
        };
        struct VertexOut
        {
            float4 PosH    : SV_POSITION;
            float4 Color   : COLOR;
            float3 NormalW : NORMAL;
            float2 TexC    : TEXCOORD;
        };

        VertexOut VS(VertexIn vin)
        {
            VertexOut vout;
            vout.PosH    = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
            vout.Color   = vin.Color;
            vout.NormalW = vin.NormalL; // 균등 스케일이라 회전이 없으면 로컬=월드
            vout.TexC    = vin.TexC;
            return vout;
        }
        float4 PS(VertexOut pin) : SV_Target
        {
            float4 tex = gDiffuseMap.Sample(gSampler, pin.TexC);
            float3 N   = normalize(pin.NormalW);
            float ndotl = saturate(dot(N, gSunDir));
            float3 lit  = gAmbient + gSunColor * ndotl;
            float3 rgb  = tex.rgb * pin.Color.rgb * lit;
            return float4(rgb, tex.a * pin.Color.a);
        }

        // BlendDemo::alphaTested PS: 알파가 0.1 미만인 픽셀을 버림
        float4 PS_AlphaTest(VertexOut pin) : SV_Target
        {
            float4 tex = gDiffuseMap.Sample(gSampler, pin.TexC);
            clip(tex.a - 0.1f);   // 울타리 구멍 제거
            float3 N   = normalize(pin.NormalW);
            float ndotl = saturate(dot(N, gSunDir));
            float3 lit  = gAmbient + gSunColor * ndotl;
            float3 rgb  = tex.rgb * pin.Color.rgb * lit;
            return float4(rgb, tex.a * pin.Color.a);
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
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // ── 나무 전용 셰이더 (Texture2DArray, TexC.z = 배열 슬라이스 인덱스) ──
    // 빌보드라 정확한 노멀이 없으므로 라이팅은 (Ambient + 0.5*SunColor) 균일 톤으로 단순 모듈레이션
    const char* treeShaderSrc = R"(
        cbuffer cbPerObject : register(b0)
        {
            float4x4 gWorldViewProj;
        };
        cbuffer cbPass : register(b1)
        {
            float3 gSunDir;       float gPad0;
            float3 gSunColor;     float gPad1;
            float3 gAmbient;      float gPad2;
        };
        Texture2DArray gTreeMapArray : register(t0);
        SamplerState   gSampler      : register(s0);

        struct VertexIn
        {
            float3 PosL  : POSITION;
            float4 Color : COLOR;
            float3 TexC  : TEXCOORD; // xy=uv, z=array slice
        };
        struct VertexOut
        {
            float4 PosH  : SV_POSITION;
            float4 Color : COLOR;
            float3 TexC  : TEXCOORD;
        };

        VertexOut TreeVS(VertexIn vin)
        {
            VertexOut vout;
            vout.PosH  = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
            vout.Color = vin.Color;
            vout.TexC  = vin.TexC;
            return vout;
        }
        float4 TreePS(VertexOut pin) : SV_Target
        {
            float4 tex = gTreeMapArray.Sample(gSampler, pin.TexC); // z = 슬라이스
            clip(tex.a - 0.1f); // 나무 윤곽 밖 픽셀 제거
            float3 lit = gAmbient + gSunColor * 0.5f; // 빌보드 균일 라이팅
            float3 rgb = tex.rgb * pin.Color.rgb * lit;
            return float4(rgb, tex.a * pin.Color.a);
        }
    )";

    ComPtr<ID3DBlob> treeVS, treePS;
    D3DCompile(treeShaderSrc, strlen(treeShaderSrc), nullptr, nullptr, nullptr,
        "TreeVS", "vs_5_0", flags, 0, &treeVS, &error);
    D3DCompile(treeShaderSrc, strlen(treeShaderSrc), nullptr, nullptr, nullptr,
        "TreePS", "ps_5_0", flags, 0, &treePS, &error);

    mShaders["treeVS"] = treeVS;
    mShaders["treePS"] = treePS;

    // TreeVertex 전용 InputLayout (TEXCOORD 가 float3 — z 에 슬라이스 인덱스)
    mTreeInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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
        // 라이팅용 노멀(언덕 표면) — BlendDemo 와 동일 공식
        vertices[i].Normal = GetHillsNormal(p.x, p.z);
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

// ═════════════════════════════════════════════════════════════════════
// BuildTreeSpritesGeometry
// ① 지형에서 수면 위 버텍스를 후보로 수집 → mTreeCandidates
// ② kMaxTrees 그루 수용 동적 VB (persistent map) 생성 → mTreeDynamicVB
// ③ 인덱스 버퍼는 최대 용량으로 미리 채워 두고 (패턴이 고정이므로 한 번만 계산)
//    실제 드로우는 mTreeRitem->IndexCount 로 제어한다.
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::BuildTreeSpritesGeometry()
{
    // ① 후보 위치 수집 (수면보다 높은 곳, local space h > 2.5)
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData grid = geoGen.CreateGrid(160.0f, 160.0f, 50, 50);

    mTreeCandidates.clear();
    for (auto& v : grid.Vertices)
    {
        float h = GetHillsHeight(v.Position.x, v.Position.z);
        if (h > 2.5f)
            mTreeCandidates.push_back(XMFLOAT3(v.Position.x, h, v.Position.z));
    }

    // 슬라이스 수 저장
    mTreeArraySlices = (UINT)mTextures["treeArrayTex"]->Resource->GetDesc().DepthOrArraySize;
    if (mTreeArraySlices == 0) mTreeArraySlices = 4;

    // ② 동적 VB (kMaxTrees × 8 정점, persistent map)
    const UINT vbSize = (UINT)(kMaxTrees * 8 * sizeof(TreeVertex));

    D3D12_HEAP_PROPERTIES heapProp = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = vbSize;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mTreeDynamicVB));
    { D3D12_RANGE r={0,0};
      mTreeDynamicVB->Map(0, &r, reinterpret_cast<void**>(&mTreeMappedVertices)); }

    // ③ 정적 IB (최대 kMaxTrees 그루 × 12 인덱스, 패턴은 고정)
    const UINT ibSize = (UINT)(kMaxTrees * 12 * sizeof(uint16_t));
    bufDesc.Width = ibSize;
    ComPtr<ID3D12Resource> ibRes;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&ibRes));
    {
        uint16_t* p = nullptr; D3D12_RANGE r={0,0};
        ibRes->Map(0, &r, reinterpret_cast<void**>(&p));
        for (int t = 0; t < kMaxTrees; ++t)
        {
            uint16_t base = (uint16_t)(t * 8);
            int      k    = t * 12;
            p[k+ 0]=base+0; p[k+ 1]=base+1; p[k+ 2]=base+2;
            p[k+ 3]=base+0; p[k+ 4]=base+2; p[k+ 5]=base+3;
            p[k+ 6]=base+4; p[k+ 7]=base+5; p[k+ 8]=base+6;
            p[k+ 9]=base+4; p[k+10]=base+6; p[k+11]=base+7;
        }
        ibRes->Unmap(0, nullptr);
    }

    // MeshGeometry 등록
    auto geo  = std::make_unique<MeshGeometry>();
    geo->Name = "treeSpritesGeo";
    geo->VertexBufferGPU      = mTreeDynamicVB;
    geo->IndexBufferGPU       = ibRes;
    geo->VertexByteStride     = sizeof(TreeVertex);
    geo->VertexBufferByteSize = vbSize;
    geo->IndexFormat          = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize  = ibSize;

    SubmeshGeometry submesh;
    submesh.IndexCount         = 0; // 초기 0, AddTree 할 때마다 증가
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    geo->DrawArgs["trees"] = submesh;

    mGeometries["treeSpritesGeo"] = std::move(geo);
    mTreeCurrentCount = 0;
    mTreePositions.clear();
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
        mWavesMappedVertices[i].Pos    = mWaves->Position(i);
        mWavesMappedVertices[i].Color  = XMFLOAT4(1.0f, 1.0f, 1.0f, 0.6f); // 텍스처 그대로 + 반투명
        mWavesMappedVertices[i].Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);       // 평탄 수면 노멀 (간이)
        int r = i / col, c = i % col;
        mWavesMappedVertices[i].TexC   = XMFLOAT2((float)c / (col - 1), (float)r / (row - 1));
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

    // ── Tree PSO (Texture2DArray + TreeVertex InputLayout + clip + CullNone) ──
    D3D12_GRAPHICS_PIPELINE_STATE_DESC treePsoDesc = opaquePsoDesc;
    treePsoDesc.InputLayout = { mTreeInputLayout.data(), (UINT)mTreeInputLayout.size() };
    treePsoDesc.VS = { mShaders["treeVS"]->GetBufferPointer(),
                       mShaders["treeVS"]->GetBufferSize() };
    treePsoDesc.PS = { mShaders["treePS"]->GetBufferPointer(),
                       mShaders["treePS"]->GetBufferSize() };
    treePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    ComPtr<ID3D12PipelineState> treePso;
    device->CreateGraphicsPipelineState(&treePsoDesc, IID_PPV_ARGS(&treePso));
    mPSOs["tree"] = treePso;
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

    // ── PassCB (낮/밤 라이팅용, 256-aligned 단일 블록) ──
    mPassCBByteSize = (sizeof(PassConstants) + 255) & ~255;
    bufDesc.Width = mPassCBByteSize;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mPassCB));
    mPassCB->Map(0, &readRange, reinterpret_cast<void**>(&mPassCBMapped));
}

// ═════════════════════════════════════════════════════════════════════
// UpdatePassCB
// 낮(mDayBlend=1) ↔ 밤(mDayBlend=0) 사이를 1초 동안 부드럽게 보간하고
// 보간 결과를 태양 색/앰비언트/하늘색에 반영해 PassCB 와 mClearColor 에 기록한다.
// ═════════════════════════════════════════════════════════════════════
void Dx12Renderer::UpdatePassCB(float Dt)
{
    // 1초 안에 목표(낮=1 / 밤=0) 도달하도록 선형 보간
    float target = mIsNight ? 0.0f : 1.0f;
    const float speed = 1.0f; // 단위/초 → 1초에 0↔1 전환
    if (mDayBlend < target) mDayBlend = (std::min)(target, mDayBlend + Dt * speed);
    else                    mDayBlend = (std::max)(target, mDayBlend - Dt * speed);

    // 낮/밤 색상 (간이) — 낮: 따뜻한 햇빛 + 밝은 하늘 / 밤: 푸른 달빛 + 어두운 남색 하늘
    XMFLOAT3 sunDay   (0.6f,  0.7f, -0.4f); // 태양으로 향하는 방향 (정규화 전)
    XMFLOAT3 sunNight (-0.3f, 0.5f,  0.6f); // 달 방향 — 반대편에서 오기

    XMFLOAT3 sunColDay  (1.00f, 0.96f, 0.85f);
    XMFLOAT3 sunColNight(0.10f, 0.12f, 0.25f);

    XMFLOAT3 ambDay   (0.35f, 0.35f, 0.40f);
    XMFLOAT3 ambNight (0.05f, 0.05f, 0.10f);

    XMFLOAT4 skyDay   (0.69f, 0.77f, 0.87f, 1.0f); // LightSteelBlue
    XMFLOAT4 skyNight (0.02f, 0.02f, 0.07f, 1.0f); // 거의 검은 남색

    auto lerp3 = [&](const XMFLOAT3& a, const XMFLOAT3& b, float t) {
        return XMFLOAT3(a.x*(1-t)+b.x*t, a.y*(1-t)+b.y*t, a.z*(1-t)+b.z*t);
    };
    auto lerp4 = [&](const XMFLOAT4& a, const XMFLOAT4& b, float t) {
        return XMFLOAT4(a.x*(1-t)+b.x*t, a.y*(1-t)+b.y*t, a.z*(1-t)+b.z*t, a.w*(1-t)+b.w*t);
    };

    // mDayBlend=1 → 낮, 0 → 밤
    XMFLOAT3 sunDirRaw = lerp3(sunNight, sunDay,   mDayBlend);
    XMFLOAT3 sunCol    = lerp3(sunColNight, sunColDay, mDayBlend);
    XMFLOAT3 amb       = lerp3(ambNight, ambDay,   mDayBlend);
    mClearColor        = lerp4(skyNight, skyDay,   mDayBlend);

    // 정규화
    XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&sunDirRaw));
    XMFLOAT3 sunDir; XMStoreFloat3(&sunDir, n);

    PassConstants cb = {};
    cb.SunDir       = sunDir;
    cb.SunColor     = sunCol;
    cb.AmbientColor = amb;
    memcpy(mPassCBMapped, &cb, sizeof(PassConstants));
}

// ToggleDayNight — 버튼 핸들러에서 호출
void Dx12Renderer::ToggleDayNight()
{
    mIsNight = !mIsNight;
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

    // ── Tree RenderItem (RenderLayer::AlphaTested) ──
    {
        auto treeRitem = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&treeRitem->World,
            XMMatrixScaling(kWorldScale, kWorldScale, kWorldScale));
        treeRitem->ObjCBIndex        = objCBIndex++;
        treeRitem->Geo               = mGeometries["treeSpritesGeo"].get();
        treeRitem->PrimitiveType     = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        treeRitem->IndexCount        = 0;  // 초기엔 나무 없음, AddTree() 할 때마다 증가
        treeRitem->StartIndexLocation= 0;
        treeRitem->BaseVertexLocation= 0;
        treeRitem->SrvHeapIndex      = 3;  // treeArray2.dds (Texture2DArray)

        mTreeRitem = treeRitem.get();
        mRitemLayer[(int)RenderLayer::AlphaTested].push_back(treeRitem.get());
        mAllRitems.push_back(std::move(treeRitem));
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

    // 낮/밤 보간 결과로 매 프레임 갱신되는 하늘색
    const float clearColor[] = { mClearColor.x, mClearColor.y, mClearColor.z, mClearColor.w };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsvHandle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // SRV 힙을 셰이더에 바인딩
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    commandList->SetGraphicsRootSignature(mRootSignature.Get());

    // PassCB(b1) 는 모든 드로우에서 동일하므로 한 번만 바인딩한다
    commandList->SetGraphicsRootConstantBufferView(2, mPassCB->GetGPUVirtualAddress());

    // ① Opaque 레이어 (기본 PSO) — land 만
    commandList->SetPipelineState(mPSOs["opaque"].Get());
    DrawRenderItems(commandList.Get(), mRitemLayer[(int)RenderLayer::Opaque]);

    // ② AlphaTested 레이어 — tree PSO (Texture2DArray + clip + CullNone)
    commandList->SetPipelineState(mPSOs["tree"].Get());
    DrawRenderItems(commandList.Get(), mRitemLayer[(int)RenderLayer::AlphaTested]);

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
    if (BtnState & MK_LBUTTON) // 왼쪽 드래그: FPS 시점 회전
    {
        float dx = XMConvertToRadians(0.25f * (float)(X - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * (float)(Y - mLastMousePos.y));

        // Pitch(dy): Camera::Pitch — Right 축 기준 Up, Look 회전
        {
            XMMATRIX R = XMMatrixRotationAxis(XMLoadFloat3(&mCamRight), dy);
            XMStoreFloat3(&mCamUp,   XMVector3TransformNormal(XMLoadFloat3(&mCamUp),   R));
            XMStoreFloat3(&mCamLook, XMVector3TransformNormal(XMLoadFloat3(&mCamLook), R));
        }
        // RotateY(dx): Camera::RotateY — 월드 Y 축 기준 Right, Up, Look 회전
        {
            XMMATRIX R = XMMatrixRotationY(dx);
            XMStoreFloat3(&mCamRight, XMVector3TransformNormal(XMLoadFloat3(&mCamRight), R));
            XMStoreFloat3(&mCamUp,    XMVector3TransformNormal(XMLoadFloat3(&mCamUp),    R));
            XMStoreFloat3(&mCamLook,  XMVector3TransformNormal(XMLoadFloat3(&mCamLook),  R));
        }
    }
    mLastMousePos.x = X;
    mLastMousePos.y = Y;
}

// ═════════════════════════════════════════════════════════════════════
// WaitForPreviousFrame / Cleanup
// ═════════════════════════════════════════════════════════════════════
// ═════════════════════════════════════════════════════════════════════
// 나무 심기 관련
// ═════════════════════════════════════════════════════════════════════

// 정점 하나(그루)를 mapped VB 에 기록 (AddTree / LoadTrees 공용)
void Dx12Renderer::WriteTreeVertex(int Index, const XMFLOAT3& Pos)
{
    const float halfW = 1.6f;
    const float treeH = 4.5f;
    float ai = (float)(rand() % mTreeArraySlices);
    XMFLOAT4 white(1, 1, 1, 1);
    int base = Index * 8;

    mTreeMappedVertices[base+0] = { XMFLOAT3(Pos.x-halfW, Pos.y,       Pos.z), white, XMFLOAT3(0,1,ai) };
    mTreeMappedVertices[base+1] = { XMFLOAT3(Pos.x-halfW, Pos.y+treeH, Pos.z), white, XMFLOAT3(0,0,ai) };
    mTreeMappedVertices[base+2] = { XMFLOAT3(Pos.x+halfW, Pos.y+treeH, Pos.z), white, XMFLOAT3(1,0,ai) };
    mTreeMappedVertices[base+3] = { XMFLOAT3(Pos.x+halfW, Pos.y,       Pos.z), white, XMFLOAT3(1,1,ai) };
    mTreeMappedVertices[base+4] = { XMFLOAT3(Pos.x, Pos.y,       Pos.z-halfW), white, XMFLOAT3(0,1,ai) };
    mTreeMappedVertices[base+5] = { XMFLOAT3(Pos.x, Pos.y+treeH, Pos.z-halfW), white, XMFLOAT3(0,0,ai) };
    mTreeMappedVertices[base+6] = { XMFLOAT3(Pos.x, Pos.y+treeH, Pos.z+halfW), white, XMFLOAT3(1,0,ai) };
    mTreeMappedVertices[base+7] = { XMFLOAT3(Pos.x, Pos.y,       Pos.z+halfW), white, XMFLOAT3(1,1,ai) };
}

// 나무 심기 버튼 핸들러 — 후보 중 랜덤 한 곳에 1 그루 추가
void Dx12Renderer::AddTree()
{
    if (mTreeCurrentCount >= kMaxTrees || mTreeCandidates.empty()) return;

    int idx = rand() % (int)mTreeCandidates.size();
    const XMFLOAT3& pos = mTreeCandidates[idx];

    WriteTreeVertex(mTreeCurrentCount, pos);
    mTreePositions.push_back(pos);
    mTreeCurrentCount++;
    mTreeRitem->IndexCount = (UINT)mTreeCurrentCount * 12;
}

// 저장 — 현재 심긴 나무 위치를 바이너리 파일에 기록
void Dx12Renderer::SaveTrees(const CString& Filename)
{
    FILE* f = nullptr;
    _wfopen_s(&f, Filename.GetString(), L"wb");
    if (!f) return;
    int cnt = (int)mTreePositions.size();
    fwrite(&cnt, sizeof(int), 1, f);
    if (cnt > 0)
        fwrite(mTreePositions.data(), sizeof(XMFLOAT3), cnt, f);
    fclose(f);
}

// 불러오기 — 파일에서 나무 위치를 읽어 VB 재구성
bool Dx12Renderer::LoadTrees(const CString& Filename)
{
    FILE* f = nullptr;
    _wfopen_s(&f, Filename.GetString(), L"rb");
    if (!f) return false;

    int cnt = 0;
    fread(&cnt, sizeof(int), 1, f);
    cnt = min(cnt, kMaxTrees);

    std::vector<XMFLOAT3> positions(cnt);
    if (cnt > 0)
        fread(positions.data(), sizeof(XMFLOAT3), cnt, f);
    fclose(f);

    // VB 재구성
    mTreeCurrentCount = 0;
    mTreePositions.clear();
    for (auto& p : positions)
    {
        WriteTreeVertex(mTreeCurrentCount, p);
        mTreePositions.push_back(p);
        mTreeCurrentCount++;
    }
    mTreeRitem->IndexCount = (UINT)mTreeCurrentCount * 12;
    return true;
}

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
    if (mTreeDynamicVB)   mTreeDynamicVB->Unmap(0, nullptr);
    if (fenceEvent)       CloseHandle(fenceEvent);
}
