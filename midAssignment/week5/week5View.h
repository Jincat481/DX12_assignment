
// week5View.h: Cweek5View 클래스의 인터페이스
//

#pragma once
#include "Dx12Renderer.h"


class Cweek5View : public CView
{
protected: // serialization에서만 만들어집니다.
	Cweek5View() noexcept;
	DECLARE_DYNCREATE(Cweek5View)

// 특성입니다.
public:
	Cweek5Doc* GetDocument() const;

// 작업입니다.
public:

// 재정의입니다.
public:
	virtual void OnDraw(CDC* pDC);  // 이 뷰를 그리기 위해 재정의되었습니다.
	virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
	virtual void OnInitialUpdate() override;
	afx_msg void OnDestroy();
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnRButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnBtnAddTree();      // 나무 심기
	afx_msg void OnBtnSave();         // 저장
	afx_msg void OnBtnLoad();         // 불러오기
	afx_msg void OnBtnDayNight();     // 낮/밤 전환
	afx_msg void OnTimer(UINT_PTR nIDEvent); // 연속 렌더링 타이머
protected:
	virtual BOOL OnPreparePrinting(CPrintInfo* pInfo);
	virtual void OnBeginPrinting(CDC* pDC, CPrintInfo* pInfo);
	virtual void OnEndPrinting(CDC* pDC, CPrintInfo* pInfo);

// 구현입니다.
public:
	virtual ~Cweek5View();
#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif

protected:

// 생성된 메시지 맵 함수
protected:
	afx_msg void OnFilePrintPreview();
	afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
	DECLARE_MESSAGE_MAP()

private:
	Dx12Renderer dx12Renderer;
	CButton btnAddTree;   // 나무 심기
	CButton btnSave;      // 저장
	CButton btnLoad;      // 불러오기
	CButton btnDayNight;  // 낮/밤 전환
};

#ifndef _DEBUG  // week5View.cpp의 디버그 버전
inline Cweek5Doc* Cweek5View::GetDocument() const
   { return reinterpret_cast<Cweek5Doc*>(m_pDocument); }
#endif

