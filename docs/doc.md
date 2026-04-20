# MyStocks

一个桌面端用于实时查看股价的软件

## 功能

1. 需要支持有悬浮窗，悬浮窗是表格，可以自定义列宽
1. 悬浮窗实时查看股票代码、股票名称、股价、涨跌幅、涨价价格等，悬浮窗透明度、背景、字体颜色支持自定义，默认情况下上涨红色文字，下跌绿色，0%白色
1. 需要支持快捷键显示隐藏悬浮窗，鼠标可拖动
1. 打开程序后会显示托盘,可以控制显示隐藏悬浮窗，也可以打开设置页面设置
1. 设置页面的通用设置页面可以设置：最小轮询股票时间，悬浮窗透明度，悬浮窗隐藏显示快捷键，API数据源
1. 设置页面的显示页面可以勾选设置悬浮窗列表的抬头显示开关、显示列等，自定义悬浮窗背景色、透明度、涨跌时的文本颜色

## 开发要求

1. 项目基于Qt开发，兼容最低Windows10  MacOS
1. API接口：XTick接口文档：<http://xtick.top/doc/xtick.md> ，后续可能加入不同的API，需要考虑切换不同API的兼容问题
1. 参数配置参考data.yaml

## 本地环境配置（macOS）

1. 安装 CMake 与 Qt6：

```bash
brew install cmake qt
```

1. 检查安装结果：

```bash
cmake --version
brew list --versions qt
```

1. 配置并构建：

```bash
cmake -S . -B build
cmake --build build -j
```

说明：项目在 macOS 下会自动尝试从 Homebrew 目录查找 Qt6（`/opt/homebrew/opt/qt` 或 `/usr/local/opt/qt`），通常不需要手动设置 `CMAKE_PREFIX_PATH`。

## CMake 预设用法

项目已提供 `CMakePresets.json`，可以直接使用以下命令：

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
```

发布构建：

```bash
cmake --preset dev-release
cmake --build --preset dev-release
```

## VS Code 一键任务

已提供任务文件：`.vscode/tasks.json`。

可在 VS Code 中通过“运行任务”直接选择：

1. `CMake: Configure (dev-debug)`
1. `CMake: Build (dev-debug)`
1. `Run: MyStocks (dev-debug)`
1. `One Click: Configure + Build + Run`

其中 `One Click: Configure + Build + Run` 会按顺序执行配置、构建并启动程序。

## VS Code 调试（launch.json）

已提供调试配置文件：`.vscode/launch.json`。

可在“运行和调试”中选择：

1. `MyStocks: Debug (dev-debug)`
1. `MyStocks: Debug (dev-release)`

两者都会在启动前自动执行对应的预构建任务：

1. `CMake: Prepare (dev-debug)`
1. `CMake: Prepare (dev-release)`
