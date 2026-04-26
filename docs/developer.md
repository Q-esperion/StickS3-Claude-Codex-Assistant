# 开发者发布说明

## 本地验证

```powershell
python -m unittest discover -s tests
python -m platformio run -d .
python -m platformio run -d . -e m5stack-sticks3-release
```

开发环境保留 `CORE_DEBUG_LEVEL=3`，发布环境 `m5stack-sticks3-release` 使用 `CORE_DEBUG_LEVEL=0`。

## 生成 Release 文件

```powershell
python helper\prepare_release.py --repo OWNER/REPO --notes "本次更新说明"
```

输出目录：

```text
dist/release/
  firmware.bin
  manifest.json
  StickS3ClaudeCodexHelper.exe   # 如果 helper/dist 下存在，会自动复制
```

如果需要把当前固件快照放进仓库树，可以同步到：

```text
releases/latest/
  firmware.bin
  manifest.json
```

`manifest.json` 字段：

```json
{
  "version": "1.0.0",
  "url": "https://github.com/OWNER/REPO/releases/latest/download/firmware.bin",
  "sha256": "firmware.bin 的 sha256",
  "size": 2957445,
  "notes": "本次更新说明"
}
```

## 远程 OTA

固件通过 `REMOTE_OTA_MANIFEST_URL` 读取 manifest。用户在 **设置 → 远程 OTA** 检查更新时，版本不同才会下载 `firmware.bin`；下载后按 `size` 和 `sha256` 校验，再写入 OTA 分区。

`notes` 是可选字段，建议保持在 30 个中文字符以内，板子会在发现新版本后短暂显示。
