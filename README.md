# MyStocks

`MyStocks` 是一个基于 Qt6 的桌面看盘工具，适合在 macOS 和 Windows 上常驻运行。它以悬浮窗的方式展示自选股、指数、板块和期货行情，强调低打扰、可配置和快速查看。

## 核心能力

- 实时悬浮窗行情展示，支持自选股、指数、板块、期货混合监控
- 托盘常驻与全局快捷键，一键显示或隐藏窗口
- 列显示、列顺序、列宽、字体、颜色、透明度等界面自定义
- 支持鼠标穿透、悬停阅读、分时图弹窗、市场宽度信息等增强能力
- 可切换不同行情源，并支持自定义 `User-Agent`、代理和日志配置

## 项目结构

- `include/`：核心头文件，包含控制器、数据模型、窗口与公共类型定义
- `src/`：应用实现代码
- `src/watchlist_utils.cpp`：自选项与代码归一化相关公共逻辑
- `docs/`：需求说明、接口参考和应用介绍
- `assets/`：应用图标等静态资源

## 快速开始

### macOS

```bash
brew install cmake qt
cmake -S . -B build
cmake --build build
```

项目会优先尝试从 Homebrew 的 Qt 路径自动查找 Qt6。

### 使用 CMake 预设

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
```

发布构建：

```bash
cmake --preset dev-release
cmake --build --preset dev-release
```
