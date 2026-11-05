#!/usr/bin/env python3
"""
D2 e2e fixture（P4 PhotoRec兜底雕刻引擎，2026-11-05 第28步排期）：
生成一个 2MB、无分区表的裸镜像，在一个扇区对齐的偏移处嵌入一张真实、
能被标准 JPEG 解码器解析的小图片。

这张 JPEG 的字节是第27步调研实测时用 Pillow 生成过、并且已经验证过
"PhotoRec 真的能把它雕刻出来、雕刻结果和原图哈希一致"的那一份——这里
直接把它的 base64 编码内联进脚本本身，不依赖这台机器上装没装
Pillow/PIL（测试 fixture 的可重复生成不应该依赖一个画图库）。

之所以不能用"只有 SOI+APP0+EOI、没有真实扫描数据"的极简假 JPEG（第27步
最早的尝试）：PhotoRec 的分块大小探测（STATUS_FIND_OFFSET）会根据镜像
里数据的模式猜一个 blocksize，几乎全零的裸镜像会被猜成一个很大的
blocksize（实测猜出 65536），导致雕刻时跳过的采样点未必落在那个假
JPEG 所在的字节范围内。一张字节数够、内容是真实压缩数据的 JPEG 不会
触发这个问题。
"""
import base64
import os

REAL_JPEG_BASE64 = (
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsK"
    "CwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQU"
    "FBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCABAAEADASIA"
    "AhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQA"
    "AAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3"
    "ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWm"
    "p6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEA"
    "AwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSEx"
    "BhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElK"
    "U1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3"
    "uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDwSiii"
    "v6zP0gKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKACiiigAooooAKKKKAC"
    "iiigAooooA//2Q=="
)

IMAGE_SIZE = 2 * 1024 * 1024
EMBED_OFFSET = 512 * 100  # 扇区对齐（512字节/扇区），和第27步实测用的偏移一致

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def main():
    jpeg_bytes = base64.b64decode(REAL_JPEG_BASE64)

    image = bytearray(IMAGE_SIZE)
    image[EMBED_OFFSET:EMBED_OFFSET + len(jpeg_bytes)] = jpeg_bytes

    with open(os.path.join(SCRIPT_DIR, "test_image.dd"), "wb") as f:
        f.write(image)
    with open(os.path.join(SCRIPT_DIR, "embedded.jpg"), "wb") as f:
        f.write(jpeg_bytes)

    print(f"embedded {len(jpeg_bytes)} bytes at offset {EMBED_OFFSET}")


if __name__ == "__main__":
    main()
