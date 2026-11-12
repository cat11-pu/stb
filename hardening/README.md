# stb_image 畸形输入加固

## 背景与基线

仓库原始版本为 `stb_image` v2.30（见 `hardening/baseline/stb_image_v2.30_orig.h`）。
加固在两个层面进行：

1. **升级到上游 v2.32**：吸收 2026-11 发布的 2.31/2.32 两个版本共 44 个提交，
   其中包含大量安全修复，关键项包括：
   - GIF 解码器改为非递归实现（消除畸形帧导致的栈耗尽）
   - JPEG 坏标记拒绝、渐进式 JPEG 提前退出
   - TGA 错误路径内存泄漏修复、水平翻转支持
   - 8/16 位转换在分配失败时的原始数据泄漏修复
   - `getn` 的两处 UB 修复、PNM 原生端序修复、BMP v3 头修复
   - HDR 重建数学修正（因此 HDR 像素与 v2.30 有意不同，见下）
2. **本目录内的自研加固层**（直接写入根目录 `stb_image.h`，搜索
   `hardening` 注释即可定位）：
   - 单次内部分配上限，默认 256 MB，可运行时调整
    - 上限同时覆盖 malloc、zlib 输出缓冲 realloc、PNG IDAT 累积 realloc
   - 失败位置（输入流字节偏移）追踪与可读的组合错误信息

## 新增公共 API

```c
void        stbi_set_max_alloc(int max_bytes); // 0=默认上限; 负数=关闭
int         stbi_get_max_alloc(void);
long long   stbi_failure_offset(void);          // 最近一次失败的字节偏移，-1 表示未知
const char *stbi_failure_detail(void);          // "<reason> @ offset <n>"
```

- 上限默认值由编译期宏 `STBI_MAX_ALLOC_DEFAULT` 控制（默认 `256*1024*1024`）。
- 所有新增状态在支持 `STBI_THREAD_LOCAL` 的平台上为线程局部，已做多线程隔离测试。
- 不改变任何既有 API 的签名与语义；合法图片解码输出像素保持不变。

## 目录结构

```
hardening/
  baseline/   原始 v2.30 头文件（一致性比对的对照基线）
  harness/    生成器、崩溃驱动器、守卫测试、基准测试
  corpus/     2000 个畸形样本（确定性生成，可随时重建）
  valid/      17 个全格式合法样本（PNG/BMP/TGA/JPG/PSD/GIF/HDR/PNM，8/16 位）
  reports/    run_all.sh 产生的报告与中间像素转储
  run_all.sh  一键验证（崩溃测试 + 守卫 + 一致性 + 性能）
```

## 畸形样本集（2000 个，可重建）

`python3 hardening/harness/gen_malformed.py` 以固定 RNG 种子
（`0x57B17A6E`）确定性生成，覆盖：

- 9 个格式家族（PNG/JPG/BMP/TGA/GIF/PSD/HDR/PNM + 手工结构样本）
- 变异类型：定点/随机截断、位翻转、多点重度翻转、头部定向砸坏、
  插入/删除、长度/维度字段篡改（0x7FFFFFFF 等 int32 边界值）
- 解压炸弹类：PNG 20000x20000（解码输出约 400 MB，命中 256 MB 分配上限）
- 手工样本：空输入、魔数后全 0/全 0xFF、坏 zlib 流、零尺寸描述符、
  负尺寸 PNM、超界 BMP 像素偏移等

## 一键验证

```bash
./hardening/run_all.sh
```

验收标准与实测结果（报告见 `hardening/reports/`）：

1. **零崩溃**：2000 个样本在 ASan+UBSan 构建与普通构建下均无
   越界读/写、无 UB、无内存泄漏、无 OOM 杀手；畸形输入一律返回 NULL + 错误码。
   另有 220 万次进程内随机模糊（`random_fuzz.c`，ASan 70 万 + 原生 150 万）零发现。
2. **无超量分配**：仪器化分配器确认任何单次分配都不超过当前上限，
   维度炸弹在到达系统分配器之前即被拒绝。
3. **错误可定位**：每个失败都可通过 `stbi_failure_offset()` /
   `stbi_failure_detail()` 得到输入流偏移，内存与回调两种 I/O 路径均验证。
4. **像素一致性**：合法样本（17 个自研 + pngsuite 258 个 PNG，共 500+ 个
   8/16/32 位像素转储）与 v2.30 逐字节相同。唯一例外是 HDR：
   v2.32 修复了 v2.30 的 RGBE 反量化错误（上游提交 `ade8b91`，
   由 floor 量化改为中心重建），属于有意的正确性修复。
5. **性能**：2048x2048 大图交替运行取中位数，各格式耗时变化在 ±2% 以内，
   远低于 10% 预算；上限检查本身在纯分配微基准中不可测（约 4.4 亿次/秒）。

## 已验证的编译配置

`-O2 -Wall -Wextra` 下 C 与 C++ 均零告警通过，并额外验证：
`STBI_NO_JPEG/PNG/BMP/TGA/GIF/PSD/HDR/PNM/LINEAR/STDIO/FAILURE_STRINGS`、
`STBI_ONLY_PNG` 共 12 种裁剪配置均可编译运行。
