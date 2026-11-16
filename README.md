# 截贴 · JieTie

截图之后，直接按 `Ctrl+V` 粘到文件夹里，就是一个 PNG 文件。

---

## 这玩意儿解决什么问题

Windows 上截图（`Win+Shift+S`、`PrtSc`、QQ/微信截图）有个老毛病：图片只是进了剪贴板，
你跑到桌面或资源管理器里按 `Ctrl+V`，什么都粘不出来——因为它不是一个"文件"，Explorer
只认文件。

它常驻托盘盯着剪贴板。一旦发现里面是图片，就把这张图**补成一个真正的文件**：剪贴板里
同时存在 `CF_HDROP`（文件）、`CF_DIB` + `CF_BITMAP`（位图）和 `PNG`（原始数据）三种格式。
于是——

- 在文件夹 / 桌面按 `Ctrl+V`：生成 PNG 文件
- 粘进 Word、PPT、画图：图片照常
- 粘进聊天框：多数软件读位图，还是图片（QQ/微信会优先当"文件"发，见文末"已知的毛病"）

单文件 exe，约 1.1 MB，静态链接，不依赖 VC 运行库，常驻内存 10 MB 左右。

## 三种模式

托盘右键切换，立即生效，也会写回配置文件：

| 模式 | 行为 | 适合 |
| --- | --- | --- |
| 变成文件，`Ctrl+V` 粘贴（默认，`clip`） | 图片转成临时文件挂进剪贴板 | 截完随手粘到某个文件夹 |
| 自动保存到文件夹（`save`） | 每张图直接存进指定目录，剪贴板不动 | 攒素材、留证据 |
| 两种都做（`both`） | 既存档，也能粘贴 | 都要 |

`clip` 模式下文件先落在 `%TEMP%\JieTie\`，粘贴时是**复制**过去的，所以暂存件留着也没关系；
超过 7 天的暂存文件会在下次启动时自动清掉。

## 上手

1. 双击 `bin\JieTie.exe`，右下角托盘出现一个蓝色小相机。
2. 想截什么就截什么。
3. 切到桌面或目标文件夹，`Ctrl+V`。

双击托盘图标打开保存文件夹；右键是菜单：三种模式、改保存文件夹、暂停、开机自启、退出。

暂停功能不常用但有用——有时候你只是想把图片复制过去，不希望它落盘，点一下"暂停"就行。

## 目录结构

```
截贴/
├─ src/JieTie.cpp        全部源码，单文件
├─ bin/                  编译产物（已 gitignore），运行时 JieTie.ini 也在这
├─ docs/使用说明.txt      给不想看 Markdown 的人准备的简版说明
├─ build.bat             一键编译
├─ README.md
├─ LICENSE
└─ .gitignore / .gitattributes
```

## 配置

首次运行会在 exe 旁边生成 `JieTie.ini`，记事本就能改：

```ini
[Settings]
Mode=clip           ; clip / save / both
Folder=             ; save/both 的保存目录，留空 = 桌面\Screenshots
Prefix=screenshot   ; 文件名前缀，可以改成"截图"
MinWidth=0          ; 小于这个宽度不处理（0 = 不限制）
MinHeight=0         ; 小于这个高度不处理
Notify=0            ; 1 = 每次处理完弹通知
Paused=0            ; 1 = 启动时暂停
```

文件名形如 `screenshot_20261116_113401_857.png`，同一毫秒撞名会自动加 `_2`、`_3`。

## 从源码编译

只依赖 MinGW-w64 的 g++（>= 8）和系统自带的 GDI+，没有第三方库：

```bat
build.bat
```

或者手动来（输出到 `bin\`）：

```bash
mkdir -p bin
g++ -municode -mwindows -std=c++17 -O2 -static -static-libgcc -static-libstdc++ \
    -finput-charset=UTF-8 src/JieTie.cpp -o bin/JieTie.exe \
    -lgdiplus -lshlwapi -lshell32 -lole32 -luuid -ladvapi32 -lgdi32
```

源码就一个 `src/JieTie.cpp`，纯 Win32 + GDI+。

## 里面踩过的坑（想改代码的话值得看一眼）

**1. 别在收到 `WM_CLIPBOARDUPDATE` 时立刻去读剪贴板。**
WinForms、Office 这类程序用的是 OLE 延迟渲染（delayed rendering）——按微软文档的说法，
数据不是马上放进剪贴板的，而是等别人来取的时候由源程序现场"渲染"。你要是立刻
`OpenClipboard` + `GetClipboardData`，就和源程序正在进行的写入撞上了：轻则读到空数据，
重则在对方那边抛出"剪贴板操作失败"。截贴的做法是先去抖 120 ms，再交给工作线程多等 150 ms，
用 `GetClipboardSequenceNumber()` 校验内容有没有变，最多重试 6 轮。UI 线程全程不碰剪贴板。

**2. 位图得用"自下而上"的 DIB。**
我一开始图省事用了负的 `biHeight`（top-down），自己读没问题，换个进程读 `CF_BITMAP`
就永远拿不到。改成正的 `biHeight`，并且 `CF_BITMAP` 用 `CreateCompatibleBitmap` + `SetDIBits`
生成 DDB 才算稳（`CreateDIBSection` 出来的句柄跨进程传递会失败，这个坑我踩了两次）。

**3. 自己改剪贴板会触发自己的监听。**
重建完剪贴板，系统还会再发一次 `WM_CLIPBOARDUPDATE`，不管它就会自己喂自己、无限循环。
记一个 `g_ownSeq` 把这次更新跳过就行。

**4. 别拿 .NET 的 `Clipboard.GetImage()` 当验收标准。**
它对原生 `SetClipboardData` 放进去的图经常返回 null，但原生 `GetClipboardData(CF_DIB)`
其实是好的。我最后写了个小探针程序逐个格式去读，才确认四路格式（HDROP / DIB / BITMAP / PNG）
全部正常。

## 已知的毛病

- 默认模式下把截图粘进 QQ/微信，多半会变成"发送文件"而不是内联图片，因为剪贴板里带了文件。
  介意的话把模式切成"自动保存到文件夹"。
- 只管图片。复制的文字、文件不受影响。
- 需要 Windows 7 及以上（依赖 `AddClipboardFormatListener`）。
- 某些杀毒软件会对"监听剪贴板"的行为提示一下，放行即可。
- 暂存文件 7 天清理，所以翻剪贴板历史（`Win+V`）去粘很久以前那张图时，可能会提示文件不存在。

## License

MIT，详见 [LICENSE](LICENSE)。
