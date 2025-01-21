import base64
import os

def txt_to_png():
    # 获取当前目录下所有文件
    files = os.listdir('.')
    
    for file in files:
        if file.endswith('.txt'):
            # 读取Base64编码的文本文件
            with open(file, "r") as text_file:
                encoded_string = text_file.read()
            
            # 生成对应的PNG文件名
            png_filename = os.path.splitext(file)[0] + '.png'
            
            # 解码并保存为PNG图片
            with open(png_filename, "wb") as image_file:
                image_file.write(base64.b64decode(encoded_string))
            
            print(f"Converted {file} to {png_filename}")

if __name__ == "__main__":
    txt_to_png()
