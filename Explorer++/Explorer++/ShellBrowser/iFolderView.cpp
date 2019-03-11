// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include <string>
#include "IShellView.h"
#include "iShellBrowser_internal.h"
#include "SortModes.h"
#include "ViewModes.h"
#include "../Helper/Helper.h"
#include "../Helper/ShellHelper.h"
#include "../Helper/Controls.h"
#include "../Helper/RegistrySettings.h"
#include "../Helper/ListViewHelper.h"
#include "../Helper/Macros.h"

int CShellBrowser::listViewParentSubclassIdCounter = 0;

/* IUnknown interface members. */
HRESULT __stdcall CShellBrowser::QueryInterface(REFIID iid, void **ppvObject)
{
	*ppvObject = NULL;

	if(iid == IID_IUnknown)
	{
		*ppvObject = this;
	}

	if(*ppvObject)
	{
		AddRef();
		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG __stdcall CShellBrowser::AddRef(void)
{
	return ++m_iRefCount;
}

ULONG __stdcall CShellBrowser::Release(void)
{
	m_iRefCount--;
	
	if(m_iRefCount == 0)
	{
		delete this;
		return 0;
	}

	return m_iRefCount;
}

CShellBrowser *CShellBrowser::CreateNew(HWND hOwner,HWND hListView,
	const InitialSettings_t *pSettings)
{
	return new CShellBrowser(hOwner,hListView,pSettings);
}

CShellBrowser::CShellBrowser(HWND hOwner,HWND hListView,
const InitialSettings_t *pSettings) :
m_hOwner(hOwner),
m_hListView(hListView),
m_itemIDCounter(0),
m_columnThreadPool(1),
m_columnResultIDCounter(0),
m_itemImageThreadPool(1),
m_thumbnailResultIDCounter(0),
m_iconResultIDCounter(0),
m_shChangeNotifyId(0)
{
	m_iRefCount = 1;

	InitializeDragDropHelpers();

	m_SortMode				= FSM_NAME;
	m_ViewMode				= VM_ICONS;
	m_bSortAscending		= TRUE;
	m_bAutoArrange			= TRUE;
	m_bShowInGroups			= FALSE;
	m_bShowFriendlyDates	= TRUE;
	m_bFolderVisited		= FALSE;
	m_bApplyFilter			= FALSE;
	m_bFilterCaseSensitive	= FALSE;
	m_bGridlinesActive		= TRUE;
	m_bShowHidden			= FALSE;
	m_bShowExtensions		= TRUE;
	m_bHideSystemFiles		= FALSE;
	m_bHideLinkExtension	= FALSE;

	m_bColumnsPlaced		= FALSE;
	m_bOverFolder			= FALSE;
	m_bDragging				= FALSE;
	m_bVirtualFolder		= FALSE;
	m_bThumbnailsSetup		= FALSE;
	m_nCurrentColumns		= 0;
	m_pActiveColumnList		= NULL;
	m_bPerformingDrag		= FALSE;
	m_nActiveColumns		= 0;
	m_bNewItemCreated		= FALSE;
	m_iDropped				= -1;

	m_iUniqueFolderIndex	= 0;

	m_pidlDirectory			= NULL;

	m_PreviousSortColumnExists = false;

	SetUserOptions(pSettings);

	NListView::ListView_SetAutoArrange(m_hListView,m_bAutoArrange);
	NListView::ListView_SetGridlines(m_hListView,m_bGridlinesActive);

	m_nAwaitingAdd = 0;

	InitializeCriticalSection(&m_csDirectoryAltered);

	m_iFolderIcon = GetDefaultFolderIconIndex();
	m_iFileIcon = GetDefaultFileIconIndex();

	m_ListViewSubclassed = SetWindowSubclass(hListView, ListViewProcStub, LISTVIEW_SUBCLASS_ID, reinterpret_cast<DWORD_PTR>(this));

	HWND hParent = GetParent(hListView);

	if (hParent != NULL)
	{
		m_listViewParentSubclassId = listViewParentSubclassIdCounter++;
		m_ListViewParentSubclassed = SetWindowSubclass(hParent, ListViewParentProcStub, m_listViewParentSubclassId, reinterpret_cast<DWORD_PTR>(this));
	}
	else
	{
		m_ListViewParentSubclassed = FALSE;
	}

	m_itemImageThreadPool.push([](int id) {
		UNREFERENCED_PARAMETER(id);

		CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	});

	m_ListViewSubclassed = SetWindowSubclass(hListView, ListViewProcStub, LISTVIEW_SUBCLASS_ID, reinterpret_cast<DWORD_PTR>(this));
}

CShellBrowser::~CShellBrowser()
{
	StopDirectoryMonitoring();

	if (m_ListViewSubclassed)
	{
		RemoveWindowSubclass(m_hListView, ListViewProcStub, LISTVIEW_SUBCLASS_ID);
	}

	HWND hParent = GetParent(m_hListView);

	if (m_ListViewParentSubclassed && hParent != NULL)
	{
		RemoveWindowSubclass(hParent, ListViewParentProcStub, m_listViewParentSubclassId);
	}

	if (m_ListViewSubclassed)
	{
		RemoveWindowSubclass(m_hListView, ListViewProcStub, LISTVIEW_SUBCLASS_ID);
	}

	m_columnThreadPool.clear_queue();
	m_itemImageThreadPool.clear_queue();

	m_itemImageThreadPool.push([](int id) {
		UNREFERENCED_PARAMETER(id);

		CoUninitialize();
	});

	/* Release the drag and drop helpers. */
	m_pDropTargetHelper->Release();
	m_pDragSourceHelper->Release();

	DeleteCriticalSection(&m_csDirectoryAltered);

	int nItems = ListView_GetItemCount(m_hListView);

	for(int i = 0;i < nItems;i++)
	{
		LVITEM lvItem;
		lvItem.mask		= LVIF_PARAM;
		lvItem.iItem	= i;
		lvItem.iSubItem	= 0;

		ListView_GetItem(m_hListView,&lvItem);

		/* Also destroy the thumbnails imagelist... */
	}

	CoTaskMemFree(m_pidlDirectory);
}

BOOL CShellBrowser::GetAutoArrange(void) const
{
	return m_bAutoArrange;
}

BOOL CShellBrowser::ToggleAutoArrange(void)
{
	m_bAutoArrange = !m_bAutoArrange;

	NListView::ListView_SetAutoArrange(m_hListView, m_bAutoArrange);

	return m_bAutoArrange;
}

ViewMode CShellBrowser::GetCurrentViewMode() const
{
	return m_ViewMode;
}

/* This function is only called on 'hard' view changes
(i.e. view changes resulting from user requests). It is
not called when a tab is first set up (in which case
the view mode still needs to be setup), or when entering
a folder. */
void CShellBrowser::SetCurrentViewMode(ViewMode viewMode)
{
	if(viewMode == m_ViewMode)
	{
		return;
	}

	if(m_ViewMode == VM_THUMBNAILS && viewMode != VM_THUMBNAILS)
		RemoveThumbnailsView();

	SetCurrentViewModeInternal(viewMode);

	switch(viewMode)
	{
		case VM_TILES:
			SetTileViewInfo();
			break;
	}
}

/* Explicitly sets the view mode within in the listview.
This function also initializes any items needed to support
the current view mode. This MUST be done within this
function, as when a tab is first opened, the view settings
will need to be initialized. */
void CShellBrowser::SetCurrentViewModeInternal(ViewMode viewMode)
{
	DWORD dwStyle;

	switch(viewMode)
	{
	case VM_EXTRALARGEICONS:
		{
			IImageList *pImageList = NULL;

			SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&pImageList));
			ListView_SetImageList(m_hListView,(HIMAGELIST)pImageList,LVSIL_NORMAL);
			pImageList->Release();
		}
		break;

	case VM_LARGEICONS:
		{
			IImageList *pImageList = NULL;

			SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&pImageList));
			ListView_SetImageList(m_hListView,(HIMAGELIST)pImageList,LVSIL_NORMAL);
			pImageList->Release();
		}
		break;

	/* Do nothing. This will setup the listview by itself. */
	case VM_THUMBNAILS:
		break;

	case VM_TILES:
	case VM_ICONS:
	case VM_SMALLICONS:
	case VM_LIST:
	case VM_DETAILS:
		{
			IImageList *pImageList = NULL;

			SHGetImageList(SHIL_LARGE, IID_PPV_ARGS(&pImageList));
			ListView_SetImageList(m_hListView,(HIMAGELIST)pImageList,LVSIL_NORMAL);
			pImageList->Release();
		}
		break;
	}

	/* Delete all the tile view columns. */
	if(m_ViewMode == VM_TILES && viewMode != VM_TILES)
		DeleteTileViewColumns();

	switch(viewMode)
	{
		case VM_TILES:
			dwStyle = LV_VIEW_TILE;

			InsertTileViewColumns();
			break;

		case VM_EXTRALARGEICONS:
		case VM_LARGEICONS:
		case VM_ICONS:
			dwStyle = LV_VIEW_ICON;
			break;

		case VM_SMALLICONS:
			dwStyle = LV_VIEW_SMALLICON;
			break;

		case VM_LIST:
			dwStyle = LV_VIEW_LIST;
			break;

		case VM_DETAILS:
			dwStyle = LV_VIEW_DETAILS;

			if(!m_bColumnsPlaced)
			{
				PlaceColumns();
				m_bColumnsPlaced = TRUE;
			}
			break;

		case VM_THUMBNAILS:
			dwStyle = LV_VIEW_ICON;

			if(!m_bThumbnailsSetup)
				SetupThumbnailsView();
			break;

		default:
			dwStyle = LV_VIEW_ICON;
			viewMode = VM_ICONS;
			break;
	}

	m_ViewMode = viewMode;

	if (viewMode != VM_DETAILS)
	{
		NListView::ListView_SetGridlines(m_hListView, FALSE);

		m_columnThreadPool.clear_queue();
		m_columnResults.clear();
	}
	else
	{
		NListView::ListView_SetGridlines(m_hListView, m_bGridlinesActive);
	}

	SendMessage(m_hListView,LVM_SETVIEW,dwStyle,0);
}

SortMode CShellBrowser::GetSortMode() const
{
	return m_SortMode;
}

void CShellBrowser::SetSortMode(SortMode sortMode)
{
	m_SortMode	= sortMode;
}

BOOL CShellBrowser::IsGroupViewEnabled(void) const
{
	return m_bShowInGroups;
}

HRESULT CShellBrowser::InitializeDragDropHelpers(void)
{
	HRESULT hr;

	/* Initialize the drag source helper, and use it to initialize the drop target helper. */
	hr = CoCreateInstance(CLSID_DragDropHelper,NULL,CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&m_pDragSourceHelper));

	if(SUCCEEDED(hr))
	{
		hr = m_pDragSourceHelper->QueryInterface(IID_PPV_ARGS(&m_pDropTargetHelper));

		RegisterDragDrop(m_hListView,this);

		/* RegisterDragDrop calls AddRef on initialization. */
		Release();
	}

	return hr;
}

void CShellBrowser::SetUserOptions(const InitialSettings_t *is)
{
	m_bAutoArrange			= is->bAutoArrange;
	m_bGridlinesActive		= is->bGridlinesActive;
	m_bShowHidden			= is->bShowHidden;
	m_bShowInGroups			= is->bShowInGroups;
	m_bSortAscending		= is->bSortAscending;
	m_SortMode				= is->sortMode;
	m_ViewMode				= is->viewMode;
	m_bApplyFilter			= is->bApplyFilter;
	m_bFilterCaseSensitive	= is->bFilterCaseSensitive;
	m_bShowFolderSizes		= is->bShowFolderSizes;
	m_bDisableFolderSizesNetworkRemovable = is->bDisableFolderSizesNetworkRemovable;
	m_bHideSystemFiles		= is->bHideSystemFiles;
	m_bHideLinkExtension	= is->bHideLinkExtension;
	m_bForceSize			= is->bForceSize;
	m_SizeDisplayFormat		= is->sdf;

	StringCchCopy(m_szFilter,SIZEOF_ARRAY(m_szFilter),is->szFilter);

	m_ControlPanelColumnList = *is->pControlPanelColumnList;
	m_MyComputerColumnList = *is->pMyComputerColumnList;
	m_MyNetworkPlacesColumnList = *is->pMyNetworkPlacesColumnList;
	m_NetworkConnectionsColumnList = *is->pNetworkConnectionsColumnList;
	m_PrintersColumnList = *is->pPrintersColumnList;
	m_RealFolderColumnList = *is->pRealFolderColumnList;
	m_RecycleBinColumnList = *is->pRecycleBinColumnList;

	NListView::ListView_SetGridlines(m_hListView,m_bGridlinesActive);
}

void CShellBrowser::SetGlobalSettings(const GlobalSettings_t *gs)
{
	m_bShowExtensions		= gs->bShowExtensions;
	m_bShowFriendlyDates	= gs->bShowFriendlyDates;
	m_bShowFolderSizes		= gs->bShowFolderSizes;
}

int CShellBrowser::GetId(void) const
{
	return m_ID;
}

void CShellBrowser::SetId(int ID)
{
	m_ID = ID;
}

void CShellBrowser::ToggleGridlines(void)
{
	m_bGridlinesActive = !m_bGridlinesActive;

	NListView::ListView_SetGridlines(m_hListView,m_bGridlinesActive);
}

BOOL CShellBrowser::QueryGridlinesActive(void) const
{
	return m_bGridlinesActive;
}

void CShellBrowser::SetResourceModule(HINSTANCE hResourceModule)
{
	m_hResourceModule = hResourceModule;
}

HRESULT CShellBrowser::Refresh()
{
	return BrowseFolder(m_pidlDirectory,SBSP_ABSOLUTE|SBSP_WRITENOHISTORY);
}

void CShellBrowser::SetHideSystemFiles(BOOL bHideSystemFiles)
{
	m_bHideSystemFiles = bHideSystemFiles;
}

void CShellBrowser::SetShowExtensions(BOOL bShowExtensions)
{
	m_bShowExtensions = bShowExtensions;
}

void CShellBrowser::SetHideLinkExtension(BOOL bHideLinkExtension)
{
	m_bHideLinkExtension = bHideLinkExtension;
}

void CShellBrowser::SetShowFolderSizes(BOOL bShowFolderSizes)
{
	m_bShowFolderSizes = bShowFolderSizes;
}

void CShellBrowser::SetDisableFolderSizesNetworkRemovable(BOOL bDisableFolderSizesNetworkRemovable)
{
	m_bDisableFolderSizesNetworkRemovable = bDisableFolderSizesNetworkRemovable;
}

void CShellBrowser::SetShowFriendlyDates(BOOL bShowFriendlyDates)
{
	m_bShowFriendlyDates = bShowFriendlyDates;
}

void CShellBrowser::SetInsertSorted(BOOL bInsertSorted)
{
	m_bInsertSorted = bInsertSorted;
}

void CShellBrowser::SetForceSize(BOOL bForceSize)
{
	m_bForceSize = bForceSize;
}

void CShellBrowser::SetSizeDisplayFormat(SizeDisplayFormat_t sdf)
{
	m_SizeDisplayFormat = sdf;
}

void CShellBrowser::InsertTileViewColumns(void)
{
	LVTILEVIEWINFO lvtvi;
	LVCOLUMN lvColumn;

	/* Name. */
	lvColumn.mask		= LVCF_TEXT;
	lvColumn.pszText	= EMPTY_STRING;
	ListView_InsertColumn(m_hListView,1,&lvColumn);

	/* Type. */
	lvColumn.mask		= LVCF_TEXT;
	lvColumn.pszText	= EMPTY_STRING;
	ListView_InsertColumn(m_hListView,2,&lvColumn);

	/* File size. */
	lvColumn.mask		= LVCF_TEXT;
	lvColumn.pszText	= EMPTY_STRING;
	ListView_InsertColumn(m_hListView,3,&lvColumn);

	lvtvi.cbSize	= sizeof(lvtvi);
	lvtvi.dwMask	= LVTVIM_COLUMNS;
	lvtvi.dwFlags	= LVTVIF_AUTOSIZE;
	lvtvi.cLines	= 2;
	ListView_SetTileViewInfo(m_hListView,&lvtvi);
}

void CShellBrowser::DeleteTileViewColumns(void)
{
	ListView_DeleteColumn(m_hListView,3);
	ListView_DeleteColumn(m_hListView,2);
	ListView_DeleteColumn(m_hListView,1);
}

void CShellBrowser::SetTileViewInfo(void)
{
	LVITEM lvItem;
	BOOL bRes;
	int nItems;
	int i = 0;

	nItems = ListView_GetItemCount(m_hListView);

	for(i = 0;i < nItems;i++)
	{
		lvItem.mask		= LVIF_PARAM;
		lvItem.iItem	= i;
		lvItem.iSubItem	= 0;
		bRes = ListView_GetItem(m_hListView,&lvItem);

		if(bRes)
		{
			SetTileViewItemInfo(i,(int)lvItem.lParam);
		}
	}
}

/* TODO: Make this function configurable. */
void CShellBrowser::SetTileViewItemInfo(int iItem,int iItemInternal)
{
	SHFILEINFO shfi;
	LVTILEINFO lvti;
	UINT uColumns[2] = {1,2};
	int columnFormats[2] = { LVCFMT_LEFT, LVCFMT_LEFT };
	TCHAR FullFileName[MAX_PATH];

	lvti.cbSize		= sizeof(lvti);
	lvti.iItem		= iItem;
	lvti.cColumns	= 2;
	lvti.puColumns	= uColumns;
	lvti.piColFmt	= columnFormats;
	ListView_SetTileInfo(m_hListView,&lvti);

	QueryFullItemName(iItem,FullFileName,SIZEOF_ARRAY(FullFileName));

	SHGetFileInfo(FullFileName,0,
		&shfi,sizeof(SHFILEINFO),SHGFI_TYPENAME);

	ListView_SetItemText(m_hListView,iItem,1,shfi.szTypeName);

	if((m_itemInfoMap.at(iItemInternal).wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) !=
		FILE_ATTRIBUTE_DIRECTORY)
	{
		TCHAR			lpszFileSize[32];
		ULARGE_INTEGER	lFileSize;

		lFileSize.LowPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeLow;
		lFileSize.HighPart = m_itemInfoMap.at(iItemInternal).wfd.nFileSizeHigh;

		FormatSizeString(lFileSize,lpszFileSize,SIZEOF_ARRAY(lpszFileSize),
			m_bForceSize,m_SizeDisplayFormat);

		ListView_SetItemText(m_hListView,iItem,2,lpszFileSize);
	}
}