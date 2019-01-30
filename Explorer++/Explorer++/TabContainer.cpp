// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "TabContainer.h"
#include "Explorer++_internal.h"


CTabContainer::CTabContainer(HWND hTabCtrl, std::unordered_map<int, TabInfo_t> *tabInfo, IExplorerplusplus *pexpp) :
m_hTabCtrl(hTabCtrl),
m_tabInfo(tabInfo),
m_pexpp(pexpp)
{

}

CTabContainer::~CTabContainer()
{

}

int CTabContainer::GetSelection()
{
	int Index = TabCtrl_GetCurSel(m_hTabCtrl);
	assert(Index != -1);

	return Index;
}

void CTabContainer::SetSelection(int Index)
{
	assert(Index >= 0);
	m_pexpp->SetTabSelection(Index);
}

CShellBrowser *CTabContainer::GetBrowserForTab(int Index)
{
	TCITEM tcItem;
	tcItem.mask = TCIF_PARAM;
	BOOL res = TabCtrl_GetItem(m_hTabCtrl,Index,&tcItem);
	assert(res);

	if(!res)
	{
		return NULL;
	}

	return m_tabInfo->at(static_cast<int>(tcItem.lParam)).shellBrower;
}