from PIL import Image
import os

source_dir = r"C:/Users/user/.gemini/antigravity/brain/0164f848-9e1e-4f15-ad14-25e4343f9f8d"
dest_dir = r"c:/Users/user/Documents/qrsoloenesp/ads"

images = [
    ("ad_welcome_1769430589707.png", "ad1.jpg"),
    ("ad_promo_1769430609964.png", "ad2.jpg"),
    ("ad_instructions_1769430630770.png", "ad3.jpg")
]

for src_name, dest_name in images:
    try:
        src_path = os.path.join(source_dir, src_name)
        dest_path = os.path.join(dest_dir, dest_name)
        
        if os.path.exists(src_path):
            img = Image.open(src_path)
            rgb_img = img.convert('RGB')
            rgb_img.save(dest_path, "JPEG", quality=90)
            print(f"Converted {src_name} to {dest_path}")
        else:
            print(f"Source not found: {src_path}")
    except Exception as e:
        print(f"Error converting {src_name}: {e}")
