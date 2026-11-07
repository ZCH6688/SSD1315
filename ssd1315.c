#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/string.h>


/* OLED 参数 */
#define SSD1315_WIDTH          128
#define SSD1315_HEIGHT         64
#define SSD1315_PAGE_NUM       (SSD1315_HEIGHT / 8)
#define SSD1315_PAGE_SIZE      SSD1315_WIDTH

/* I2C 控制字节 */
#define SSD1315_CMD_CTRL       0x00
#define SSD1315_DATA_CTRL      0x40

/* 每次发送的显示数据长度 */
#define SSD1315_I2C_CHUNK      16

struct ssd1315_device {
    struct i2c_client *client;

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
    ret = ssd1315_fill(oled, 0xFF);
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