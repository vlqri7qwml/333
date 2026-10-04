# CrAcker Reader

Windows x64 本地内存读取组件，由常规驱动、静态 SDK 和验证工具组成。通过四级页表转换目标进程用户虚拟地址，使用 PTE 刷页后端读取数据。

## 功能

- `Open / Read / Query / Close`：打开目标、读取、查询设备信息、关闭连接。
- 支持 4 KiB、2 MiB、1 GiB 页的地址转换及跨页读取。
- 每个连接独立管理目标与映射窗口；单次内核请求最多 64 KiB，SDK 自动拆分大请求。
- 返回通信错误、内存操作状态和实际读取字节数；失败时仅返回有效数据前缀。

## 项目结构

| 路径 | 内容 |
| --- | --- |
| `Cracker.sln` | 解决方案，包含三个编译目标 |
| `driver/` | 驱动入口、会话、环境适配、页表转换与刷页读取 |
| `client/` | 本地 SDK |
| `shared/protocol.h` | 通信协议 |
| `verify/` | 离线检查与设备读取验证 |
| `build/`、`build.ps1` | 编译配置与构建脚本 |
| `out/` | 自动生成的构建产物 |

## 构建

需要 PowerShell 7、VS 2022 C++ 工具及版本匹配的 Windows SDK / WDK 头文件和库。

```powershell
pwsh -File .\build.ps1
pwsh -File .\build.ps1 -Configuration Debug
```

脚本自动选择已安装的 SDK/WDK 配对，使用 v143 编译并运行离线检查。可用 `-KitVersion 10.0.19041.0` 指定版本。

输出目录为 `out/x64/Release/` 或 `out/x64/Debug/`，主要产物是 `CrackerReader.sys`、`CrackerClient.lib`、`CrackerVerify.exe`。

## SDK 使用

业务工程链接 `CrackerClient.lib` 并包含 `client/reader.h`。运行前需在测试环境完成驱动签名、安装和启动。

```cpp
#include "client/reader.h"

cracker::Reader reader;
DWORD error = reader.Open(process_id);
if (error != ERROR_SUCCESS) return;

unsigned char buffer[4096];
auto result = reader.Read(target_address, buffer, sizeof(buffer));
if (!result.complete(sizeof(buffer))) {
    // error：Win32 通信错误；status：内存操作的 NTSTATUS。
    // 只有 buffer[0 .. result.bytes) 有效，其余内容保持原样。
}
reader.Close(); // 析构也会关闭。
```

`target_address` 是目标进程的用户虚拟地址。SDK 使用 `PROCESS_VM_READ` 句柄绑定目标，设备访问限管理员和 SYSTEM；当前接口仅提供读取。

同一连接的驱动请求串行执行，多个目标使用多个 `Reader`。SDK 允许并发调用 `Read`，但 `Open/Close` 需要与同一实例的其他调用互斥。

每个请求先锁定目标页，再经页表转换和 PTE 刷页复制。跨页或跨请求的读取不是原子快照。

## 验证与状态

```powershell
# 默认模式，无需驱动。
.\out\x64\Release\CrackerVerify.exe --self-test

# 需要测试环境中已安装并启动的驱动。
.\out\x64\Release\CrackerVerify.exe --device-test
```

验证状态（2026-09-27）：

| 项目 | 结果 |
| --- | --- |
| x64 Debug / Release 全量构建 | 通过，VS 2022 + SDK/WDK 10.0.19041.0 |
| 离线检查 | 两种配置各 15 项通过 |
| 驱动签名、加载、真实读取与卸载 | 尚未验证 |
| 性能与运行兼容性 | 尚未测量和确认 |

离线检查使用合成页表，覆盖大小页、PCID、缺页、读取失败、地址掩码及基础 SDK 行为。设备验证读取自身分配的已知数据，检查跨页、大请求分块、失败后的有效前缀和重复开关。

源码使用 Windows 10 2004 及更新版本提供的接口，支持四级 x64 页表；具体运行系统尚待验证。构建产物未签名，当前结果不足以确认真实驱动功能完好。

后续目标与下一阶段规划见 [项目方向.md](项目方向.md)。
