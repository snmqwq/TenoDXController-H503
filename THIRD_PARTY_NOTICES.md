# Third-Party Notices

本仓库包含第三方软件和工具生成代码。它们不受项目根目录中的 PolyForm Noncommercial License 1.0.0 重新授权，并继续适用各自许可证。

| 组件 | 位置 | 许可证 | 来源 |
| --- | --- | --- | --- |
| STM32H5 HAL Driver | `Drivers/STM32H5xx_HAL_Driver/` | BSD-3-Clause | STMicroelectronics；许可证见组件目录 |
| CMSIS Core | `Drivers/CMSIS/` | Apache-2.0 | Arm Limited；许可证见组件目录 |
| STM32H5 CMSIS Device | `Drivers/CMSIS/Device/ST/STM32H5xx/` | Apache-2.0 | STMicroelectronics；许可证见组件目录 |
| TinyUSB 0.21.0 | `tinyusb/` | MIT | [hathach/tinyusb](https://github.com/hathach/tinyusb)；基于上游 `src`，`class/cdc/cdc_device.c` 包含逐实例 ACM 能力解析适配，许可文本保留在源文件头中 |
| MultiButton | `App/button/third_party/` | MIT | [0x1abin/MultiButton](https://github.com/0x1abin/MultiButton)；许可证见同目录 `LICENSE` |
| STM32CubeMX / STM32CubeIDE 生成代码 | `Core/` | 对应 STM32Cube 软件包条款 | STMicroelectronics；保留各文件版权与许可声明 |

`ref/` 中的参考资料不属于项目公共许可证的授权范围。除非具体文件另有明确许可，不应假定获得复制、修改、分发或商业使用权。
