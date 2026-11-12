# 加固验收结果（由 ./hardening/run_all.sh 生成数据）

## 崩溃与内存安全（2000 畸形样本）

| 构建 | 结果 |
| --- | --- |
| GCC `-fsanitize=address,undefined` | `samples=2000 rejected=1414 decoded=586 missing_reason=0`，stderr 零报告 |
| 普通 `-O2` 构建 | 同上，零告警 |

- 1414 个样本被拒绝（返回 NULL + 错误原因 + 字节偏移），586 个变异后仍合法的样本正常解码
- 无越界读/写、无 UB、无内存泄漏、无超时/被杀
- 额外随机模糊：ASan 下 70 万次 + 原生 150 万次迭代，零发现

## 分配上限（默认 256 MB，可运行时调整）

| 攻击样本 | 实际最大分配 | 结果 |
| --- | --- | --- |
| BMP 维度炸弹 0x7FFFFFFF | 0 字节（维度检查先行） | `too large @ offset 54` |
| PNG 20000x20000（约 400 MB 输出） | 4 KB | `allocation cap exceeded @ offset 65` |
| PNG zlib 解压炸弹（314 KB → 324 MB） | 0.3 MB | `too large @ offset 314988` |

上限同时覆盖 `stbi__malloc`、zlib 输出缓冲 realloc、PNG IDAT 累积 realloc 三条路径，
仪器化分配器确认没有任何请求越过当前上限。

## 像素一致性（对照 hardening/baseline 中的原始 v2.30）

- 自研 17 个合法样本：48 个非 HDR 像素转储（8/16/32 位三条解码路径）逐字节相同
- pngsuite 258 个 PNG：507 个像素转储逐字节相同；12 个损坏 PNG 两版均以相同原因拒绝
- HDR 唯一差异来自上游 v2.32 对 RGBE 反量化的有意正确性修复（提交 `ade8b91`）

## 性能（2048x2048 大图，交替运行 3 轮取中位数，120 次解码/轮）

| 格式 | v2.30 中位耗时 | 加固版中位耗时 | 差值 |
| --- | --- | --- | --- |
| PNG(RGBA) | 9.1946 s | 9.2578 s | +0.69% |
| JPEG | 9.9033 s | 9.9907 s | +0.88% |
| BMP | 5.3804 s | 5.2663 s | -2.12% |
| TGA | 7.1248 s | 7.0722 s | -0.74% |
| 灰度 PNG | 2.4135 s | 2.4073 s | -0.26% |

全部在 ±2.12% 内，满足 +10% 预算。纯分配微基准约 4.4 亿次 malloc/秒，
上限检查开销不可测。

## 错误定位示例

```
first not IHDR @ offset 8
image too large (allocation cap exceeded) @ offset 65
too large @ offset 54
wrong version @ offset 6
```

内存缓冲与 I/O 回调两条路径均验证；`stbi_failure_offset()` 返回 -1 表示未知，
`stbi_failure_detail()` 提供组合字符串。
