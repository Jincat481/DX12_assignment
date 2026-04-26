# midAssignment / week5 — DirectX 12 MFC 렌더러

## 프로젝트 개요

DirectX 12 + MFC(CView) 기반 3D 렌더러.  
Frank Luna의 BlendDemo(Chapter 10/11) 아키텍처를 참고하여 구현.

- **브랜치**: `midAssignment`
- **솔루션**: `week5/week5.sln`
- **빌드**: Visual Studio 2022, x64 Debug

---

## 폴더 구조

```
midAssignment/
├── week5/
│   ├── Dx12Renderer.h / .cpp   ← 핵심 렌더러 (모든 DX12 로직)
│   ├── week5View.h / .cpp      ← MFC View (버튼, 마우스, 타이머)
│   ├── Waves.h / .cpp          ← 파도 시뮬레이션
│   └── Resource.h              ← 버튼 ID 정의
├── .gitignore
└── CLAUDE.md
```

**외부 의존 경로** (git 미포함, 로컬에 있어야 함):
- `../../Common/` — GeometryGenerator.cpp, MathHelper.cpp, DDSTextureLoader.cpp
- `../../Textures/` — grass.dds, water1.dds, WireFence.dds, treeArray2.dds

---

## 구현된 기능

### 렌더링 (BlendDemo 구조)
- **지형(Land)**: `GetHillsHeight(x,z) = 0.3*(z*sin(0.1x) + x*cos(0.1z))` 적용한 160×160 grid
- **파도(Waves)**: 128×128 동적 버텍스 버퍼, 0.25초마다 랜덤 교란
- **텍스처**: grass(SRV slot 0), water(slot 1), WireFence(slot 2), treeArray2(slot 3)
- **PSO 4종**: opaque / transparent(파도 알파블렌딩) / alphaTested / tree(Texture2DArray)

### RenderLayer 순서
```
Opaque      → 지형 (grass 텍스처)
AlphaTested → 나무 (Texture2DArray + clip(a-0.1))
Waves       → 파도 (SRC_ALPHA / INV_SRC_ALPHA 블렌딩)
```

### 루트 시그니처
- slot 0: SRV descriptor table (t0) — PS 가시
- slot 1: CBV (b0, ObjectConstants) — VS 가시
- slot 2: CBV (b1, PassConstants) — PS 가시 (낮/밤 라이팅)
- static sampler s0: Linear Wrap

### ObjectConstants / PassConstants (셰이더 CB)
```cpp
struct ObjectConstants { XMFLOAT4X4 WorldViewProj; };
// UpdateObjectCBs: WVP = transpose(world * view * proj)

struct PassConstants {
    XMFLOAT3 SunDir;       float pad0; // 태양으로 향하는 방향
    XMFLOAT3 SunColor;     float pad1; // 직접광 색
    XMFLOAT3 AmbientColor; float pad2; // 환경광 색
};
```

### Vertex 레이아웃 (Land/Waves 공용, 48 byte)
| offset | 필드 | 형식 |
|--------|------|------|
| 0  | POSITION | float3 |
| 12 | COLOR    | float4 |
| 28 | NORMAL   | float3 — 람베르트 라이팅용 |
| 40 | TEXCOORD | float2 |

`TreeVertex`(40 byte) 는 별도 InputLayout 사용 (변경 없음).

---

## 나무 심기 시스템

### 동작 방식
1. **후보 위치**: `BuildTreeSpritesGeometry()`에서 지형 꼭짓점 중 `h > 2.5` 인 위치만 수집
2. **버튼 클릭**: `AddTree()` → 후보 중 랜덤 1개 선택 → `WriteTreeVertex()` 로 VB 기록
3. **드로우 제어**: `mTreeRitem->IndexCount = mTreeCurrentCount * 12` 로 범위 증가
4. **최대 500그루**: `kMaxTrees = 500`, VB/IB 미리 전체 할당

### 나무 형태
- 십자형 billboard: 정면 quad(4정점) + 측면 quad(4정점) = 8정점/12인덱스
- `treeArray2.dds` Texture2DArray, 슬라이스 랜덤 선택으로 종류 다양화
- 크기: halfW=1.6, treeH=4.5 (WorldScale 0.1 적용 후 적절한 크기)

### 저장/불러오기
- 포맷: `[int count][XMFLOAT3 × count]` 바이너리
- 확장자: `.tree`
- `CFileDialog` 사용 → 저장/열기 탐색창

---

## FPS 카메라 (Chapter 15 방식)

### 조작법
| 입력 | 동작 |
|------|------|
| W / S | 앞뒤 이동 (Walk) |
| A / D | 좌우 이동 (Strafe) |
| 마우스 좌클릭 드래그 | 시점 회전 (Pitch + RotateY) |

### 구현 포인트
- `Camera.h` 미사용 (d3dUtil.h 의존성 문제) → 로직을 Dx12Renderer에 직접 이식
- `OnKeyboardInput(float Dt)`: `GetAsyncKeyState` 로 매 프레임 키 상태 확인
- `UpdateCamera()`: Camera::UpdateViewMatrix() 동일 로직으로 뷰 행렬 재계산
- 초기 위치: `(0, 2, -10)` → 원점 바라보기
- 이동 속도: `8.0f` units/sec

### 연속 렌더링
- `SetTimer(1, 16, nullptr)` — 약 60fps 주기로 `Invalidate(FALSE)` 호출
- WASD 키만 눌러도 화면 업데이트됨

---

## MFC 버튼 (week5View)

```
IDC_BTN_ADD_TREE        (202) → OnBtnAddTree()   → dx12Renderer.AddTree()
IDC_BTN_SAVE            (203) → OnBtnSave()      → CFileDialog → SaveTrees()
IDC_BTN_LOAD            (204) → OnBtnLoad()      → CFileDialog → LoadTrees()
IDC_BTN_TOGGLE_DAYNIGHT (205) → OnBtnDayNight()  → dx12Renderer.ToggleDayNight()
```

---

## 낮/밤 조명 시스템

### 동작 방식
1. **버튼 클릭**: `OnBtnDayNight()` → `dx12Renderer.ToggleDayNight()` → `mIsNight` 플래그 토글
2. **매 프레임 보간**: `UpdatePassCB(dt)` 가 `mDayBlend` 를 목표값(낮=1 / 밤=0)으로 1초 안에 선형 보간
3. **PassConstants 기록**: 보간된 `mDayBlend` 로 `SunDir / SunColor / AmbientColor / 하늘색` 을 lerp 해서 PassCB 에 기록
4. **셰이더 적용**: PS 에서 `tex * (Ambient + SunColor * saturate(dot(N, SunDir)))` 람베르트 라이팅
5. **하늘색**: `mClearColor` 가 같은 보간값으로 갱신되어 `ClearRenderTargetView` 에 사용

### 라이팅 파라미터
| 항목         | 낮                        | 밤                        |
|--------------|---------------------------|---------------------------|
| SunDir       | (0.6, 0.7, -0.4) 정규화   | (-0.3, 0.5, 0.6) 정규화   |
| SunColor     | (1.00, 0.96, 0.85) 따뜻함 | (0.10, 0.12, 0.25) 푸름    |
| AmbientColor | (0.35, 0.35, 0.40)        | (0.05, 0.05, 0.10)        |
| 하늘색       | (0.69, 0.77, 0.87)        | (0.02, 0.02, 0.07)        |

### 셰이더별 적용
- **standardVS / PS / PS_AlphaTest**: 정점 NORMAL 을 받아 람베르트 (지형/파도)
- **TreeVS / TreePS**: 빌보드라 노멀이 부정확 → `Ambient + 0.5 * SunColor` 균일 톤으로 단순 모듈레이션

---

## 주요 멤버 변수

```cpp
// 나무
std::vector<XMFLOAT3>  mTreeCandidates;   // 심을 수 있는 후보 위치
std::vector<XMFLOAT3>  mTreePositions;    // 현재 심긴 나무 위치
TreeVertex*            mTreeMappedVertices; // persistent mapped VB
int                    mTreeCurrentCount;  // 현재 나무 수
RenderItem*            mTreeRitem;         // IndexCount 제어용 포인터

// 카메라
XMFLOAT3 mEyePos, mCamRight, mCamUp, mCamLook;

// 파도
std::unique_ptr<Waves> mWaves;
Vertex*                mWavesMappedVertices;

// 낮/밤
bool      mIsNight;       // 토글 목표 (false=낮, true=밤)
float     mDayBlend;      // 1=낮, 0=밤 (1초간 보간)
XMFLOAT4  mClearColor;    // 하늘색, 매 프레임 갱신
ComPtr<ID3D12Resource> mPassCB;
BYTE*     mPassCBMapped;
```

---

## 빌드 시 주의

- `d3dUtil.cpp` 는 프로젝트에 **포함하지 않음** (`gNumFrameResources` 링크 에러)
- Common 파일 3개만 PCH NotUsing 으로 포함:
  - `GeometryGenerator.cpp`
  - `MathHelper.cpp`
  - `DDSTextureLoader.cpp`
- 세계 스케일: `kWorldScale = 0.1f` (지형 160→16 유닛)
