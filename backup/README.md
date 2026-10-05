# 出厂固件备份

`factory_flash_16MB.bin` 是 2026-09-29 第一次上传自己的固件之前，用 esptool 从设备读出的整片 16MB flash（已用 `verify-flash` 和设备核对一致）。它不进 git，只留在本机，建议另外再存一份。

- SHA-256：`6808ec181311598a0f37eb920bd19175357fd61dc98acdc360f5cf8074012002`
- 恢复（大约几分钟）：`uv run esptool write-flash 0x0 backup/factory_flash_16MB.bin`

出厂分区表：

| 名称 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 16 KB |
| otadata | data/ota | 0xD000 | 8 KB |
| phy_init | data/phy | 0xF000 | 4 KB |
| ota_0 | app | 0x20000 | 4.94 MB |
| ota_1 | app | 0x510000 | 4.94 MB |
| storage | data/fat | 0xA00000 | 4 MB |
| coredump | data/coredump | 0xE00000 | 64 KB |
