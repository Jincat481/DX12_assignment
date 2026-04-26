
// week5View.cpp: Cweek5View 클래스의 구현
//

#include "pch.h"
#include "framework.h"
// SHARED_HANDLERS는 미리 보기, 축소판 그림 및 검색 필터 처리기를 구현하는 ATL 프로젝트에서 정의할 수 있으며
// 해당 프로젝트와 문서 코드를 공유하도록 해 줍니다.
#ifndef SHARED_HANDLERS
#include "week5.h"
#endif

#include "week5Doc.h"
#include "week5View.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// Cweek5View

IMPLEMENT_DYNCREATE(Cweek5View, CView)

BEGIN_MESSAGE_MAP(Cweek5View, CView)
	// 표준 인쇄 명령입니다.
	ON_COMMAND(ID_FILE_PRINT, &CView::OnFilePrint)
	ON_COMMAND(ID_FILE_PRINT_DIRECT, &CView::OnFilePrint)
	ON_COMMAND(ID_FILE_PRINT_PREVIEW, &Cweek5View::OnFilePrintPreview)
	ON_WM_CONTEXTMENU()
	ON_WM_RBUTTONUP()
	ON_WM_DESTROY()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_RBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_BN_CLICKED(IDC_BTN_ADD_TREE,        &Cweek5View::OnBtnAddTree)
	ON_BN_CLICKED(IDC_BTN_SAVE,            &Cweek5View::OnBtnSave)
	ON_BN_CLICKED(IDC_BTN_LOAD,            &Cweek5View::OnBtnLoad)
	ON_BN_CLICKED(IDC_BTN_TOGGLE_DAYNIGHT, &Cweek5View::OnBtnDayNight)
	ON_WM_TIMER()
END_MESSAGE_MAP()

// Cweek5View 생성/소멸

Cweek5View::Cweek5View() noexcept
{
	// TODO: 여기에 생성 코드를 추가합니다.

}

Cweek5View::~Cweek5View()
{
}

BOOL Cweek5View::PreCreateWindow(CREATESTRUCT& cs)
{
	// TODO: CREATESTRUCT cs를 수정하여 여기에서
	//  Window 클래스 또는 스타일을 수정합니다.

	return CView::PreCreateWindow(cs);
}

// Cweek5View 그리기

void Cweek5View::OnDraw(CDC* /*pDC*/)
{
	Cweek5Doc* pDoc = GetDocument();
	ASSERT_VALID(pDoc);
	if (!pDoc)
		return;

	// TODO: 여기에 원시 데이터에 대한 그리기 코드를 추가합니다.
	dx12Renderer.Render();
}


// Cweek5View 인쇄


void Cweek5View::OnFilePrintPreview()
{
#ifndef SHARED_HANDLERS
	AFXPrintPreview(this);
#endif
}

BOOL Cweek5View::OnPreparePrinting(CPrintInfo* pInfo)
{
	// 기본적인 준비
	return DoPreparePrinting(pInfo);
}

void Cweek5View::OnBeginPrinting(CDC* /*pDC*/, CPrintInfo* /*pInfo*/)
{
	// TODO: 인쇄하기 전에 추가 초기화 작업을 추가합니다.
}

void Cweek5View::OnEndPrinting(CDC* /*pDC*/, CPrintInfo* /*pInfo*/)
{
	// TODO: 인쇄 후 정리 작업을 추가합니다.
}

void Cweek5View::OnRButtonUp(UINT nFlags, CPoint point)
{
	ClientToScreen(&point);
	OnContextMenu(this, point);
	ReleaseCapture();
	dx12Renderer.OnMouseUp(nFlags, point.x, point.y);
}

void Cweek5View::OnContextMenu(CWnd* /* pWnd */, CPoint point)
{
#ifndef SHARED_HANDLERS
	theApp.GetContextMenuManager()->ShowPopupMenu(IDR_POPUP_EDIT, point.x, point.y, this, TRUE);
#endif
}


// Cweek5View 진단

#ifdef _DEBUG
void Cweek5View::AssertValid() const
{
	CView::AssertValid();
}

void Cweek5View::Dump(CDumpContext& dc) const
{
	CView::Dump(dc);
}

Cweek5Doc* Cweek5View::GetDocument() const // 디버그되지 않은 버전은 인라인으로 지정됩니다.
{
	ASSERT(m_pDocument->IsKindOf(RUNTIME_CLASS(Cweek5Doc)));
	return (Cweek5Doc*)m_pDocument;
}
#endif //_DEBUG


// Cweek5View 메시지 처리기

void Cweek5View::OnInitialUpdate()
{
	CView::OnInitialUpdate();

	CRect rect;
	GetClientRect(&rect);
	dx12Renderer.Initialize(GetSafeHwnd(), rect.Width(), rect.Height());

	// 버튼 생성 (나무 심기 / 저장 / 불러오기)
	btnAddTree.Create(L"나무 심기", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		CRect(10, 10, 110, 40), this, IDC_BTN_ADD_TREE);
	btnSave.Create(L"저장", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		CRect(120, 10, 200, 40), this, IDC_BTN_SAVE);
	btnLoad.Create(L"불러오기", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		CRect(210, 10, 310, 40), this, IDC_BTN_LOAD);
	btnDayNight.Create(L"낮/밤 전환", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		CRect(320, 10, 440, 40), this, IDC_BTN_TOGGLE_DAYNIGHT);

	// ~16ms 주기 타이머 → WASD 키 입력 시에도 연속 렌더링
	SetTimer(1, 16, nullptr);
}

void Cweek5View::OnDestroy()
{
	KillTimer(1);
	dx12Renderer.Cleanup();
	CView::OnDestroy();
}

void Cweek5View::OnTimer(UINT_PTR nIDEvent)
{
	Invalidate(FALSE); // 매 틱마다 재렌더링 → WASD 이동 반영
}

void Cweek5View::OnLButtonDown(UINT nFlags, CPoint point)
{
	SetCapture();
	dx12Renderer.OnMouseDown(nFlags, point.x, point.y);
}
void Cweek5View::OnLButtonUp(UINT nFlags, CPoint point)
{
	ReleaseCapture();
	dx12Renderer.OnMouseUp(nFlags, point.x, point.y);
}
void Cweek5View::OnRButtonDown(UINT nFlags, CPoint point)
{
	SetCapture();
	dx12Renderer.OnMouseDown(nFlags, point.x, point.y);
}

void Cweek5View::OnMouseMove(UINT nFlags, CPoint point)
{
	dx12Renderer.OnMouseMove(nFlags, point.x, point.y);
	Invalidate(FALSE); // 다음 프레임 렌더 요청
}

// 
void Cweek5View::OnBtnAddTree()
{
	dx12Renderer.AddTree();
	Invalidate(FALSE);
}

void Cweek5View::OnBtnSave()
{
	CFileDialog dlg(FALSE,           // FALSE = 저장 대화상자
		L"tree",                     // 기본 확장자
		L"trees.tree",               // 기본 파일명
		OFN_OVERWRITEPROMPT,
		L"Tree Files (*.tree)|*.tree|All Files (*.*)|*.*||");
	if (dlg.DoModal() == IDOK)
	{
		dx12Renderer.SaveTrees(dlg.GetPathName());
	}
}

void Cweek5View::OnBtnLoad()
{
	CFileDialog dlg(TRUE,            // TRUE = 열기 대화상자
		L"tree",
		nullptr,
		OFN_FILEMUSTEXIST,
		L"Tree Files (*.tree)|*.tree|All Files (*.*)|*.*||");
	if (dlg.DoModal() == IDOK)
	{
		dx12Renderer.LoadTrees(dlg.GetPathName());
		Invalidate(FALSE);
	}
}

// 낮/밤 전환 — 1초간 부드럽게 보간된다
void Cweek5View::OnBtnDayNight()
{
	dx12Renderer.ToggleDayNight();
	Invalidate(FALSE);
}