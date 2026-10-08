# 重建宿主运行接口 ABI 1

`2026.10.08p3` 新增 `ttpcomm_query_runtime`（ordinal 501），原 67 个导出及 ordinal 500 不变。旧宿主不调用新接口，仍使用 `ttpcomm_getversion()` 返回的 `0x50700`。

## 协议

声明：`sdk/include/ttpcomm/runtime_api.h`。调用约定为 x86 `__cdecl`；结构使用固定宽度整数，`#pragma pack(8)`。查询方先清零结构、填写 `size`，传入 `TTPCOMM_RUNTIME_ABI` 与所需能力位。成功返回 1 并填写函数表；失败返回 0，不部分接受请求。ABI 1 的结构布局固定，将来不兼容的改动必须使用新 ABI。

能力位：压缩 `0x01`、PCM `0x02`、标签 `0x04`。重建播放器要求三项均有。版本化的是接口协议，不是文件日期；不同日期版本只要满足 ABI 和能力即可配套。

服务返回值为 `OK=0`、`INVALID=-1`、`CAPACITY=-2`、`NOMEM=-3`、`DATA=-4`；标签遍历正常结束为 `END=1`。数量通常为字节，PCM 的 `samples` 为交错声道样本数。指针须对应调用方持有的有效缓冲区，容量按函数声明给出。

## 内存与生命周期

- PCM、CRC、常规解压由调用方分配输入输出缓冲区，DLL 不向宿主传递 STL、CRT 文件或异常。
- 量化器由 `quantizer_create` 创建，`quantizer_destroy` 销毁；状态属于独立播放/转换实例。不同实例可并行，同一实例的 encode/reset/destroy 由调用者串行。
- `tag_decode` 返回 `owner == NULL` 时借用输入；否则使用 `tag_decoded_destroy(owner)` 释放 DLL 内存。输出视图不能超出输入或 owner 的生命周期。
- `runtime_client.cpp` 只从 EXE 同目录加载核心并验证 ABI、能力和所有函数指针。成功的模块引用保留至进程结束，保证静态/线程局部对象的析构顺序安全。

## 语义边界

1. 解压要求 `Z_STREAM_END`；默认不接受尾随数据，返回实际输入消耗和输出字节数。`ALLOW_TRAILING` 只保留历史压缩 ID3 的容忍策略，不能用于严格 ZIP 校验。旧 ordinal 80–83 的返回语义不变。
2. PCM 显式区分内部 ±0.5 与公共 ±1 幅度；支持有效位与帧填充。效果链编码、最终量化是不同接口，舍入和抖动不可混用。
3. `runtime_pcm.cpp` 和 `runtime_quantizer.cpp` 使用 `/O2 /Ob2` 保持迁移前热路径的浮点运算；其余冷路径继续体积优化。
4. 标签 API 解析标准 ID3 帧和图片字段，不决定宿主的 GBK/ANSI、多值语义、文件保存事务或 UI。旧 ordinal 53/libid3tag 的兼容规则继续独立保留。
5. 原版 67 个导出及对应第三方算法没有因新增接口而替换。新版核心可供原版宿主使用；新版重建 EXE 则必须使用有 ABI 1 的核心。

## 发布和验证

首先发布 TTPlayerComm 新版，再由播放器 Action 解析 `latest`（或指定该版本）。播放器下载脚本要求 ordinal 501 存在；打包脚本不再允许省略经核验的核心。首次升级按已确认方式手动替换完整播放器包，避免旧更新器只装 EXE。

本地 `2026.10.08p3`：核心 259,072 字节，保留 67 个旧导出并有 2 个扩展，XP/Win7 导入审计通过。XP SP3、Win7 SP1、Windows 11 均通过新接口、PCM/增益、原抖动黄金值、压缩 ID3/封面/保存、并发与启动测试；原版宿主也能加载新核心播放。Win11 的 16 次真实 WaveOut 播放、8 对比较中 PCM16 逐字节一致。Win10 由 Win11 代测。

宿主 EXE 从 3,776,000 减至 3,733,504 字节；DLL 从 239,616 增至 259,072 字节；两者合计减少 23,040 字节。独立更新器仍自带解压器且大小不变。小型 SDK 助手和视觉专用 FFT 未继续迁移。详细边界及私有证据索引见播放器 `docs/TTPCOMM_REQUIRED_RUNTIME.md`。
