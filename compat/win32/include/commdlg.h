#pragma once
// ============================================================================
// ConcordRTS Win32 兼容层 · commdlg.h（S1）
// 只提供类型 / 常量 / 声明；实现见 S5（窗口族）——macOS 侧将以文件选择面板承接。
// 守卫见 windows.h 顶部说明：本目录只在非 Windows 生效。
// ============================================================================

#if defined(_WIN32)
#error "compat/win32/include/commdlg.h is the non-Windows shim; it must never be used on Windows builds."
#endif

#include "windows.h"

// ---------------------------------------------------------------------------
// 打开 / 保存文件名对话框
// ---------------------------------------------------------------------------
struct OPENFILENAMEW {
    DWORD lStructSize;
    HWND hwndOwner;
    HINSTANCE hInstance;
    LPCWSTR lpstrFilter;
    LPWSTR lpstrCustomFilter;
    DWORD nMaxCustFilter;
    DWORD nFilterIndex;
    LPWSTR lpstrFile;
    DWORD nMaxFile;
    LPWSTR lpstrFileTitle;
    DWORD nMaxFileTitle;
    LPCWSTR lpstrInitialDir;
    LPCWSTR lpstrTitle;
    DWORD Flags;
    WORD nFileOffset;
    WORD nFileExtension;
    LPCWSTR lpstrDefExt;
    LPARAM lCustData;
    UINT (*lpfnHook)(HWND, UINT, WPARAM, LPARAM);
    LPCWSTR lpTemplateName;
    void* pvReserved;
    DWORD dwReserved;
    DWORD FlagsEx;
};
using LPOPENFILENAMEW = OPENFILENAMEW*;

#define OFN_READONLY 0x00000001
#define OFN_OVERWRITEPROMPT 0x00000002
#define OFN_HIDEREADONLY 0x00000004
#define OFN_NOCHANGEDIR 0x00000008
#define OFN_SHOWHELP 0x00000010
#define OFN_ENABLEHOOK 0x00000020
#define OFN_ENABLETEMPLATE 0x00000040
#define OFN_ENABLETEMPLATEHANDLE 0x00000080
#define OFN_NOVALIDATE 0x00000100
#define OFN_ALLOWMULTISELECT 0x00000200
#define OFN_EXTENSIONDIFFERENT 0x00000400
#define OFN_PATHMUSTEXIST 0x00000800
#define OFN_FILEMUSTEXIST 0x00001000
#define OFN_CREATEPROMPT 0x00002000
#define OFN_SHAREAWARE 0x00004000
#define OFN_NOREADONLYRETURN 0x00008000
#define OFN_NOTESTFILECREATE 0x00010000
#define OFN_NONETWORKBUTTON 0x00020000
#define OFN_NOLONGNAMES 0x00040000
#define OFN_EXPLORER 0x00080000
#define OFN_NODEREFERENCELINKS 0x00100000
#define OFN_LONGNAMES 0x00200000
#define OFN_ENABLEINCLUDENOTIFY 0x00400000
#define OFN_ENABLESIZING 0x00800000
#define OFN_DONTADDTORECENT 0x02000000
#define OFN_FORCESHOWHIDDEN 0x10000000

extern "C" {

BOOL GetOpenFileNameW(OPENFILENAMEW* lpofn);
BOOL GetSaveFileNameW(OPENFILENAMEW* lpofn);
short GetFileTitleW(LPCWSTR lpszFile, LPWSTR lpszTitle, WORD cbBuf);

} // extern "C"
