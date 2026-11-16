# stb_image PNG 区域流式接口

## 接口

```c
typedef struct stbi_region_decoder stbi_region_decoder;
typedef int (*stbi_region_row_callback)(void *user, int y, const stbi_uc *rgba);

stbi_region_decoder *stbi_region_begin(int x, int y, int width, int height,
                                       stbi_region_row_callback callback,
                                       void *user);
int stbi_region_feed(stbi_region_decoder *decoder, const void *data, int size);
int stbi_region_finish(stbi_region_decoder *decoder);
int stbi_region_complete(const stbi_region_decoder *decoder);
const char *stbi_region_failure_reason(const stbi_region_decoder *decoder);
void stbi_region_free(stbi_region_decoder *decoder);
```

```c
static int row(void *user, int y, const stbi_uc *rgba) {
   sink_row(user, y, rgba);  /* width * 4 bytes */
   return 1;
}

stbi_region_decoder *d = stbi_region_begin(x, y, width, height, row, sink);
while (have_input)
   if (!stbi_region_feed(d, chunk, chunk_size))
      break;
if (!stbi_region_finish(d))
   fprintf(stderr, "%s\n", stbi_region_failure_reason(d));
stbi_region_free(d);
```

`x, y` 是源图左上角为原点的像素坐标。`width, height` 必须大于 0，且区域必须落在 PNG 内。

输出固定为紧密排列的 RGBA8，每行 `width * 4` 字节，不包含行填充。回调按源图 `y`
从上到下调用；回调返回 0 会中止解码。回调中的 `rgba` 仅在本次回调期间有效。

`feed` 接受任意大小、任意切分位置的输入块。`finish` 表示输入已结束；只有目标区域
的全部行均已输出时才返回 1。`complete` 可随时查询是否已经完整输出。错误原因通过
`failure_reason` 获取。

## 一致性规则

- 先对整行做 PNG unfilter，再裁横向列。不能从区域左边界开始重建，因为 Sub/Avg/Paeth
  依赖同行左侧像素。
- 保留上一整行用于 Up/Avg/Paeth，再只把目标行裁剪成 RGBA8 回调输出。
- 解码到目标区域最后一行即停止，不再解后半张图。
- 颜色类型 0/2/3/4/6、位深 1/2/4/8/16、PLTE、tRNS 和 5 种 PNG 滤波都走同一输出转换。
- 输出等价于 `stbi_load(..., 4)` 后按 `(x, y, width, height)` 裁剪。
- 不受 `stbi_set_flip_vertically_on_load` 等全局加载选项影响。

## 边界用例

| 用例 | 预期 |
|---|---|
| `1x1`、整行、整列、整图、四个角 | 与整图裁剪逐字节相等 |
| 区域紧贴图像右/下边界 | 不越界，不包含邻行/邻列 |
| 五个 PNG filter，逐行轮换 | 前一行的重建状态正确交接 |
| 位深 1/2/4 的打包灰度 | 按 PNG 位序解包并缩放 |
| 调色板 + tRNS | 使用调色板 RGB 和截断 alpha |
| 16-bit + tRNS | 先按 16-bit 比较透明键，再取高字节输出 |
| 多 IDAT 块、1 字节输入切分 | 状态跨块保持，无丢字节 |
| 截断输入 | `finish` 返回 0，`complete` 为 0 |
| 回调主动中止 | `feed` 返回 0，错误原因包含 callback |
| 区域越界、宽高为 0、负坐标 | 创建或首次解析 IHDR 时失败 |
| Adam7 交错 PNG | 明确报错，不做静默整图回退 |
| 非 PNG | 明确报错 |

## 内存与验证

常驻状态主要包括 32 KiB Deflate 历史窗口、Huffman 表、两条未滤波扫描行、一条输入行
和一条目标 RGBA 行。除固定窗口外，内存随图像宽度和目标区域宽度线性增长，不分配
整图解码缓冲。

验证入口：

```sh
make -C tests test_image_region
cd tests && ./test_image_region
```

当前测试覆盖 12 组合成 PNG、3 个仓库内真实 PNG、1 个由 `stb_image_write` 生成的
fixed-Huffman PNG，以及 8 种输入切分粒度。测试对比对象始终是
`stbi_load_from_memory(..., 4)`。
