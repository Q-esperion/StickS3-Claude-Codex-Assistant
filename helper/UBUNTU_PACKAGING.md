# Ubuntu 助手打包与文件管理说明

本文档用于规范 `helper` 目录下 Ubuntu 助手相关文件的用途，以及哪些文件需要纳入 Git，哪些仅本地保留。

## 一、目录与文件职责

- `type_server_ubuntu.py`：Ubuntu/Linux 版助手主程序（托盘、粘贴、UDP 发现、HTTP 服务）。
- `StickS3HelperUbuntu.spec`：PyInstaller 打包配置（onefile）。
- `icon_22.png` / `icon_32.png` / `icon_64.png` / `icon.svg`：图标资源（需要入库）。
- `dist/`：打包输出目录（可执行文件、desktop 运行副本等），**本地产物，不入库**。
- `build/`：PyInstaller 中间构建目录，**本地产物，不入库**。
- `config_ubuntu.json`：本机运行配置（窗口绑定、端口等），**本地配置，不入库**。
- `stick_log_ubuntu.txt`：本机运行日志，**本地日志，不入库**。

## 二、Git 同步策略

已在仓库根 `.gitignore` 中配置以下规则，避免把本地/打包产物同步到 Git：

- `helper/dist/`
- `helper/build/`
- `helper/config_ubuntu.json`
- `helper/stick_log_ubuntu.txt`

保留入库（可共享）的 Ubuntu 相关文件：

- `helper/type_server_ubuntu.py`
- `helper/StickS3HelperUbuntu.spec`
- `helper/icon_*.png`
- `helper/icon.svg`

## 三、标准打包流程（Ubuntu）

在 `helper` 目录执行：

```bash
venv/bin/pyinstaller --clean --noconfirm StickS3HelperUbuntu.spec
```

产物默认位于：

- `helper/dist/StickS3HelperUbuntu`

## 四、应用菜单（无需 deb）

推荐用用户级 desktop 方式注册到应用菜单：

- desktop 文件路径：`~/.local/share/applications/sticks3-helper.desktop`
- 图标路径：`~/.local/share/icons/hicolor/64x64/apps/sticks3-helper.png`

这种方式无需 root 权限，也无需 `.deb` 安装包。
