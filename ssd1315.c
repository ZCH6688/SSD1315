#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "ssd1315_font.h"



/* OLED 参数 */
#define SSD1315_WIDTH          128
#define SSD1315_HEIGHT         64
#define SSD1315_PAGE_NUM       (SSD1315_HEIGHT / 8)
#define SSD1315_PAGE_SIZE      SSD1315_WIDTH
#define SSD1315_FB_SIZE        (SSD1315_WIDTH * SSD1315_HEIGHT / 8)

/* I2C 控制字节 */
#define SSD1315_CMD_CTRL       0x00
#define SSD1315_DATA_CTRL      0x40

/* 每次发送的显示数据长度 */
#define SSD1315_I2C_CHUNK      16


struct ssd1315_device {
    struct i2c_client *client;

    /* OLED 软件显存 */
    u8 framebuffer[SSD1315_FB_SIZE];

    /* 并发放问保护 */
    struct mutex lock;
};



/*
 * 向 SSD1315 发送命令
 *
 * I2C 数据格式：
 * [0x00][CMD1][CMD2]...[CMDn]
 */
static int ssd1315_write_cmds(struct ssd1315_device *oled,
                              const u8 *cmds,
                              size_t len)
{
    u8 *buf;
    int ret;

    if (!cmds || !len)
        return -EINVAL;

    buf = kmalloc(len + 1, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    buf[0] = SSD1315_CMD_CTRL;

    memcpy(&buf[1], cmds, len);

    ret = i2c_master_send(oled->client, buf, (int)(len + 1));

    kfree(buf);

    if (ret < 0)
        return ret;

    if (ret != (int)(len + 1))
        return -EIO;

    return 0;
}

/*
 * 向 SSD1315 发送显示数据
 *
 * I2C 数据格式：
 * [0x40][DATA1][DATA2]...[DATAn]
 *
 * 每次最多发送 16 字节显示数据，
 * 避免一次传输过长。
 */
static int ssd1315_write_data(struct ssd1315_device *oled,
                              const u8 *data,
                              size_t len)
{
    u8 buf[1 + SSD1315_I2C_CHUNK];
    size_t chunk;
    int ret;

    while (len > 0) {

        chunk = len > SSD1315_I2C_CHUNK ?
                SSD1315_I2C_CHUNK : len;

        buf[0] = SSD1315_DATA_CTRL;

        memcpy(&buf[1], data, chunk);

        ret = i2c_master_send(oled->client,
                              buf,
                              (int)(chunk + 1));

        if (ret < 0)
            return ret;

        if (ret != (int)(chunk + 1))
            return -EIO;

        data += chunk;
        len -= chunk;
    }

    return 0;
}

/*
 * 设置 SSD1315 的显存位置
 *
 * Page Addressing Mode:
 * 0xB0 ~ 0xB7：选择 Page
 * 0x00 ~ 0x0F：设置列地址低四位
 * 0x10 ~ 0x17：设置列地址高位
 */
static int ssd1315_set_page(struct ssd1315_device *oled,
                            u8 page)
{
    u8 cmds[] = {
        (u8)(0xB0 | page),
        0x00,
        0x10
    };

    return ssd1315_write_cmds(oled, cmds, sizeof(cmds));
}

/*
 * 清空 OLED 显存
 *
 * 128 * 64 / 8 = 1024 Bytes
 *
 * 每个 Page 写入 128 Bytes 的 0x00。
 */
static int ssd1315_clear(struct ssd1315_device *oled)
{
    u8 zero[SSD1315_PAGE_SIZE] = {0};

    int page;
    int ret;

    for (page = 0; page < SSD1315_PAGE_NUM; page++) {

        /* 设置当前 Page 和起始列 */
        ret = ssd1315_set_page(oled, page);
        if (ret < 0)
            return ret;

        /* 清空当前 Page */
        ret = ssd1315_write_data(oled,
                                 zero,
                                 sizeof(zero));

        if (ret < 0)
            return ret;
    }

    return 0;
}

static int ssd1315_fill(struct ssd1315_device *oled, u8 value)
{
    u8 data[SSD1315_PAGE_SIZE];

    int page;
    int ret;

    memset(data, value, sizeof(data));

    for (page = 0; page < SSD1315_PAGE_NUM; page++) {

        ret = ssd1315_set_page(oled, page);
        if (ret < 0)
            return ret;

        ret = ssd1315_write_data(oled,
                                 data,
                                 sizeof(data));

        if (ret < 0)
            return ret;
    }

    return 0;
}

/* 修改指定像素 
 *
 * 注意调用者必须持有mutex
 * 
 * x:横坐标，范围0～127
 * y:纵坐标，范围0～63
 * color: 1表示点亮，0表示熄灭
*/
static void ssd1315_draw_point_locked(struct ssd1315_device *oled, u8 x, u8 y, u8 color)
{
    u16 offset;
    u8 mask;

    /* 计算现存偏移 */
    offset = (y / 8) * SSD1315_WIDTH + x;

    mask = (u8)(1 << (y % 8));


    if(color)
        oled->framebuffer[offset] |= mask;
    else
        oled->framebuffer[offset] &= ~mask;
}

/*
 * 绘制直线
 *
 * 使用 Bresenham 算法。
 *
 * 注意：调用者必须持有 oled->lock。
 */
static int ssd1315_draw_line_locked(struct ssd1315_device *oled,
                                    int x0, int y0,
                                    int x1, int y1,
                                    int color)
{
    int dx, dy;
    int sx, sy;
    int err, e2;

    /* 检查坐标范围 */
    if (x0 < 0 || x0 >= SSD1315_WIDTH ||
        x1 < 0 || x1 >= SSD1315_WIDTH ||
        y0 < 0 || y0 >= SSD1315_HEIGHT ||
        y1 < 0 || y1 >= SSD1315_HEIGHT)
        return -EINVAL;

    dx = abs(x1 - x0);
    dy = -abs(y1 - y0);

    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;

    err = dx + dy;

    while (1) {

        /* 绘制当前像素 */
        ssd1315_draw_point_locked(oled,
                                  x0, y0,
                                  color);

        if (x0 == x1 && y0 == y1)
            break;

        e2 = 2 * err;

        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }

    return 0;
}

/*
 * 绘制空心矩形
 *
 * 注意：调用者必须持有 oled->lock。
 */
static int ssd1315_draw_rect_locked(struct ssd1315_device *oled,
                                    int x0, int y0,
                                    int x1, int y1,
                                    int color)
{
    int ret;

    if (x0 < 0 || x0 >= SSD1315_WIDTH ||
        x1 < 0 || x1 >= SSD1315_WIDTH ||
        y0 < 0 || y0 >= SSD1315_HEIGHT ||
        y1 < 0 || y1 >= SSD1315_HEIGHT ||
        x0 > x1 || y0 > y1)
        return -EINVAL;

    /* 上边 */
    ret = ssd1315_draw_line_locked(oled,
                                   x0, y0,
                                   x1, y0,
                                   color);
    if (ret < 0)
        return ret;

    /* 下边 */
    ret = ssd1315_draw_line_locked(oled,
                                   x0, y1,
                                   x1, y1,
                                   color);
    if (ret < 0)
        return ret;

    /* 左边 */
    ret = ssd1315_draw_line_locked(oled,
                                   x0, y0,
                                   x0, y1,
                                   color);
    if (ret < 0)
        return ret;

    /* 右边 */
    ret = ssd1315_draw_line_locked(oled,
                                   x1, y0,
                                   x1, y1,
                                   color);

    return ret;
}

/*
 * 绘制实心矩形
 *
 * 注意：调用者必须持有 oled->lock。
 */
static int ssd1315_fill_rect_locked(struct ssd1315_device *oled,
                                    int x0, int y0,
                                    int x1, int y1,
                                    int color)
{
    int x, y;

    if (x0 < 0 || x0 >= SSD1315_WIDTH ||
        x1 < 0 || x1 >= SSD1315_WIDTH ||
        y0 < 0 || y0 >= SSD1315_HEIGHT ||
        y1 < 0 || y1 >= SSD1315_HEIGHT ||
        x0 > x1 || y0 > y1)
        return -EINVAL;

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {

            ssd1315_draw_point_locked(oled,
                                      x, y,
                                      color);
        }
    }

    return 0;
}

/*
 * 显示一个 ASCII 字符
 *
 * x、y：字符左上角坐标
 * fg：前景色，1 点亮，0 熄灭
 * bg：背景色，1 点亮，0 熄灭
 *
 * 字符占用 6x8 像素：
 * 5x7 字模 + 1列字符间距 + 1行底部间距。
 *
 * 注意：调用者必须持有 oled->lock。
 */
static int ssd1315_draw_char_locked(struct ssd1315_device *oled,
                                    unsigned int x,
                                    unsigned int y,
                                    unsigned char ch,
                                    int fg,
                                    int bg)
{
    const u8 *glyph;
    unsigned int col;
    unsigned int row;

    /* 检查字符显示区域是否越界 */
    if (x > SSD1315_WIDTH - 6 ||
        y > SSD1315_HEIGHT - 8)
        return -EINVAL;

    /* 不支持的字符统一显示为 '?' */
    if (ch < 0x20 || ch > 0x7E)
        ch = '?';

    glyph = ssd1315_font5x7[ch - 0x20];

    /*
     * 先绘制完整字符背景。
     * 这样不会留下上一次显示的残留像素。
     */
    ssd1315_fill_rect_locked(oled,
                             x, y,
                             x + 5, y + 7,
                             bg);

    /* 绘制 5x7 字模 */
    for (col = 0; col < 5; col++) {

        for (row = 0; row < 7; row++) {

            if (glyph[col] & (1U << row)) {

                ssd1315_draw_point_locked(oled,
                                          x + col,
                                          y + row,
                                          fg);
            }
        }
    }

    return 0;
}

/*
 * 显示 ASCII 字符串
 *
 * x、y：字符串起始坐标
 * fg：前景色
 * bg：背景色
 *
 * 支持：
 * 1. 自动换行
 * 2. '\n' 换行
 * 3. '\r' 忽略
 *
 * 注意：调用者必须持有 oled->lock。
 */
static int ssd1315_draw_string_locked(struct ssd1315_device *oled,
                                      unsigned int x,
                                      unsigned int y,
                                      const char *str,
                                      int fg,
                                      int bg)
{
    unsigned int cursor_x = x;
    unsigned int cursor_y = y;
    unsigned char ch;
    int ret;

    if (!str)
        return -EINVAL;

    if (x > SSD1315_WIDTH - 6 ||
        y > SSD1315_HEIGHT - 8)
        return -EINVAL;

    while (*str) {

        ch = (unsigned char)*str++;

        /* 忽略回车 */
        if (ch == '\r')
            continue;

        /* 处理换行 */
        if (ch == '\n') {

            cursor_x = x;
            cursor_y += 8;

            if (cursor_y > SSD1315_HEIGHT - 8)
                break;

            continue;
        }

        /* 当前行空间不足，自动换行 */
        if (cursor_x > SSD1315_WIDTH - 6) {

            cursor_x = x;
            cursor_y += 8;
        }

        /* 屏幕空间不足，停止显示 */
        if (cursor_y > SSD1315_HEIGHT - 8)
            break;

        ret = ssd1315_draw_char_locked(oled,
                                       cursor_x,
                                       cursor_y,
                                       ch,
                                       fg,
                                       bg);

        if (ret < 0)
            return ret;

        /* 下一个字符向右移动 6 个像素 */
        cursor_x += 6;
    }

    return 0;
}

/*
 * 将软件显存刷新到 SSD1315
 *
 * 注意：调用者必须持有 oled->lock
 *
*/
static int ssd1315_refresh_locked(struct ssd1315_device *oled)
{
    int ret;
    u8 page;
    size_t offset;

    for(page = 0; page < SSD1315_PAGE_NUM; page++)
    {
        /* 计算当前 page 在 framebuffer 中的偏移 */
        offset = page * SSD1315_WIDTH;

        /* 设置当前 Page 在 framebuffer 中的偏移*/
        ret = ssd1315_set_page(oled, page);
        if(ret < 0)
            return ret;
        ret = ssd1315_write_data(oled, &oled->framebuffer[offset], SSD1315_PAGE_SIZE);
        if(ret < 0)
            return ret;

    }

    return 0;
}

/*
 * 清空软件显存
 *
 * 注意：调用者必须持有 oled->lock。
 */
static void ssd1315_fb_clear_locked(struct ssd1315_device *oled)
{
    memset(oled->framebuffer, 0, sizeof(oled->framebuffer));
}

/*
 * 清空 framebuffer，并刷新 OLED
 *
 * 注意：调用者必须持有 oled->lock。
 */
static int ssd1315_clear_locked(struct ssd1315_device *oled)
{
    ssd1315_fb_clear_locked(oled);

    return ssd1315_refresh_locked(oled);
}

/*
 * SSD1315 初始化
 */
static int ssd1315_init(struct ssd1315_device *oled)
{
    int ret;

    /*
     * 基础初始化命令
     *
     * 参考 SSD1315 数据手册
     */
    static const u8 init_cmds[] = {

        0xAE,       /* 关闭显示 */

        0xD5, 0x80, /* 设置显示时钟 */
        0xA8, 0x3F, /* 设置复用率：64 */
        0xD3, 0x00, /* 设置显示偏移 */
        0x40,       /* 设置起始行 */

        0x8D, 0x14, /* 启用内部电荷泵 */

        0x20, 0x02, /* Page Addressing Mode */

        0xA1,       /* 设置列映射 */
        0xC8,       /* 设置行扫描方向 */
        0xDA, 0x12, /* 设置 COM 引脚配置 */

        0x81, 0x7F, /* 设置对比度 */
        0xD9, 0x22, /* 设置预充电周期 */
        0xDB, 0x20, /* 设置 VCOMH */

        0xA4,       /* 根据 GDDRAM 显示 */
        0xA6,       /* 正常显示模式 */

        0x2E        /* 关闭滚动功能 */
    };

    u8 display_on = 0xAF;

    mutex_lock(&oled->lock);

    /*
     * 等待电源稳定。
     *
     * 当前模块没有单独引出 RES# 引脚，
     * 因此暂时依赖模块自身的上电复位。
     */
    msleep(20);

    /* 发送初始化命令 */
    ret = ssd1315_write_cmds(oled,
                             init_cmds,
                             sizeof(init_cmds));

    if (ret < 0)
        goto out;

    /* 清空显存 */
    ret = ssd1315_clear_locked(oled);
    if (ret < 0)
        goto out;   

    /* 打开显示 */
    ret = ssd1315_write_cmds(oled,
                             &display_on,
                             1);

out:
    mutex_unlock(&oled->lock);

    return ret;
}

/*
 * 测试 framebuffer
 *
 * 显示矩形边框和一条对角线。
 */
/*
 * V3.0 图形与字符显示测试
 */
static int ssd1315_test_pattern(struct ssd1315_device *oled)
{
    int ret;

    mutex_lock(&oled->lock);

    /* 清空软件显存 */
    ssd1315_fb_clear_locked(oled);

    /* 绘制屏幕边框 */
    ret = ssd1315_draw_rect_locked(oled,
                                   0, 0,
                                   127, 63,
                                   1);
    if (ret < 0)
        goto out;

    /* 显示第一行字符串 */
    ret = ssd1315_draw_string_locked(oled,
                                     6, 8,
                                     "Hello Linux!",
                                     1, 0);
    if (ret < 0)
        goto out;

    /* 显示第二行字符串 */
    ret = ssd1315_draw_string_locked(oled,
                                     6, 24,
                                     "SSD1315 OLED",
                                     1, 0);
    if (ret < 0)
        goto out;

    /* 显示第三行字符串 */
    ret = ssd1315_draw_string_locked(oled,
                                     6, 40,
                                     "i.MX6ULL",
                                     1, 0);
    if (ret < 0)
        goto out;

    /* 绘制底部横线 */
    ret = ssd1315_draw_line_locked(oled,
                                   6, 54,
                                   121, 54,
                                   1);
    if (ret < 0)
        goto out;

    /* 刷新 OLED */
    ret = ssd1315_refresh_locked(oled);

out:
    mutex_unlock(&oled->lock);

    return ret;
}



/*
 * I2C 设备 probe
 */
static int ssd1315_probe(struct i2c_client *client,
                         const struct i2c_device_id *id)
{
    struct ssd1315_device *oled;
    int ret;

    dev_info(&client->dev, "SSD1315 probe\n");

    /* 检查 I2C 控制器是否支持普通 I2C 传输 */
    if (!i2c_check_functionality(client->adapter,
                                 I2C_FUNC_I2C)) {

        dev_err(&client->dev,
                "I2C adapter does not support I2C transfer\n");

        return -EOPNOTSUPP;
    }

    /* 分配设备私有数据 */
    oled = devm_kzalloc(&client->dev,
                        sizeof(*oled),
                        GFP_KERNEL);

    if (!oled)
        return -ENOMEM;

    oled->client = client;

    mutex_init(&oled->lock);

    /* 保存私有数据 */
    i2c_set_clientdata(client, oled);

    /* 初始化 OLED */
    ret = ssd1315_init(oled);

    if (ret < 0) {
        dev_err(&client->dev,
                "SSD1315 initialization failed: %d\n",
                ret);

        return ret;
    }
    ret = ssd1315_test_pattern(oled);

    if (ret < 0) {
         dev_err(&client->dev,
                 "Framebuffer test failed: %d\n",
                 ret);
    return ret;
    }


    dev_info(&client->dev,
             "SSD1315 initialized: addr=0x%02x\n",
             client->addr);

    return 0;
}

/*
 * I2C 设备 remove
 */
static int ssd1315_remove(struct i2c_client *client)
{
    struct ssd1315_device *oled;
    u8 display_off = 0xAE;
    int ret;

    oled = i2c_get_clientdata(client);

    mutex_lock(&oled->lock);

    /* 卸载驱动时关闭 OLED */
    ret = ssd1315_write_cmds(oled,
                             &display_off,
                             1);

    mutex_unlock(&oled->lock);

    if (ret < 0)
        dev_warn(&client->dev,
                 "Failed to turn off OLED: %d\n",
                 ret);

    dev_info(&client->dev, "SSD1315 removed\n");

    return 0;
}

/* 设备树匹配表 */
static const struct of_device_id ssd1315_of_match[] = {
    {
        .compatible = "solomon,ssd1315",
    },
    {}
};

MODULE_DEVICE_TABLE(of, ssd1315_of_match);

/* I2C 设备 ID 表 */
static const struct i2c_device_id ssd1315_id[] = {
    {
        "ssd1315", 0
    },
    {}
};

MODULE_DEVICE_TABLE(i2c, ssd1315_id);

/* I2C 驱动 */
static struct i2c_driver ssd1315_driver = {

    .driver = {
        .name = "ssd1315",
        .of_match_table = ssd1315_of_match,
    },

    .probe = ssd1315_probe,
    .remove = ssd1315_remove,

    .id_table = ssd1315_id,
};

module_i2c_driver(ssd1315_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("bit0");
MODULE_DESCRIPTION("SSD1315 OLED I2C Driver");