# 第三方依赖与许可证

依赖源码在构建时下载，不提交源码副本，不把许可证文本嵌入 DLL 资源。
本目录保留相应的许可证/版权文本；这不改变上游许可证及其分发义务。
尤其 libid3tag 使用 GPL，不能将含有它的构建结果描述为无许可限制。

| 依赖 | 固定版本/提交 | 用途 | 本地文本 |
| --- | --- | --- | --- |
| Tenacity libid3tag | `0.16.4` | ID3 对象、字段与 PIC 封面解析；构建目录内应用原版兼容修改 | `libid3tag-COPYING.txt`、`libid3tag-COPYRIGHT.txt`、`libid3tag-CREDITS.txt` |
| zlib | `1.3.2` | 解压后端；导出层恢复旧接口返回规则 | `zlib-LICENSE.txt` |
| KISS FFT | `131.2.0` | 频谱后端，恢复原版 x87 中间值舍入位置 | `kissfft-COPYING.txt`、`kissfft-BSD-3-Clause.txt` |
| SSRC（AviSynthPlus 历史实现） | `6c02f0dfac72678b80176e0aeefb7e1fde205b97` | double SSRC、Ooura FFT 与 Bessel | `SSRC-NOTICES.txt`、`LGPL-2.0.txt` |
| LAME mpglib | `3.100` | 仅编译三层 MPEG 解码原语，独立恢复 TTPlayer 缓冲及 ADU 协议 | `mpglib-AUTHORS.txt`、`mpglib-README.txt`、`LGPL-2.0.txt` |
| Goom / Dream | `wgoom-1.9.3-src.zip` | 恢复原版 Dream 控制流、IFS、缩放与线条 | `GOOM-NOTICES.txt`、`LGPL-2.0.txt` |
| CoolSB | 1.2，镜像提交 `472051497f176f5853eae03677f63260ad4eed44` | 非客户区滚动条及 TTPlayer 扩展 | `CoolSB-NOTICES.txt` |
| Microsoft Detours | `4.0.1` | 进程内 8 路 USER32 API 拦截 | `Detours-LICENSE.txt` |
| VC-LTL | `5.3.1` | 系统 MSVCRT 适配 | `VC-LTL-LICENSE.txt` |
| YY-Thunks | `1.2.2` | 旧 Windows API 兼容 | `YY-Thunks-LICENSE.txt` |

下载地址及 SHA256 的权威配置是 `cmake/dependencies.cmake` 和 `cmake/legacy_windows.cmake`。
源码源站分别为 Codeberg tenacityteam/libid3tag、madler/zlib、mborgerding/kissfft、AviSynth/AviSynthPlus、LAME/Goom 官方 SourceForge、CoolSB 1.2 源码镜像、microsoft/Detours、Chuyu-Team/VC-LTL5、Chuyu-Team/YY-Thunks。

libid3tag 的 `compat.gperf`、`frametype.gperf` 声明在构建时由 Python 转成有界二分查找表，保留上游字段和翻译函数；不要求额外安装 gperf。流派表由上游数据生成并保留 TTPlayer 的 148 项历史解释。生成代码只存在于构建目录。

SSRC 上游明确将该部分列为 LGPL，FFT 部分保留 Ooura 的单独使用许可；早期 foobar2000/PFC 修改声明也予保留。
本实现用自己的缓冲支持头替换 PFC/Avs 运行时，不链接 AviSynth DLL。
mpglib 固定旧版是为了恢复原版 double 算法，不能因此宣称已接入最新版 LAME，也不包含 LAME 编码器。

ReplayGain 系数由所提供参考二进制的数据表读取；均衡器、环绕、DTS、身份接口及兼容层按该二进制与伪代码恢复。
本阶段未复制工作区 Winamp 目录的实现。原版二进制仅作为私有对照，不是新 DLL 的运行依赖。

CoolSB 原文另有限制（禁止另外发布其源码或对源码收费），并非 MIT/BSD 许可；其源码只下载到构建目录，不纳入仓库。保留声明或构建时下载本身不会免除各依赖的分发义务。
Detours 事务记账改用 VirtualAlloc，避免挂起其他线程时争用其可能持有的 CRT 堆锁；此修改只作用于构建副本。
