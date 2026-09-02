# tslt

`tslt` is a minimal native Windows text translation tool built with Win32 API, CMake, and WinHTTP. It reads OpenAI-compatible provider settings from `tslt.ini`, keeps window state in `%LOCALAPPDATA%`, and runs translation requests on a worker thread so the UI stays responsive.

## Current scope

- Native Win32 desktop UI
- OpenAI-compatible chat-completions backends
- Configurable provider selection from `tslt.ini`
- Target language selection, automatically set to English for predominantly Chinese input and Simplified Chinese for all other predominantly textual input
- Copy translated output to clipboard
- Separate runtime state file at `%LOCALAPPDATA%/tslt/state.dat`
- Optional piped stdin input preloaded into the input box

## Build

This repository now defaults to Microsoft toolchains through CMake presets.

### Local build

```bash
cmake --preset default
cmake --build --preset default
```

Output:

- `build/vs2026-x64/Release/tslt.exe`

## Configuration

Copy `tslt.ini.example` to `tslt.ini` next to the executable, or place it under `%AppData%/tslt/tslt.ini`.

Example:

```ini
[LLM]
provider = Qwen

[LLM.Qwen]
name = SiliconFlow Qwen
base_url = https://api.siliconflow.cn/v1
api_key = sk-XXXXX
model = Qwen/Qwen3.5-4B

[LLM.DSK]
base_url = https://api.deepseek.com
api_key = sk-xxxxxx
model = deepseek-v4-flash

[Translate]
target_language = zh-CN
source_language = auto
temperature = 0.2
```

Rules:

- The active provider is selected by `[LLM].provider`.
- Every provider is loaded from an `LLM.<name>` section.
- `name` is optional and only affects the UI label.
- A single non-whitespace word of no more than 20 characters selects the configured dictionary; phrases, sentences, and longer unbroken input select the default LLM. Set `[Dictionary] auto_select_max_characters` to adjust the limit.
- Runtime window state is never written into `tslt.ini`.

## Release

A GitHub Actions workflow at `.github/workflows/release.yml` creates a Windows release zip when you push a semantic version tag. Both `0.0.2` and `v0.0.2` will trigger the workflow.

Examples:

```bash
git tag 0.0.2
git push origin 0.0.2
```

```bash
git tag v0.0.2
git push origin v0.0.2
```

The uploaded asset is always named without the `v` prefix:

- `tslt-0.0.2-windows-x64.zip`

## License

MIT. See `LICENSE`.
