#include "pch.h"
#include "Dx12Renderer.h"

// 단위행렬 헬퍼
static XMFLOAT4X4 Identity4x4()
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixIdentity());
    return m;
}

bool Dx12Renderer::Initialize(HWND Hwnd, int Width, int Height)
{
    if (!LoadPipeline(Hwnd, Width, Height)) return false;
    if (!LoadAssets()) return false;
    return true;
}

bool Dx12Renderer::LoadPipeline(HWND Hwnd, int Width, int Height)
{
    width = Width;
    height = Height;

#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = frameCount;
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
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
    if (!BuildDescriptorHeaps())    return false;
    if (!BuildConstantBuffers())    return false;
    if (!BuildRootSignature())      return false;
    if (!BuildDepthStencil())       return false;

    ComPtr<ID3DBlob> vsByteCode, psByteCode;
    if (!BuildShadersAndInputLayout(vsByteCode, psByteCode)) return false;
    if (!BuildBoxGeometry())        return false;
    if (!BuildPSO(vsByteCode, psByteCode)) return false;

    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocator.Get(), pipelineState.Get(),
        IID_PPV_ARGS(&commandList));
    commandList->Close();

    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fenceValue = 1;
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // 투영행렬 (BoxApp과 동일: fov=0.25*Pi)
    float aspect = (float)width / (float)height;
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 1.0f, 1000.0f);
    XMStoreFloat4x4(&projMatrix, P);
    worldMatrix = Identity4x4();
    viewMatrix = Identity4x4();

    WaitForPreviousFrame();
    return true;
}

bool Dx12Renderer::BuildDescriptorHeaps()
{
    // RTV 힙
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = frameCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
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
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap));
    dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // CBV 힙
    D3D12_DESCRIPTOR_HEAP_DESC cbvDesc = {};
    cbvDesc.NumDescriptors = 1;
    cbvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&cbvDesc, IID_PPV_ARGS(&cbvHeap));

    return true;
}

bool Dx12Renderer::BuildDepthStencil()
{
    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &optClear, IID_PPV_ARGS(&depthStencilBuffer));

    device->CreateDepthStencilView(depthStencilBuffer.Get(), nullptr,
        dsvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

bool Dx12Renderer::BuildConstantBuffers()
{
    const UINT cbSize = (sizeof(ObjectConstants) + 255) & ~255; // 256바이트 정렬

    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = cbSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&constantBuffer));

    D3D12_RANGE readRange = { 0, 0 };
    constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&cbMappedData));

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = cbSize;
    device->CreateConstantBufferView(&cbvDesc,
        cbvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

bool Dx12Renderer::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE cbvRange = {};
    cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbvRange.NumDescriptors = 1;
    cbvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &cbvRange;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 1;
    rootSigDesc.pParameters = &rootParam;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (error) OutputDebugStringA((char*)error->GetBufferPointer());
    if (FAILED(hr)) return false;

    device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    return true;
}

bool Dx12Renderer::BuildShadersAndInputLayout(
    ComPtr<ID3DBlob>& VsByteCode, ComPtr<ID3DBlob>& PsByteCode)
{
    // color.hlsl 과 동일
    const char* shaderSource = R"(
        cbuffer cbPerObject : register(b0)
        {
            float4x4 gWorldViewProj;
        };
        struct VertexIn  { float3 PosL : POSITION; float4 Color : COLOR; };
        struct VertexOut { float4 PosH : SV_POSITION; float4 Color : COLOR; };

        VertexOut VS(VertexIn vin)
        {
            VertexOut vout;
            vout.PosH  = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
            vout.Color = vin.Color;
            return vout;
        }
        float4 PS(VertexOut pin) : SV_Target { return pin.Color; }
    )";

    ComPtr<ID3DBlob> error;
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    if (FAILED(D3DCompile(shaderSource, strlen(shaderSource), nullptr,
        nullptr, nullptr, "VS", "vs_5_0", flags, 0, &VsByteCode, &error)))
        return false;
    if (FAILED(D3DCompile(shaderSource, strlen(shaderSource), nullptr,
        nullptr, nullptr, "PS", "ps_5_0", flags, 0, &PsByteCode, &error)))
        return false;

    return true;
}

bool Dx12Renderer::BuildBoxGeometry()
{
    // BoxApp.cpp 와 동일한 정점/인덱스
    std::array<Vertex, 8> vertices =
    {
        Vertex({ XMFLOAT3(-2.0f, -0.0f, -1.0f), XMFLOAT4(1,1,1,1) }),  // White
        Vertex({ XMFLOAT3(-0.0f, +2.0f, -1.0f), XMFLOAT4(0,0,0,1) }),  // Black
        Vertex({ XMFLOAT3(+2.0f, +0.0f, -1.0f), XMFLOAT4(1,0,0,1) }),  // Red
        Vertex({ XMFLOAT3(+0.0f, -2.0f, -1.0f), XMFLOAT4(0,1,0,1) }),  // Green
        Vertex({ XMFLOAT3(-2.0f, -0.0f, +1.0f), XMFLOAT4(0,0,1,1) }),  // Blue
        Vertex({ XMFLOAT3(-0.0f, +2.0f, +1.0f), XMFLOAT4(1,1,0,1) }),  // Yellow
        Vertex({ XMFLOAT3(+2.0f, +0.0f, +1.0f), XMFLOAT4(0,1,1,1) }),  // Cyan
        Vertex({ XMFLOAT3(+0.0f, -2.0f, +1.0f), XMFLOAT4(1,0,1,1) }),  // Magenta
    };

    std::array<uint16_t, 36> indices =
    {
        0,1,2,  0,2,3,   // front
        4,6,5,  4,7,6,   // back
        4,5,1,  4,1,0,   // left
        3,2,6,  3,6,7,   // right
        1,5,6,  1,6,2,   // top
        4,0,3,  4,3,7    // bottom
    };
    indexCount = (UINT)indices.size();

    const UINT vbSize = (UINT)(vertices.size() * sizeof(Vertex));
    const UINT ibSize = (UINT)(indices.size() * sizeof(uint16_t));

    D3D12_HEAP_PROPERTIES heapProp = {};
    heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

    // VertexBuffer
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = vbSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&vertexBuffer));

    UINT8* pData; D3D12_RANGE readRange = { 0,0 };
    vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, vertices.data(), vbSize);
    vertexBuffer->Unmap(0, nullptr);

    vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vertexBufferView.StrideInBytes = sizeof(Vertex);
    vertexBufferView.SizeInBytes = vbSize;

    // IndexBuffer
    bufDesc.Width = ibSize;
    device->CreateCommittedResource(&heapProp, D3D12_HEAP_FLAG_NONE,
        &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&indexBuffer));

    indexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pData));
    memcpy(pData, indices.data(), ibSize);
    indexBuffer->Unmap(0, nullptr);

    indexBufferView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
    indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    indexBufferView.SizeInBytes = ibSize;

    return true;
}

bool Dx12Renderer::BuildPSO(
    ComPtr<ID3DBlob>& VsByteCode, ComPtr<ID3DBlob>& PsByteCode)
{
    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rastDesc.CullMode = D3D12_CULL_MODE_BACK;
    rastDesc.FrontCounterClockwise = FALSE;
    rastDesc.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // 깊이 스텐실 (BoxApp 과 동일)
    D3D12_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    dsDesc.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    ZeroMemory(&psoDesc, sizeof(psoDesc));
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = { VsByteCode->GetBufferPointer(), VsByteCode->GetBufferSize() };
    psoDesc.PS = { PsByteCode->GetBufferPointer(), PsByteCode->GetBufferSize() };
    psoDesc.RasterizerState = rastDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = dsDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count = 1;

    return SUCCEEDED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)));
}

void Dx12Renderer::Update()
{
    // BoxApp 과 동일한 구면좌표 → 카메라 위치 계산
    float x = mRadius * sinf(mPhi) * cosf(mTheta);
    float z = mRadius * sinf(mPhi) * sinf(mTheta);
    float y = mRadius * cosf(mPhi);

    XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
    XMMATRIX proj = XMLoadFloat4x4(&projMatrix);
    XMMATRIX wvp = XMMatrixTranspose(world * view * proj);

    ObjectConstants cb;
    XMStoreFloat4x4(&cb.worldViewProj, wvp);
    memcpy(cbMappedData, &cb, sizeof(ObjectConstants));
}

void Dx12Renderer::PopulateCommandList()
{
    commandAllocator->Reset();
    commandList->Reset(commandAllocator.Get(), pipelineState.Get());

    ID3D12DescriptorHeap* heaps[] = { cbvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature.Get());
    commandList->SetGraphicsRootDescriptorTable(0,
        cbvHeap->GetGPUDescriptorHandleForHeapStart());

    D3D12_VIEWPORT viewport = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, width, height };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    // Present → RenderTarget
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = renderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
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

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    commandList->IASetIndexBuffer(&indexBufferView);
    commandList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);

    // RenderTarget → Present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
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

// ── 마우스 입력 (BoxApp 방식) ──────────────────────────────────────
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
        mPhi += dy;
        // mPhi 범위 제한
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
    if (constantBuffer) constantBuffer->Unmap(0, nullptr);
    CloseHandle(fenceEvent);
}