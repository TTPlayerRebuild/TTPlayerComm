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
当前核心已在 XP、Win7、Windows 11 对照测试通过；频谱允许整数输出误差 1，
均衡器/环绕使用文档所列浮点容差，并非所有输出逐位一致。

原版参考 DLL 未被覆盖；测试播放器、媒体副本和日志均在私有测试目录中。
另已验证两种播放器文件属性窗口的标签修改与写盘。
旧的 `ttpcomm_core.dll` 是先前阶段残留产物，使用本次输出的 `ttpcomm.dll`。
Windows 10 使用 Windows 11 代测，没有独立 Windows 10 实测结果。
