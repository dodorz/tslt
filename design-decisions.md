# tslt Win32 第一版设计决策

## 目标

实现一个极简的 Windows LLM 翻译工具，第一版优先保证：

- 原生 Win32 窗口与控件
- 配置简单、可手工维护
- 与 git 同步配置时不产生运行时状态噪音
- 网络请求与 UI 线程严格隔离

## 已确认技术选型

- 语言：C++17 或 C++20
- 窗口/UI：Win32 API
- 网络：WinHTTP
- 配置格式：INI
- 主配置文件：`tslt.ini`
- 运行时状态文件：`state.dat`
- 第一版界面：纯 Win32 控件，不使用 WebView2
- 并发模型：后台线程发起翻译请求，主线程仅处理 UI

## 第一版功能范围

保留：

- 输入文本
- 目标语言选择
- 翻译按钮
- 输出结果
- 复制结果
- 读取 LLM 配置

不做：

- OCR
- 划词翻译
- 托盘
- 全局快捷键
- 流式输出
- 历史记录
- 请求取消

## 配置文件规则

### `tslt.ini`

用途：保存稳定配置，可手工维护，可用 git 备份与同步。

查找顺序：

1. 先查找程序同目录下的 `tslt.ini`
2. 如果不存在，再查找 `%AppData%/<appname>/tslt.ini`
3. 如果两处都不存在，使用内置默认值启动

读取规则：

- 只读取第一处命中的配置文件
- 如果程序目录下存在 `tslt.ini`，则不再继续查找 `%AppData%`
- 保存配置时写回当前生效的配置文件路径
- 如果启动时两处都不存在，则首次保存写入 `%AppData%/<appname>/tslt.ini`

### LLM 配置格式

```ini
[LLM]
provider = Qwen

[LLM.Qwen]
base_url = https://api.siliconflow.cn/v1
api_key = sk-XXXXX
model = Qwen/Qwen3.5-4B

[LLM.DSK]
base_url = https://api.deepseek.com
api_key = sk-xxxxxx
model = deepseek-v4-flash
```

语义：

- `[LLM]` 中的 `provider` 表示当前激活的供应商名
- 程序运行时拼出节名 `LLM.<provider>`
- 从对应节中读取：`base_url`、`api_key`、`model`
- `provider` 按原样使用，不做大小写归一化

附加配置建议保留在 `tslt.ini` 中：

```ini
[Translate]
target_language = zh-CN
source_language = auto
temperature = 0.2
```

## 运行时状态文件规则

### `state.dat`

用途：保存运行时状态，不参与 git 同步配置。

位置：

- 仅存放于 `%LOCALAPPDATA%/<appname>/state.dat`

说明：

- 文件扩展名为 `.dat`
- 文件内容仍使用 INI 结构，便于使用系统 API 读写
- 提示词历史保存在 `[Prompts]` 区段，`item0` 为最近使用的提示词，最多保留 20 条
- 提示词中的反斜杠保存为 `\\`，换行保存为 `\\n`
- 不在程序目录下保存 `state.dat`

建议内容：

```ini
[Window]
x = 120
y = 90
width = 960
height = 720
maximized = 0

[Prompts]
count = 2
item0 = You are a translation engine. Return only the translated text.
item1 = Translate the text accurately and preserve its formatting.
```

边界：

- `state.dat` 不存在时，窗口使用默认尺寸并居中
- 若记录的位置已超出可见屏幕范围，则忽略保存的 `x/y` 并重新居中
- `WM_MOVE` / `WM_SIZE` 只更新内存中的状态，不直接高频写文件
- 关闭窗口时再保存 `state.dat`

## 配置与状态分离原则

- `tslt.ini` 只保存稳定、希望被同步和手工维护的配置
- `state.dat` 只保存运行中的 UI 状态
- 窗口位置与尺寸不写入 `tslt.ini`

## 窗口与线程模型

### 基本原则

- 系统消息只处理窗口生命周期、布局和控件命令
- 自定义消息只处理后台线程投递的结果
- 网络线程绝不直接操作 UI
- 后台线程只通过 `PostMessage` 向主窗口回传结果

### 控件 ID

```cpp
#define IDC_INPUT            1001
#define IDC_TARGET_LANG      1002
#define IDC_TRANSLATE        1003
#define IDC_OUTPUT           1004
#define IDC_COPY             1005
#define IDC_STATUS           1006
```

### 自定义消息

```cpp
constexpr UINT WM_APP_TRANSLATE_DONE  = WM_APP + 1;
constexpr UINT WM_APP_TRANSLATE_ERROR = WM_APP + 2;
```

语义：

- `WM_APP_TRANSLATE_DONE`：后台线程成功完成翻译
- `WM_APP_TRANSLATE_ERROR`：后台线程请求失败或解析失败

### 系统消息分工

- `WM_CREATE`
  - 创建子控件
  - 加载配置与状态
  - 初始化下拉框和状态栏

- `WM_SIZE`
  - 重新布局控件
  - 更新内存中的窗口宽高
  - 不直接写 `state.dat`

- `WM_MOVE`
  - 更新内存中的窗口位置
  - 仅在窗口处于 normal 状态时记录

- `WM_COMMAND`
  - 处理按钮点击与下拉框选择变化

- `WM_CLOSE`
  - 保存 `state.dat`
  - 销毁窗口

- `WM_DESTROY`
  - `PostQuitMessage(0)`

- `WM_GETMINMAXINFO`
  - 设置最小窗口尺寸

## `WM_COMMAND` 第一版交互

- `IDC_TRANSLATE + BN_CLICKED`
  - 读取输入文本与当前配置
  - 校验配置完整性
  - 禁用翻译按钮
  - 启动后台翻译线程

- `IDC_COPY + BN_CLICKED`
  - 复制输出结果到剪贴板

- `IDC_TARGET_LANG + CBN_SELCHANGE`
  - 更新当前目标语言

第一版不做：

- 输入框变更即自动翻译
- 请求中的取消
- 流式中间状态更新

## 请求模型

第一版按 OpenAI-compatible 接口实现。

请求地址：

- `base_url + /chat/completions`

请求体大致结构：

```json
{
  "model": "Qwen/Qwen3.5-4B",
  "messages": [
    {
      "role": "system",
      "content": "You are a translation engine. Return only the translated text."
    },
    {
      "role": "user",
      "content": "Translate the following text to zh-CN:\n\n..."
    }
  ],
  "temperature": 0.2
}
```

响应读取：

- 优先从 `choices[0].message.content` 提取翻译文本

## 线程边界

UI 线程负责：

- 读取和更新控件内容
- 改变按钮可用状态
- 改变状态栏文本
- 管理窗口状态

后台线程负责：

- 读取请求快照
- 发起 WinHTTP 请求
- 解析 JSON
- 构造结果对象
- `PostMessage` 回主窗口

约束：

- 后台线程不读取 UI 控件
- 后台线程不保存窗口对象指针
- 只持有 `HWND`
- 若窗口关闭导致 `PostMessage` 失败，线程自行释放结果对象并结束

## 请求与结果结构

建议结构：

```cpp
struct TranslateRequest {
    HWND ownerHwnd;
    std::wstring baseUrl;
    std::wstring apiKey;
    std::wstring model;
    std::wstring sourceLanguage;
    std::wstring targetLanguage;
    double temperature;
    std::wstring inputText;
};

struct TranslateResult {
    std::wstring translatedText;
};

struct TranslateError {
    std::wstring message;
};
```

## 主窗口状态建议

```cpp
struct WindowState {
    int x;
    int y;
    int width;
    int height;
    bool maximized;
};
```

主窗口应至少维护：

- 主窗口 `HWND`
- 各子控件 `HWND`
- `isTranslating_`
- 已加载配置对象
- `state.dat` 路径
- 当前窗口状态

## 第一版交互时序

1. 启动时加载 `tslt.ini`
2. 启动时加载 `%LOCALAPPDATA%/<appname>/state.dat`
3. 创建并显示主窗口
4. 用户点击 Translate
5. UI 线程组装 `TranslateRequest`
6. 启动后台线程
7. 后台线程发起请求并解析结果
8. 通过 `WM_APP_TRANSLATE_DONE` 或 `WM_APP_TRANSLATE_ERROR` 回传主线程
9. 主线程更新输出框与状态栏
10. 关闭窗口时保存 `state.dat`

## 后续细化方向

后续实现前还需要继续细化：

- `MainWindow` 类接口与 `WndProc` 转发实现
- `state.dat` 的恢复与屏幕外位置修正规则
- `WinHTTP` 请求构造与错误处理细节
- `tslt.ini` / `state.dat` 的读写模块接口
