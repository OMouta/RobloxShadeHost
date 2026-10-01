from tkinter import Tk, filedialog, messagebox
from PIL import Image, ImageOps

OUTPUT_WIDTH = 1800
OUTPUT_HEIGHT = 600

COLUMNS = 3
ROWS = 2

CELL_WIDTH = OUTPUT_WIDTH // COLUMNS   # 600
CELL_HEIGHT = OUTPUT_HEIGHT // ROWS    # 300

MAX_IMAGES = COLUMNS * ROWS


def main():
    root = Tk()
    root.withdraw()

    image_paths = filedialog.askopenfilenames(
        title=f"Select up to {MAX_IMAGES} PNG images",
        filetypes=[("PNG images", "*.png")],
    )

    if not image_paths:
        return

    if len(image_paths) > MAX_IMAGES:
        messagebox.showerror(
            "Too many images",
            f"Please select at most {MAX_IMAGES} images."
        )
        return

    canvas = Image.new("RGB", (OUTPUT_WIDTH, OUTPUT_HEIGHT), (0, 0, 0))

    for index, path in enumerate(image_paths):
        with Image.open(path) as image:
            image = image.convert("RGB")

            # Crop to fill a 600x300 tile without stretching.
            fitted = ImageOps.fit(
                image,
                (CELL_WIDTH, CELL_HEIGHT),
                method=Image.Resampling.LANCZOS,
                centering=(0.5, 0.5),
            )

            column = index % COLUMNS
            row = index // COLUMNS

            x = column * CELL_WIDTH
            y = row * CELL_HEIGHT

            canvas.paste(fitted, (x, y))

    output_path = filedialog.asksaveasfilename(
        title="Save grid",
        defaultextension=".png",
        initialfile="unishade-banner.png",
        filetypes=[("PNG image", "*.png")],
    )

    if not output_path:
        return

    canvas.save(output_path, "PNG", optimize=True)

    messagebox.showinfo(
        "Done",
        f"Saved banner to:\n{output_path}"
    )


if __name__ == "__main__":
    main()
