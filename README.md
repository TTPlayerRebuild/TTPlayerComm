# ttpcomm 重建

以千千静听 5.7.9 随附的 `ttpcomm.dll` 为参考，恢复 x86 导出、数据结构、处理行为及旧系统兼容性。

**已恢复全部 67 个有效导出，构建输出 `ttpcomm.dll`。**
已在 XP、Win7、Windows 11 的隔离副本中验证原版与重建版播放器的加载、MP3 播放和退出。
导出齐全不等于所有输入逐位一致；数值容差、未定义行为修正及测试边界见实施记录。

## 当前内容

- 运行时接口、线程独立随机数、分配/释放、MD5。
- ID3 标签对象、字段、解析与序列化；保留原版私有修改。
- zlib 解压兼容层。
- ReplayGain、均衡器、环绕处理、DTS 头检测、频谱。
- 多相 FIR 与 SSRC 重采样；MPEG Layer I/II/III 的 double 解码、分块和 ADU 接口。
- Dream 视觉效果：双声道输入、随机效果切换、调整尺寸、逐帧输出。
- CoolSB 1.2 扩展：自绘、鼠标捕获、滑块比例、8 路系统滚动条 API 拦截。
- 网卡/磁盘标识和 `uid=` 字符串接口。
- 固定导出清单、x86 布局断言、导出与 XP/Win7 导入检查。

实现依据、差异、测试覆盖和后续顺序见 [重建实施记录](docs/RECONSTRUCTION_STATUS.md)。
依赖来源及许可证见 [第三方说明](licenses/README.md)。

2026-10-08：KISS FFT 升级到 131.2.0，libid3tag 迁移到 Tenacity 0.16.4。
恢复原版 FFT 中间舍入步骤后，16,384 组频谱测试逐值一致。
标签兼容、PIC 封面改进和均衡器 EOF 补测见实施记录第 6 节。
后续已修复 ID3 反同步末尾越界、接通异常回调、改用无堆分配 TLS，并完成软件输出 PCM 对照；详见第 7 节。

## 构建

需要 Visual Studio C++ x86 工具链、Windows SDK、CMake 3.24+、Python 3、PowerShell。
使用现有现代工具链，通过 VC-LTL / YY-Thunks 生成兼容 XP 起的 DLL。

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release --parallel 4
```

安装 VS 2022 时，生成器改为 `Visual Studio 17 2022`。
首次配置下载固定版本并核验 SHA256；第三方源码仅存在构建目录。
可用 `FETCHCONTENT_SOURCE_DIR_*` 指定已验证的离线依赖缓存。

输出：

```text
build/Release/ttpcomm.dll
build/Release/SHA256SUMS.txt
build/Release/abi-report.json
build/Release/legacy-imports.json
```

默认文件版本取北京时间日期，格式与其他重建项目一致；也可传入
`-DTTPCOMM_BUILD_VERSION=2026.10.07` 或日期后的 `pN` 修订号。
为兼容原版宿主，`ttpcomm_getversion()` 始终返回接口版本 `0x00050700`，不随文件版本改变。

## 测试与发布边界

私有差分测试位于上级工作区 `rebuild/tests/ttpcomm_rebuild`，不在本项目内，不进入 Actions。
当前核心已在 XP、Win7、Windows 11 对照测试通过；频谱测试要求整数逐值相同，
均衡器/环绕使用文档所列浮点容差，并非所有输出逐位一致。

原版参考 DLL 未被覆盖；测试播放器、媒体副本和日志均在私有测试目录中。
另已验证两种播放器文件属性窗口的标签修改与写盘。
旧的 `ttpcomm_core.dll` 是先前阶段残留产物，使用本次输出的 `ttpcomm.dll`。
Windows 10 使用 Windows 11 代测，没有独立 Windows 10 实测结果。

## 日期发行包

```powershell
./build.ps1 -Package -SourcePackage
```

输出 `build/Release/ttpcomm-日期版本.zip` 和单独的 `-source.zip`。
运行包只包含播放器根目录的 `ttpcomm.dll` 与 `SHA256SUMS.txt`；归档校验清单为 `PACKAGE-SHA256SUMS.txt`。
源码包收录所用的固定 GPL/LGPL 等依赖源码，仓库仍只保留下载配置与许可证。

独立 Actions 的 **Release a Version** 在编译前选择日期/pN 版本，不运行私有测试。
先发布 TTPlayerComm 后，可在播放器 Actions 的 `ttpcomm_version` 中填写该版本或 `latest`，
经校验后将 DLL 合入播放器包。留空时不改变既有包内容。

## 共享 SDK（2026-10-08）

播放器的公共调用封装、PCM 转换、ID3 帧与封面解析、ZIP 结构、Base64/UTF-8 和流派表已收敛到 `sdk`。
播放器持有 `third_party/ttpcomm-sdk` 固定快照，可以独立构建；CMake 检查快照摘要，更新方式：

```powershell
python cmake/sync_sdk.py ../rebuild/third_party/ttpcomm-sdk
python cmake/sync_sdk.py ../rebuild/third_party/ttpcomm-sdk --check
```

C++ 辅助代码在宿主内编译，不跨 DLL 传递 STL 对象；原版 DLL 仍然可用。
新增 `ttpcomm_query_extension`（ordinal 500）只返回版本化能力信息，目前声明独立 ReplayGain 实例可并行。
详细迁移边界、保留差异和测试结果见实施记录第 9 节。
