"""Generate the PNG preview and multi-resolution Windows ICO from the icon design."""

from pathlib import Path
import math

from PIL import Image, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
SOURCE_SIZE = 512
SCALE = 2
SIZE = SOURCE_SIZE * SCALE


def px(value: float) -> int:
    return round(value * SCALE)


def box(x0: float, y0: float, x1: float, y1: float) -> tuple[int, int, int, int]:
    return tuple(px(value) for value in (x0, y0, x1, y1))


def generate() -> None:
    image = Image.new("RGBA", (SIZE, SIZE))
    pixels = image.load()
    for y in range(SIZE):
        for x in range(SIZE):
            t = max(0.0, min(1.0, (x + y) / (2 * (SIZE - 1))))
            pixels[x, y] = (
                round(17 + (7 - 17) * t),
                round(27 + (16 - 27) * t),
                round(50 + (31 - 50) * t),
                255,
            )

    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (px(16), px(16), SIZE - px(16), SIZE - px(16)),
        radius=px(112),
        fill=255,
    )
    image.putalpha(mask)

    # The dotted ring suggests radar range while staying quiet at small sizes.
    draw = ImageDraw.Draw(image)
    for index in range(48):
        angle = 2 * math.pi * index / 48
        cx = 256 + 174 * math.cos(angle)
        cy = 256 + 174 * math.sin(angle)
        radius = 3.5 if index % 4 == 0 else 2.5
        draw.ellipse(box(cx - radius, cy - radius, cx + radius, cy + radius),
                     fill=(36, 68, 94, 255))

    # Four short reticle arms frame the cube without obscuring its edges.
    cyan = (85, 214, 236, 255)
    draw.line([px(256), px(100), px(256), px(168)], fill=cyan, width=px(12))
    draw.line([px(256), px(344), px(256), px(412)], fill=cyan, width=px(12))
    draw.line([px(100), px(256), px(168), px(256)], fill=cyan, width=px(12))
    draw.line([px(344), px(256), px(412), px(256)], fill=cyan, width=px(12))

    # A soft halo gives the central wireframe contrast against dark game scenes.
    cube = [(256, 151), (359, 210), (359, 329), (256, 388),
            (153, 329), (153, 210), (256, 151)]
    glow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow)
    glow_draw.line([(px(x), px(y)) for x, y in cube],
                   fill=(54, 169, 255, 220), width=px(16), joint="curve")
    glow_draw.line([(px(153), px(210)), (px(256), px(270)), (px(359), px(210))],
                   fill=(118, 242, 238, 220), width=px(14), joint="curve")
    glow_draw.line([(px(256), px(270)), (px(256), px(388))],
                   fill=(80, 202, 242, 220), width=px(14))
    image = Image.alpha_composite(image, glow.filter(ImageFilter.GaussianBlur(px(7))))

    draw = ImageDraw.Draw(image)
    draw.line([(px(x), px(y)) for x, y in cube],
              fill=(118, 242, 238, 255), width=px(12), joint="curve")
    draw.line([(px(153), px(210)), (px(256), px(270)), (px(359), px(210))],
              fill=(118, 242, 238, 255), width=px(12), joint="curve")
    draw.line([(px(256), px(270)), (px(256), px(388))],
              fill=(54, 169, 255, 255), width=px(12))

    # The warm focal dot and sweep accent keep the logo visually balanced.
    draw.ellipse(box(358, 113, 382, 137), fill=(255, 189, 105, 255),
                 outline=(17, 27, 50, 255), width=px(6))
    draw.ellipse(box(366, 121, 374, 129), fill=(255, 243, 215, 255))
    draw.arc(box(96, 96, 416, 416), start=298, end=328,
             fill=(255, 189, 105, 255), width=px(8))

    icon = image.resize((SOURCE_SIZE, SOURCE_SIZE), Image.Resampling.LANCZOS)
    ASSETS.mkdir(parents=True, exist_ok=True)
    icon.save(ASSETS / "mcp-esp-icon.png", optimize=True)
    icon.save(ASSETS / "mcp-esp-icon.ico", format="ICO",
              sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                     (64, 64), (128, 128), (256, 256)])


if __name__ == "__main__":
    generate()
