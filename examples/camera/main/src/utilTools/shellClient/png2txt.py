import base64
import os

def png_to_txt():
    # 获取当前目录下所有文件
    files = os.listdir('.')
    
    for file in files:
        if file.endswith('.png'):
            # 读取PNG图片
            with open(file, "rb") as image_file:
                encoded_string = base64.b64encode(image_file.read()).decode('utf-8')
            
            # 生成对应的TXT文件名
            txt_filename = os.path.splitext(file)[0] + '.txt'
            
            # 将Base64编码的字符串保存到文本文件
            with open(txt_filename, "w") as text_file:
                text_file.write(encoded_string)
            
            print(f"Converted {file} to {txt_filename}")

if __name__ == "__main__":
    png_to_txt()
