; LiveCord 安装器：安装时让用户选「完整版」还是「轻量版」。
;   完整版 = 自带内核的 Electron 版：这里不带它，选了就从 GitHub 把 LiveCord-Setup.exe 下载下来运行
;   轻量版 = WebView2 外壳（lc-lite.cpp）+ 两个助手，就打在这个安装器里，约 1 MB
; 用 make-installer.js 构建（它负责找 makensis 和 INetC 插件，传 OUT / INETC_DIR / VERSION 进来）
Unicode true
!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!addplugindir "${INETC_DIR}"

!define FULL_URL "https://github.com/Felis-desuwa/livecord-desktop/releases/latest/download/LiveCord-Setup.exe"
!define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\LiveCordLite"
!define LITE_NAME "LiveCord 轻量版"

Name "LiveCord"
OutFile "${OUT}"
RequestExecutionLevel user
SetCompressor /SOLID lzma
InstallDir "$LOCALAPPDATA\Programs\LiveCord Lite"
BrandingText "LiveCord ${VERSION}"
ShowInstDetails nevershow
AutoCloseWindow true
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "LiveCord"
VIAddVersionKey "FileDescription" "LiveCord 安装器"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "LiveCord"

!define MUI_ICON "..\build\icon.ico"
!define MUI_UNICON "..\build\icon.ico"
!define MUI_ABORTWARNING

Var Choice     ; "full" 或 "lite"
Var RadioFull
Var RadioLite

Page custom ChoosePage ChooseLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
  StrCpy $Choice "full"
  ; 静默安装（/S）没法问：装轻量版（它就在这个安装器里，不用联网）
  IfSilent 0 +2
    StrCpy $Choice "lite"
FunctionEnd

Function ChoosePage
  !insertmacro MUI_HEADER_TEXT "选择要安装的版本" "两个版本功能相同、账号通用，可以随时换装另一个"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateRadioButton} 0 0 100% 12u "完整版（推荐）"
  Pop $RadioFull
  ${NSD_CreateLabel} 12u 13u 97% 20u "自带浏览器内核，表现最稳定，直播的各项数据都是在它上面实测的。$\r$\n需要再下载约 100 MB，装好后占约 280 MB。"
  Pop $0

  ${NSD_CreateRadioButton} 0 40u 100% 12u "轻量版（试验）"
  Pop $RadioLite
  ${NSD_CreateLabel} 12u 53u 97% 18u "用 Windows 自带的 WebView2 内核，不用再下载，装好后只占约 1 MB。$\r$\n请先看清楚风险："
  Pop $0
  ${NSD_CreateLabel} 12u 73u 97% 66u "· 内核由系统自动更新，不受软件控制。系统更新后高帧率直播可能出问题，$\r$\n   到时要等软件更新，或者改装完整版$\r$\n· 新观众进来时偶尔会多卡一两秒；自己的预览画面较小、每秒 20 帧$\r$\n· 内存和 CPU 占用不比完整版少，省的只是下载量和磁盘$\r$\n· Windows 10 上可能要先装微软的 WebView2 运行时（Windows 11 自带）"
  Pop $0

  ${If} $Choice == "lite"
    ${NSD_Check} $RadioLite
  ${Else}
    ${NSD_Check} $RadioFull
  ${EndIf}
  ; 最后一页之前的按钮写成"安装"
  GetDlgItem $0 $HWNDPARENT 1
  SendMessage $0 ${WM_SETTEXT} 0 "STR:安装(&I)"
  nsDialogs::Show
FunctionEnd

Function ChooseLeave
  ${NSD_GetState} $RadioLite $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $Choice "lite"
    Return
  ${EndIf}
  StrCpy $Choice "full"
  ; 完整版：下载它自己的安装包并运行，然后本安装器退出
  InitPluginsDir
  inetc::get /CAPTION "正在下载 LiveCord 完整版" /POPUP "" /TRANSLATE "正在下载 %s" "正在连接…" "秒" "分钟" "小时" "" "%d kB（%d%%）/ %d kB，%d.%01d kB/s" "（还要 %d %s%s）" "${FULL_URL}" "$PLUGINSDIR\LiveCord-Setup.exe" /END
  Pop $0
  ${If} $0 != "OK"
    MessageBox MB_ICONEXCLAMATION|MB_OK "完整版没有下载成功（$0）。$\r$\n$\r$\n可以检查网络后再试一次，或者先装轻量版。"
    Abort ; 留在这一页
  ${EndIf}
  HideWindow
  ExecWait '"$PLUGINSDIR\LiveCord-Setup.exe"'
  Quit
FunctionEnd

Section "LiveCord Lite"
  ; 正在运行的话先关掉（文件被占着换不了）
  nsExec::Exec 'taskkill /IM LiveCordLite.exe /F'
  Pop $0
  Sleep 400
  SetOutPath "$INSTDIR"
  File "out\LiveCordLite.exe"
  SetOutPath "$INSTDIR\bin"
  File "..\bin\lc-audio.exe"
  File "..\bin\lc-capture.exe"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\${LITE_NAME}.lnk" "$INSTDIR\LiveCordLite.exe"
  CreateShortcut "$SMPROGRAMS\${LITE_NAME}.lnk" "$INSTDIR\LiveCordLite.exe"
  WriteRegStr HKCU "${UNKEY}" "DisplayName" "${LITE_NAME}"
  WriteRegStr HKCU "${UNKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNKEY}" "Publisher" "LiveCord"
  WriteRegStr HKCU "${UNKEY}" "DisplayIcon" "$INSTDIR\LiveCordLite.exe"
  WriteRegStr HKCU "${UNKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNKEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNKEY}" "EstimatedSize" 2048
SectionEnd

Function .onInstSuccess
  ; 装完直接打开；静默安装时加 /NORUN 可以不打开
  ClearErrors
  ${GetParameters} $0
  ${GetOptions} $0 "/NORUN" $1
  IfErrors 0 +2
    Exec '"$INSTDIR\LiveCordLite.exe"'
FunctionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM LiveCordLite.exe /F'
  Pop $0
  Sleep 400
  Delete "$INSTDIR\LiveCordLite.exe"
  Delete "$INSTDIR\bin\lc-audio.exe"
  Delete "$INSTDIR\bin\lc-capture.exe"
  RMDir "$INSTDIR\bin"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$DESKTOP\${LITE_NAME}.lnk"
  Delete "$SMPROGRAMS\${LITE_NAME}.lnk"
  DeleteRegKey HKCU "${UNKEY}"
  ; 登录状态和设置在 %LOCALAPPDATA%\LiveCord Lite，留着：重装后不用重新登录
SectionEnd
