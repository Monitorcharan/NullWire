import os
from PIL import Image, ImageDraw

def render_nullwire_logo(size=512):
    # Create RGBA canvas
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    cx = size / 2.0
    cy = size / 2.0
    
    # Outer circle navy ring
    outer_r = size * 0.42
    inner_r = size * 0.22

    # Draw dark navy background circle
    draw.ellipse([cx - outer_r, cy - outer_r, cx + outer_r, cy + outer_r], fill=(6, 27, 68, 255))
    draw.ellipse([cx - inner_r, cy - inner_r, cx + inner_r, cy + inner_r], fill=(0, 0, 0, 0))

    # Inner cyan glow arc (top half)
    arc_box = [cx - inner_r - 2, cy - inner_r - 2, cx + inner_r + 2, cy + inner_r + 2]
    draw.arc(arc_box, start=180, end=0, fill=(0, 210, 255, 255), width=max(2, int(size * 0.015)))

    # Left Cyan waveform points
    wave_width = max(3, int(size * 0.024))
    points_l = [
        (cx - size * 0.46, cy),
        (cx - size * 0.35, cy),
        (cx - size * 0.27, cy - size * 0.22),
        (cx - size * 0.19, cy + size * 0.22),
        (cx - size * 0.11, cy - size * 0.12),
        (cx - size * 0.05, cy),
    ]
    for i in range(len(points_l) - 1):
        draw.line([points_l[i], points_l[i+1]], fill=(0, 210, 255, 255), width=wave_width, joint="round")
        draw.ellipse([points_l[i][0] - wave_width/2, points_l[i][1] - wave_width/2, points_l[i][0] + wave_width/2, points_l[i][1] + wave_width/2], fill=(0, 210, 255, 255))

    # Right Purple waveform points
    points_r = [
        (cx + size * 0.05, cy),
        (cx + size * 0.11, cy),
        (cx + size * 0.19, cy - size * 0.20),
        (cx + size * 0.27, cy + size * 0.20),
        (cx + size * 0.35, cy - size * 0.10),
        (cx + size * 0.46, cy),
    ]
    for i in range(len(points_r) - 1):
        draw.line([points_r[i], points_r[i+1]], fill=(168, 85, 247, 255), width=wave_width, joint="round")
        draw.ellipse([points_r[i][0] - wave_width/2, points_r[i][1] - wave_width/2, points_r[i][0] + wave_width/2, points_r[i][1] + wave_width/2], fill=(168, 85, 247, 255))

    return img

def main():
    base_dir = r"c:\Users\cteja\Desktop\NullWire"
    logo_512 = render_nullwire_logo(512)

    # 1. Save Windows ICO with multiple resolutions
    ico_sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)]
    
    sender_ico_path = os.path.join(base_dir, "sender", "app.ico")
    installer_ico_path = os.path.join(base_dir, "installer", "app.ico")
    
    logo_512.save(sender_ico_path, format="ICO", sizes=ico_sizes)
    logo_512.save(installer_ico_path, format="ICO", sizes=ico_sizes)
    print("Generated Windows icon:", sender_ico_path)

    # 2. Save Android mipmaps
    android_res = os.path.join(base_dir, "receiver", "app", "src", "main", "res")
    mipmap_configs = [
        ("mipmap-mdpi", 48),
        ("mipmap-hdpi", 72),
        ("mipmap-xhdpi", 96),
        ("mipmap-xxhdpi", 144),
        ("mipmap-xxxhdpi", 192),
    ]

    for folder, dim in mipmap_configs:
        folder_path = os.path.join(android_res, folder)
        os.makedirs(folder_path, exist_ok=True)
        
        resized = logo_512.resize((dim, dim), Image.Resampling.LANCZOS)
        
        # Square launcher icon
        resized.save(os.path.join(folder_path, "ic_launcher.png"), format="PNG")
        
        # Round launcher icon
        round_img = Image.new("RGBA", (dim, dim), (0, 0, 0, 0))
        mask = Image.new("L", (dim, dim), 0)
        draw_mask = ImageDraw.Draw(mask)
        draw_mask.ellipse([0, 0, dim, dim], fill=255)
        round_img.paste(resized, (0, 0), mask=mask)
        round_img.save(os.path.join(folder_path, "ic_launcher_round.png"), format="PNG")

    print("Generated Android mipmap icons.")

if __name__ == "__main__":
    main()
