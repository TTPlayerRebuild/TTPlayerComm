# ZIP／RAR 归档服务（更新至 2026-10-12）

## 当前实现：ABI 2

音乐 ZIP 与 RAR 现在共用后台成员存储。新增 `ttpcomm_query_archive_stream`（序号 503）；也可通过 `ttpcomm_query_archive` 查询 ABI 2。原 67 个导出、ABI 1 同步验证后返回的约定保持兼容。下方 2026-10-11 记录保留作为 ABI 1 的历史说明。

| 接口 | ABI 2 约定 |
| --- | --- |
| `enumerate` | ZIP／RAR 按成员顺序枚举；返回名称仅在回调期间有效 |
| `open_member` | 成员头就绪即可返回；后台解压，Read 等待需要的范围，Seek 调整独立游标 |
| `status` | 查询已产生字节、总长度、任务 ID、是否完成及最终 HRESULT；校验完成前为 E_PENDING |
| `clear_cache` | 释放当前缓存；有活跃读取者的任务仍然有效；卸载 DLL 前释放所有流并调用此接口 |

仅保留一个当前成员任务。键包括外层路径、文件身份、大小、修改时间、成员名和限制；Clone 独立游标。CUE 文本由宿主以不保留模式读取，不挤掉同源音频。普通释放当前任务可继续完成；最后一个已取消／超时的读取者退出时取消生产任务。生产线程不调用读取者的取消回调，回调只在调用者线程轮询，状态须保持到流及其 Clone 全部释放。

分卷组通过 ROADF_VOLUME 标识禁用跨次缓存，包括未跨卷的单个成员，避免只验证首卷而忽略其他卷变化。当前成员缓存不等于固实块缓存；切换不同固实成员仍可能重解前置数据。

ZIP 读取不再复用播放器的 SkinPackage：宽字符 OEM 名称查找、文件目录索引、16 KiB 分块 Stored／Deflate、成员边界、结束标记、长度及 CRC 校验。支持 ZIP 注释和数据描述符；未加入 ZIP64、UTF-8 标志或加密 ZIP。皮肤包原有一次性读取语义不变。

Read 到最终长度时等待校验；后台错误不能变成成功 EOF。小成员内存、大成员自动删除临时文件沿用 ABI 1 的资源限制；播放器 CUE 文本预设更小上限。新的播放器启动及 Actions 校验要求 503 和 ABI 2，部署顺序为先发布新 DLL、再构建播放器。

### 本轮验证

- 同一 x86 DLL 在 Windows 11、XP SP3、Win7 SP1：旧 ABI 1 **163 项**、新 ABI 2 **232 项**、生产解码／CUE 分轨／Seek **35 组**通过。
- 原版 EXE 的真实 ZIP／RAR 接口和 CUE 路径分类在三系统对照完成；RAR4 正常，RAR5 失败，确认引擎仍位于原 EXE 中。替换原版 `ttpcomm.dll` 不会自动增加 RAR5。
- 67 个旧导出和 4 个扩展通过 ABI 检查；XP／Win7 静态导入审计通过。
- 测试与样本位于本地 `rebuild/tests/archive_support/cue_audit_20261012`，未纳入本仓库、Actions 或 DLL。完整证据与保留差异见相邻播放器仓库 `rebuild/docs/ARCHIVE_CUE_RECOVERY_20261012.md`。

## 2026-10-11：ABI 1 初始实施记录

## 实现范围

RAR 引擎已静态并入重建的 `ttpcomm.dll`，播放器不再通过 Windows Shell 解压 RAR，也不要求安装 WinRAR 或单独的 UnRAR.dll。ZIP 继续使用已有的 ZIP 结构解析及核心 DLL 的解压服务；本次未增加 7z／TAR 或压缩包写入。

原版 5.7.9 的 RAR 后端在 EXE 中：`004738B5` 选择归档工厂，`00473338` 打开 RAR，`00474051` 按成员顺序生成 `archive.rar|member`，实际打开声音源时读取成员。本实现保留这个宿主模型，用新版引擎补上 RAR5。原版 67 个导出及其序号未变，新增接口不会让原版 EXE 自动取得 RAR5 支持，使用新接口的重建播放器才会调用它。

## 引擎、构建和兼容性

- 上游：[RARLAB UnRAR](https://www.rarlab.com/rar_add.htm)，固定 `7.3.1`。
- 源码：`https://www.rarlab.com/rar/unrarsrc-7.3.1.tar.gz`。
- SHA256：`634900842a3737d9cc15bbcc71d4c74cc713437e0bca296a573424fe5f2660ab`。
- `cmake/unrar.cmake` 下载并核验源码；源码留在构建目录。
- `cmake/adapt_unrar.py` 只对构建副本添加“跳过固实前序成员时也通知取消”的回调。适配点不匹配时停止构建。
- 使用 RARDLL／UNRAR／SILENT；只解压，不调用命令行程序或弹出控制台密码／换卷对话框。
- 沿用 x86、VC-LTL、YY-Thunks，子系统 `5.01`；UnRAR 目标使用 `/arch:IA32`，未启用要求更高 CPU 的构建选项。
- 完整许可证保存在 `licenses/UnRAR-LICENSE.txt`，并保留在适配源码注释中，不嵌入 DLL 资源。UnRAR 的许可包含不得用于重建 RAR 压缩算法的限制；仓库保留许可证不替代各依赖自身的分发条件。

## ABI 1

头文件：`sdk/include/ttpcomm/archive_api.h`。新增具名导出 `ttpcomm_query_archive`，序号 **502**。连同 500、501，共 67 个旧导出和 3 个扩展导出。

调用方先初始化 `TtpCommArchiveApi.size`，查询 ABI 1，再验证 RAR4／RAR5 能力位及两个函数指针。SDK 的 `QueryArchive` 封装了此检查。结构采用固定宽度整数、UTF-16 字符串、回调和 `IStream`；不跨 DLL 边界传递 STL 对象或 CRT 释放责任。

| 接口 | 约定 |
| --- | --- |
| `enumerate` | 读取成员头，按归档顺序回调；不为普通成员解压内容，不按文件名排序 |
| `open_member` | 解压指定成员并完成长度、CRC／哈希验证；只有全部成功才返回只读 `IStream` |
| 成员属性 | Unicode 名称、64 位大小、字典大小、目录／加密／固实／链接／分卷标志 |
| `IStream` | 支持 Read、Seek、Stat、Clone、CopyTo；克隆共享只读存储并各自保存位置 |
| 密码 | 可由 options 提供 UTF-16 密码；引擎不保存密码 |
| 进度及取消 | options 回调返回 FALSE 取消；等待引擎锁、枚举、解压和固实前序处理均检查取消 |

枚举名称只在回调期间有效；需要保留时由调用方复制。所有返回流必须在卸载 DLL 前 Release。回调不得重入归档接口。上游含进程级错误状态，因此引擎调用串行化；已打开的成员流可以独立读取。

## 存储和限制

| 项目 | 默认策略 |
| --- | --- |
| 内存成员缓冲 | 16 MiB；调用方可设置，硬上限 64 MiB |
| 较大成员 | 使用 GUID 命名的独占临时文件，`FILE_FLAG_DELETE_ON_CLOSE`；不使用成员名作为落盘路径 |
| 单个成员 | 8 GiB，读取前检查大小，回调时禁止超出声明长度；CUE／文本缓冲适配器单独限制 64 MiB |
| 字典 | 默认 256 MiB，最高 512 MiB；固实前序成员也检查 |
| 成员数量 | 默认最多 100000 |
| 路径／链接 | 拒绝根路径、盘符、点及父目录片段；目录、链接不作为可播放成员 |

较大成员仍需先完整解压、校验，再交给解码器；磁盘空间不足会失败。固实归档读取后部成员可能需要重新解压前部数据，本次未实现跨歌曲固实缓存或并行预取。

错误区分缺少密码、密码错误、缺少分卷、字典超限、不支持的成员、CRC／数据错误、成员过大、取消及内存不足。分卷会读取同目录中可自动找到的后续卷；缺卷立即返回错误，不等待交互式选择。解压失败不暴露部分成员流。

## 播放器接入与发布

播放器的 RAR 枚举、声音源、Media Foundation、旧 Windows Media Reader、独立文件信息工作进程和包内 CUE 均使用此服务。音频 Reader 直接取得可定位的流，避免先完整复制至 vector，再复制至 HGLOBAL。

播放器启动及 Actions 的核心组件检查要求 502 归档接口，打包也校验 `archive_api_export`。发布顺序为：先发布含新接口的 `ttpcomm`，再构建／发布播放器。旧版已发布 DLL 不满足新播放器要求；更新时按既定方式手动替换完整包。

播放器目前未增加密码输入界面或归档专用进度对话框；没有提供密码时明确报告需要密码。取消／进度是可供宿主调用的引擎能力，尚未接入全部 UI 操作的取消按钮。普通 RAR 枚举仍在现有导入调用线程执行，包内 CUE 读取较慢时仍可能等待。

## 验证

所有测试及样本仅保存在本地 `rebuild/tests/archive_support`，未加入 Actions。

- 本机 Windows 11、XP SP3、Win7 SP1：同一 x86 DLL 的 **163 项归档 API 检查通过**。
- 覆盖 RAR4 Stored／实际压缩、RAR5 Stored／普通压缩／固实、Unicode 成员及归档路径、数据密码／头密码、密码错误、分卷及缺卷、CRC 损坏、截断、非 RAR 文件。
- 覆盖包内顺序、逐字节比较、EOF、Seek、Clone、只读写入拒绝、磁盘后备存储、取消、字典／成员大小限制、四线程调用。
- RAR4 压缩样本来自 [libarchive 官方测试](https://github.com/libarchive/libarchive/blob/master/libarchive/test/test_read_format_rar_compress_best.rar.uu)，本地测试保留样本；其它小样本本地生成。
- DLL 导出审计：原 67 个导出全部保留，新增 3 个扩展；XP／Win7 静态导入审计通过。
- 播放器源码的 RAR／ZIP 后端：5 组样本全部逐字节一致，RAR 返回原始成员顺序。

播放器解码、包内 CUE 及发行文件验证结果见 `rebuild/docs/ARCHIVE_ENGINE_IMPLEMENTATION.md`。没有真实 Windows 10 环境；Windows 11 是现代系统验证环境，不据此宣称测试过 Windows 10。
