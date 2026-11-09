# 中文字库生成说明（LVGL）

LVGL 只渲染字库里有字形的字符，缺字会显示为空白/方块。本工程把**全部中文文案集中在
`main/ui/ui_strings.h`**，字库按该文件实际用到的字符生成子集（不要生成
`0x4E00–0x9FA5` 全量，会多占 1 MB 以上 flash）。

## 1. 字符清单来源

修改任何文案后，在工程根目录运行：

```bash
python main/ui/gen_font_chars.py
```

脚本会：
- 去掉 `ui_strings.h` 中的注释；
- 抽取所有 `"..."` 字面量；
- 写出 `main/ui/font_chars.txt`（全部去重字符，含 ASCII）和
  `main/ui/font_symbols.txt`（仅非 ASCII 字符，给 `--symbols` 用）。

当前统计（M5）：`font_symbols.txt` 共 264 个非 ASCII 字符；加上 ASCII 0x20–0x7E 共 359 个字形
（两份字库的 cmap `list_length` 均为 263——`°` 不在字体里，由 Montserrat 回退，故比字符数少 1）。

## 2. 需要的两个字库

| 变量名 | 字号 | 用途 |
|---|---|---|
| `font_cn_16` | 16 px | 正文、按钮、列表、状态栏 |
| `font_cn_24` | 24 px | 页面标题、识别结果、开门覆盖层大字 |

ASCII（数字、字母、`%`、`.`、`-`、`:`、`/`、空格）在两个字库里都要有，
否则识别分数、时间、IP、版本号无法显示。

## 3. 用 lv_font_conv 生成（推荐）

```bash
npm install -g lv_font_conv

# 16px 正文
lv_font_conv --font SourceHanSansSC-Regular.otf --size 16 --bpp 4 --format lvgl \
  --symbols "$(cat main/ui/font_symbols.txt)" \
  --range 0x20-0x7E \
  -o main/ui/ui_fonts/font_cn_16.c \
  --lv-include lvgl.h --no-compress

# 24px 标题（建议 Medium/Bold 字重）
lv_font_conv --font SourceHanSansSC-Medium.otf --size 24 --bpp 4 --format lvgl \
  --symbols "$(cat main/ui/font_symbols.txt)" \
  --range 0x20-0x7E \
  -o main/ui/ui_fonts/font_cn_24.c \
  --lv-include lvgl.h --no-compress
```

Windows 下若 `$(cat ...)` 不可用，可把 `font_symbols.txt` 的内容直接粘贴到 `--symbols "..."`。
**注意**：Git Bash 里 `var="$(cat ...)"` 传给 `npx.cmd` 时会被按 GBK 代码页转换，命令行里的符号
（见生成文件头部的 `Opts:`）会变成乱码、字库缺字。推荐用 PowerShell 以 UTF-8 读文件后调用：

```powershell
$symbols = [IO.File]::ReadAllText("main/ui/font_symbols.txt", [Text.UTF8Encoding]::new($false)).Replace("`r","").Replace("`n","")
npx lv_font_conv --bpp 4 --size 16 --no-compress --font AliFangYuanT.ttf --symbols $symbols `
  --range 0x20-0x7E --format lvgl -o main/ui/ui_fonts/font_cn_16.c --lv-include lvgl.h
```
中文冒号、逗号、句号已在字符清单中，注意文件必须保存为 **UTF-8 无 BOM**。

### 文件约定（必须一致）

生成的文件里字体变量名必须是：

```c
lv_font_t font_cn_16;   /* 16px */
lv_font_t font_cn_24;   /* 24px */
```

`lv_font_conv` 默认用输出文件的 basename 作为变量名，即 `font_cn_16.c` → `font_cn_16`，
按上表命名输出即可。若变量名不符，用正确 basename 重新生成，不要手改生成文件。

请在生成的 `.c` 头部保留注释说明来源字体与许可证。

## 4. 其他生成方式

- **在线转换器**：<https://lvgl.io/tools/fontconverter>，参数与上面相同
  （Bpp=4, Format=LVGL, Range=`0x20-0x7E`, Symbols=粘贴 `font_symbols.txt`, No compression）。
- **阿里惠普体/思源黑体**（OFL 许可，可商用）比微软雅黑安全。

## 5. 放进工程



1. 把 `font_cn_16.c`、`font_cn_24.c` 放到 `main/ui/ui_fonts/`（目录需存在）。
2. `main/CMakeLists.txt` 的 `INCLUDE_DIRS` 已包含 `.` 与新增目录时补 `ui`、`ui/ui_fonts`
   （若字体文件只在源文件中引用，`ui_fonts` 目录不必加进 INCLUDE_DIRS）。
3. 页面代码里引用：

```c
LV_FONT_DECLARE(font_cn_16);
LV_FONT_DECLARE(font_cn_24);

lv_obj_set_style_text_font(label, &font_cn_16, 0);
lv_obj_set_style_text_font(title, &font_cn_24, 0);
```

4. 在 `lv_conf.h` / `sdkconfig` 中保持 `LV_USE_FONT_COMPRESSED=0`（对应 `--no-compress`）。

## 6. 校验

烧录后逐个页面截图检查，重点确认：

- 标题、按钮、列表行**没有空白方块**；
- 数字和 `%`、`°C` 正常显示；
- 长句（如“恢复出厂将清空人脸库与全部配置”）自动换行不截断。
